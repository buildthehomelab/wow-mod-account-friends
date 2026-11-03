-- Runs under plain Lua 5.1: lua5.1 tests/protocol.lua
dofile("addon/AccountFriends/Protocol.lua")
local P = AccountFriendsProtocol

local function apply(store, ...)
    local all = {}
    for _, body in ipairs({ ... }) do
        for _, e in ipairs(P.Apply(store, body)) do
            all[#all + 1] = e
        end
    end
    return all
end

local function kinds(events)
    local out = {}
    for _, e in ipairs(events) do
        out[#out + 1] = e[1]
    end
    return table.concat(out, ",")
end

-- Split keeps the rest in the last field.
local f = P.Split("A\tPawster#1234\thi\tthere", 3)
assert(#f == 3 and f[3] == "hi\tthere")
assert(#P.Split("H", 3) == 1)

-- Cut never splits a UTF-8 character.
local accents = "\195\169\195\169"
assert(P.Cut(accents, 3) == 2 and P.Cut(accents, 4) == 4 and P.Cut("abc", 9) == 3)

-- Whispers: pieces of at most CHUNK bytes, w then a final W, joined again on the other side.
local text = string.rep("a", 219) .. accents .. string.rep("b", 30)
local bodies = P.EncodeWhisper(7, text)
assert(#bodies == 2 and bodies[1]:sub(1, 4) == "w\t7\t" and bodies[2]:sub(1, 4) == "W\t7\t")
for _, b in ipairs(bodies) do
    assert(#P.PREFIX + 1 + #b <= 254, "addon message too long")
end
assert(#P.EncodeWhisper(7, "hi") == 1 and P.EncodeWhisper(7, "hi")[1] == "W\t7\thi")

-- Outgoing bodies strip tabs, pipes and control characters from free text.
assert(P.Add(" Pawster#1234 ", "hi\tthere|r") == "A\tPawster#1234\thitherer")
assert(P.Note(3, "  note\n ") == "N\t3\tnote")
assert(P.Broadcast(string.rep("x", 200)) == "C\t" .. string.rep("x", 127))
assert(P.Accept(5) == "Y\t5" and P.Decline(5) == "D\t5" and P.Remove(9) == "X\t9" and P.Hello() == "H\t1")

-- A full sync: no events until E, then one "synced" (first = true).
local store = P.NewStore()
local events = apply(store,
    "Y",
    "S\t1\tMe\t1111\t1\t0\t",
    "F\t2\tPawster\t1234\t1700000000\t1700000100\tat the inn",
    "N\t2\tbest panda",
    "T\t2\tW\tBirta\t60\t3\tNight Elf\t0\t1\tMolten Core\tThe Pandas",
    "F\t3\tzed\t2222\t1690000000\t0\t",
    "T\t3\t-",
    "F\t4\tAnna\t3333\t0\t0\t",
    "T\t4\tW\tAnnie\t10\t8\tHuman\t0\t0\tStormwind\t",
    "R\t10\tNewbie\t4444\t1700000200\tlet's be friends",
    "E")
assert(kinds(events) == "synced" and events[1][2] == true, kinds(events))
assert(store.connected and not store.syncing)
assert(store.self.pid == 1 and store.self.name == "Me" and store.self.flags == 1)

-- Order: online by name, then offline.
local list = P.Sorted(store)
assert(#list == 3 and store.numOnline == 2)
assert(list[1].name == "Anna" and list[2].name == "Pawster" and list[3].name == "zed")
local paw = store.friends[2]
assert(paw.index == 2 and paw.display == "Pawster#1234" and paw.note == "best panda" and paw.broadcast == "at the inn")
assert(paw.toon.name == "Birta" and paw.toon.level == 60 and paw.toon.class == 3 and paw.toon.race == "Night Elf")
assert(paw.toon.afk and not paw.toon.dnd and paw.toon.zone == "Molten Core" and paw.toon.guild == "The Pandas")
assert(store.friends[3].state == "-" and not store.friends[3].toon)
assert(P.SortedRequests(store)[1].message == "let's be friends")

-- A second sync (a /reload) isn't "first".
events = apply(store, "Y", "S\t1\tMe\t1111\t1\t0\t", "E")
assert(kinds(events) == "synced" and events[1][2] == false)
assert(next(store.friends) == nil)

-- Incremental changes.
events = apply(store, "F\t2\tPawster\t1234\t0\t0\t", "T\t2\tW\tBirta\t60\t3\tNight Elf\t0\t0\tOrgrimmar\t")
assert(kinds(events) == "added,info", kinds(events))
events = apply(store, "O\t2", "B\t2", "Z\t2")
assert(kinds(events) == "online,broadcast,offline" and events[1][2] == 2)
events = apply(store, "R\t11\tX\t5555\t0\t", "R\t11\tX\t5555\t0\t", "Q\t11", "Q\t11")
assert(kinds(events) == "inviteAdded,inviteRemoved", kinds(events))
events = apply(store, "X\t2", "X\t2")
assert(kinds(events) == "removed" and next(store.friends) == nil)

-- Names always carry their numbers (a bare name could be a character's).
apply(store, "F\t5\tSam\t1000\t0\t0\t", "F\t6\tsam\t2000\t0\t0\t", "F\t7\tTom\t3000\t0\t0\t")
P.Sorted(store)
assert(store.friends[5].display == "Sam#1000" and store.friends[6].display == "sam#2000" and store.friends[7].display == "Tom#3000")

-- Whisper pieces join; a piece for a stranger or an oversized run is dropped.
events = apply(store, "w\t5\t" .. string.rep("a", 220), "W\t5\tbc")
assert(kinds(events) == "whisper" and events[1][2] == 5 and events[1][3] == string.rep("a", 220) .. "bc")
assert(kinds(apply(store, "W\t99\thi")) == "")
events = apply(store, "w\t5\t" .. string.rep("a", 200), "w\t5\t" .. string.rep("a", 200), "W\t5\tx")
assert(kinds(events) == "whisper" and events[1][3] == "x", "an oversized run is dropped")
assert(kinds(apply(store, "W\t5\t")) == "")

-- Our own whisper, echoed by the server, comes out as "sent" and doesn't mix with incoming pieces.
events = apply(store, "w\t5\tfrom ", "i\t5\tto ", "I\t5\tthem", "W\t5\tthem")
assert(kinds(events) == "sent,whisper" and events[1][3] == "to them" and events[2][3] == "from them", kinds(events))

-- Garbage is ignored.
assert(kinds(apply(store, "", "?", "T\t999\tW", "N\t999\tx")) == "")

print("protocol tests passed")
