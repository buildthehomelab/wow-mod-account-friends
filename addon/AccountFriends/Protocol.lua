-- AccountFriends protocol and friend list state: no WoW API here, so tests/protocol.lua can run
-- it under plain Lua 5.1.
--
-- The addon whispers itself with prefix ACFR; mod-account-friends swallows those whispers and
-- answers the same way. Fields are tab-separated, free text always last. See the comment at the
-- top of src/AccountFriends.cpp for the full list.

AccountFriendsProtocol = {}
local P = AccountFriendsProtocol

P.PREFIX = "ACFR"
P.VERSION = 1
-- An addon message holds 254 bytes of prefix, tab and body; "W\t<id>\t" takes up to 12.
P.CHUNK = 220
P.MAX_WHISPER = 255

-- Splits at tabs into at most `max` fields; the last keeps the rest.
function P.Split(text, max)
    local fields = {}
    local start = 1
    while #fields + 1 < max do
        local tab = text:find("\t", start, true)
        if not tab then
            break
        end
        fields[#fields + 1] = text:sub(start, tab - 1)
        start = tab + 1
    end
    fields[#fields + 1] = text:sub(start)
    return fields
end

-- The longest prefix of at most `max` bytes that doesn't cut a UTF-8 character.
function P.Cut(text, max)
    if #text <= max then
        return #text
    end
    local cut = max
    while cut > 0 do
        local byte = text:byte(cut + 1)
        if byte < 0x80 or byte >= 0xC0 then
            break
        end
        cut = cut - 1
    end
    return cut
end

-- Bodies for a whisper: "w" pieces and a final "W".
function P.EncodeWhisper(pid, text)
    local bodies = {}
    repeat
        local cut = P.Cut(text, P.CHUNK)
        local piece = text:sub(1, cut)
        text = text:sub(cut + 1)
        bodies[#bodies + 1] = (text == "" and "W" or "w") .. "\t" .. pid .. "\t" .. piece
    until text == ""
    return bodies
end

local function strip(text, max)
    text = (text or ""):gsub("[%c|]", ""):gsub("^%s+", ""):gsub("%s+$", "")
    return text:sub(1, P.Cut(text, max))
end

function P.Hello() return "H\t" .. P.VERSION end
function P.Add(tag, message) return "A\t" .. strip(tag, 20) .. "\t" .. strip(message, 127) end
function P.Accept(id) return "Y\t" .. id end
function P.Decline(id) return "D\t" .. id end
function P.Remove(pid) return "X\t" .. pid end
function P.Note(pid, note) return "N\t" .. pid .. "\t" .. strip(note, 127) end
function P.Broadcast(text) return "C\t" .. strip(text, 127) end

-- Class ids to the tokens LOCALIZED_CLASS_NAMES_MALE uses.
P.CLASS_TOKENS = {
    [1] = "WARRIOR", [2] = "PALADIN", [3] = "HUNTER", [4] = "ROGUE", [5] = "PRIEST",
    [6] = "DEATHKNIGHT", [7] = "SHAMAN", [8] = "MAGE", [9] = "WARLOCK", [11] = "DRUID",
}

---------------------------------------------------------------------------------------------
-- State. Apply() turns server bodies into the friend list and returns what happened, as a list
-- of { kind, ... } events for the shim to turn into the client's own Battle.net events.

function P.NewStore()
    return {
        self = nil,           -- { pid, name, code, flags, broadcastTime, broadcast }
        friends = {},         -- by pid
        requests = {},        -- by request id
        connected = false,    -- a full sync has arrived
        syncing = false,
        sorted = nil,         -- list order, rebuilt when nil
        pieces = {},          -- whisper pieces by pid
        sentPieces = {},      -- pieces of our own whispers, echoed by the server
    }
end

local function friendState(f)
    return f.state == "W"
end

-- Online first, then by name, then by tag code.
function P.Sorted(store)
    if store.sorted then
        return store.sorted
    end
    local list = {}
    for _, f in pairs(store.friends) do
        list[#list + 1] = f
    end
    table.sort(list, function(a, b)
        local ao, bo = friendState(a), friendState(b)
        if ao ~= bo then
            return ao
        end
        local an, bn = a.name:lower(), b.name:lower()
        if an ~= bn then
            return an < bn
        end
        return a.code < b.code
    end)
    -- Always with the number: a bare "Bob" would also be a character's name, and whispers or
    -- /ignore meant for that character would reach the account friend instead. Character names
    -- can't hold '#'.
    store.numOnline = 0
    for i, f in ipairs(list) do
        f.index = i
        f.display = f.name .. "#" .. f.code
        if friendState(f) then
            store.numOnline = store.numOnline + 1
        end
    end
    store.sorted = list
    return list
end

function P.Online(f)
    return friendState(f)
end

-- Requests oldest first.
function P.SortedRequests(store)
    local list = {}
    for _, r in pairs(store.requests) do
        list[#list + 1] = r
    end
    table.sort(list, function(a, b) return a.id < b.id end)
    return list
end

local function number(text)
    return tonumber(text) or 0
end

local function friendOf(store, pid)
    local f = store.friends[pid]
    if not f then
        f = { pid = pid, name = "?", code = 0, lastOnline = 0, broadcastTime = 0, broadcast = "", note = "", state = "-" }
        store.friends[pid] = f
    end
    return f
end

function P.Apply(store, body)
    local op = body:sub(1, 1)
    local events = {}
    local function emit(...)
        events[#events + 1] = { ... }
    end
    -- Events of a full sync are summed up once at its end.
    local quiet = store.syncing

    if op == "Y" then
        store.syncing = true
        store.friends = {}
        store.requests = {}
        store.sorted = nil
        store.pieces = {}
        store.sentPieces = {}
    elseif op == "E" then
        local first = not store.connected
        store.syncing = false
        store.connected = true
        emit("synced", first)
    elseif op == "S" then
        local f = P.Split(body, 7)
        store.self = {
            pid = number(f[2]), name = f[3] or "", code = number(f[4]), flags = number(f[5]),
            broadcastTime = number(f[6]), broadcast = f[7] or "",
        }
        if store.self.pid == 0 then
            store.self.pid = nil
        end
        if not quiet then
            emit("self")
        end
    elseif op == "F" then
        local f = P.Split(body, 7)
        local pid = number(f[2])
        local isNew = not store.friends[pid]
        local entry = friendOf(store, pid)
        entry.name = f[3] or "?"
        entry.code = number(f[4])
        entry.lastOnline = number(f[5])
        entry.broadcastTime = number(f[6])
        entry.broadcast = f[7] or ""
        store.sorted = nil
        if not quiet then
            emit(isNew and "added" or "info", pid)
        end
    elseif op == "N" then
        local f = P.Split(body, 3)
        local entry = store.friends[number(f[2])]
        if entry then
            entry.note = f[3] or ""
            if not quiet then
                emit("info", entry.pid)
            end
        end
    elseif op == "T" then
        local f = P.Split(body, 11)
        local entry = store.friends[number(f[2])]
        if entry then
            local state = f[3] or "-"
            entry.state = state
            if state == "W" then
                local flags = number(f[9])
                entry.toon = {
                    name = f[4] or "", level = number(f[5]), class = number(f[6]), race = f[7] or "",
                    team = number(f[8]), afk = flags % 2 == 1, dnd = math.floor(flags / 2) % 2 == 1,
                    zone = f[10] or "", guild = f[11] or "",
                }
            else
                entry.toon = nil
            end
            store.sorted = nil
            if not quiet then
                emit("info", entry.pid)
            end
        end
    elseif op == "R" then
        local f = P.Split(body, 6)
        local id = number(f[2])
        local isNew = not store.requests[id]
        store.requests[id] = { id = id, name = f[3] or "?", code = number(f[4]), time = number(f[5]), message = f[6] or "" }
        if not quiet and isNew then
            emit("inviteAdded", id)
        end
    elseif op == "Q" then
        local id = number(body:sub(3))
        if store.requests[id] then
            store.requests[id] = nil
            emit("inviteRemoved", id)
        end
    elseif op == "X" then
        local pid = number(body:sub(3))
        if store.friends[pid] then
            store.friends[pid] = nil
            store.pieces[pid] = nil
            store.sorted = nil
            emit("removed", pid)
        end
    elseif op == "O" then
        emit("online", number(body:sub(3)))
    elseif op == "Z" then
        emit("offline", number(body:sub(3)))
    elseif op == "B" then
        emit("broadcast", number(body:sub(3)))
    elseif op == "w" or op == "W" or op == "i" or op == "I" then
        -- w/W a whisper from a friend, i/I our own whisper after the server sent it.
        local mine = op == "i" or op == "I"
        local buffer = mine and store.sentPieces or store.pieces
        local f = P.Split(body, 3)
        local pid = number(f[2])
        local text = (buffer[pid] or "") .. (f[3] or "")
        if op == "w" or op == "i" then
            buffer[pid] = #text <= P.MAX_WHISPER and text or nil
        else
            buffer[pid] = nil
            if store.friends[pid] and text ~= "" then
                emit(mine and "sent" or "whisper", pid, text)
            end
        end
    end
    return events
end
