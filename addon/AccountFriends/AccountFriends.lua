-- AccountFriends: account friends from mod-account-friends in the stock Friends window.
--
-- The 3.3.5 client already has the whole Real ID interface (blue friend rows, the toast, the
-- pending requests tab, Battle.net whispers in chat), but on a private server its Battle.net
-- functions return nothing. This addon answers those functions from the friend list the server
-- sends, and raises the client's own Battle.net events, so the stock code (and any addon that
-- skins it) draws account friends exactly like Real ID friends.
--
-- Until the server has sent a friend list, every replaced function passes through to the
-- original, so on a server without the module nothing changes.
--
-- Typed /cast and other secure slash commands stay safe: the chat box runs them before it
-- reaches any of the functions replaced here.

local P = AccountFriendsProtocol
local store = P.NewStore()
local selected = 0
local lineID = 900000000  -- far above the client's own chat line ids
local helloTries = 0

local function send(body)
    SendAddonMessage(P.PREFIX, body, "WHISPER", UnitName("player"))
end

local function system(text)
    local info = ChatTypeInfo["SYSTEM"]
    DEFAULT_CHAT_FRAME:AddMessage(text, info.r, info.g, info.b)
end

local function friendAt(index)
    return P.Sorted(store)[index]
end

local function friendBy(pid)
    pid = tonumber(pid)
    if not pid then
        return nil
    end
    P.Sorted(store)  -- display names
    return store.friends[pid]
end

local function className(class)
    local token = P.CLASS_TOKENS[class]
    return token and LOCALIZED_CLASS_NAMES_MALE and LOCALIZED_CLASS_NAMES_MALE[token] or ""
end

-- Raises an event on every frame that listens for it: the client never raises Battle.net
-- events here, so these reach the stock frames and other addons only once each.
-- Handlers written the old way, and ChatFrame_MessageEventHandler for arg13 (the presence id),
-- read the globals event and arg1..arg13 that the client sets for a real event; they're set
-- the same way here and put back afterwards. Returns how many handlers ran without an error.
local ARGS = 13
local function fire(event, ...)
    local saved = { event = _G.event }
    local args = { ... }
    for i = 1, ARGS do
        saved[i] = _G["arg" .. i]
    end
    _G.event = event
    for i = 1, ARGS do
        _G["arg" .. i] = args[i]
    end

    local reached = 0
    local frame = EnumerateFrames()
    while frame do
        if frame:IsEventRegistered(event) then
            local handler = frame:GetScript("OnEvent")
            if handler then
                local ok, err = pcall(handler, frame, event, ...)
                if ok then
                    reached = reached + 1
                else
                    geterrorhandler()(err)
                end
            end
        end
        frame = EnumerateFrames(frame)
    end

    _G.event = saved.event
    for i = 1, ARGS do
        _G["arg" .. i] = saved[i]
    end
    return reached
end

local function nextLine()
    lineID = lineID + 1
    return lineID
end

-- The arguments of a Battle.net chat event: arg2 is the sender, arg13 the presence id.
local function chat(event, text, f)
    return fire(event, text, f.display, "", "", "", "", 0, 0, "", 0, nextLine(), "", f.pid)
end

---------------------------------------------------------------------------------------------
-- What the stock code asks for.

local function friendInfo(f)
    if not f then
        return nil
    end
    local toonName, toonID, client
    if f.state == "W" then
        toonName, toonID, client = f.toon.name, f.pid, BNET_CLIENT_WOW
    end
    local afk = f.toon and f.toon.afk
    local dnd = f.toon and f.toon.dnd
    -- Shown through BATTLENET_NAME_FORMAT "%1$s%2$s" as Name#1234.
    return f.pid, f.name, "#" .. f.code, toonName, toonID, client, P.Online(f), f.lastOnline, afk or false, dnd or false,
        f.broadcast, f.note, true, f.broadcastTime
end

local function toonInfo(f)
    if not f then
        return nil
    end
    if f.state == "W" then
        local t = f.toon
        -- PLAYER_FACTION_GROUP: 0 Horde, 1 Alliance; the server sends 0 Alliance, 1 Horde.
        return true, t.name, BNET_CLIENT_WOW, GetRealmName(), t.team == 1 and 0 or 1, t.race, className(t.class),
            t.guild, t.zone, t.level, t.zone
    end
    return nil
end

local function canCooperate(f)
    if not f or f.state ~= "W" then
        return false
    end
    -- Flag 1: this server lets groups cross factions.
    if store.self and store.self.flags % 2 == 1 then
        return true
    end
    local mine = UnitFactionGroup("player") == "Horde" and 1 or 0
    return f.toon.team == mine
end

local shim = {}

function shim.BNFeaturesEnabled() return true end
function shim.BNFeaturesEnabledAndConnected() return true end
function shim.BNConnected() return true end

function shim.BNGetInfo()
    local s = store.self
    return s and s.pid, s and s.name, s and s.broadcast or "", UnitIsAFK("player") == 1, UnitIsDND("player") == 1
end

function shim.BNSetAFK() end
function shim.BNSetDND() end

function shim.BNGetNumFriends()
    local list = P.Sorted(store)
    return #list, store.numOnline
end

function shim.BNGetFriendInfo(index) return friendInfo(friendAt(index)) end
function shim.BNGetFriendInfoByID(pid) return friendInfo(friendBy(pid)) end
function shim.BNGetToonInfo(toonID) return toonInfo(friendBy(toonID)) end

function shim.BNGetNumFriendToons(index)
    local f = friendAt(index)
    return f and P.Online(f) and 1 or 0
end

function shim.BNGetFriendToonInfo(index, toonIndex)
    if toonIndex == 1 then
        return toonInfo(friendAt(index))
    end
end

function shim.BNGetSelectedFriend() return selected end
function shim.BNSetSelectedFriend(index) selected = index or 0 end

function shim.BNGetCustomMessageTable(t)
    t = t or {}
    for k in pairs(t) do
        t[k] = nil
    end
    local online, offline = 0, 0
    for i, f in ipairs(P.Sorted(store)) do
        if f.broadcast ~= "" then
            t[i] = true
            if P.Online(f) then
                online = online + 1
            else
                offline = offline + 1
            end
        end
    end
    return t, online, offline
end

function shim.BNSetCustomMessage(text) send(P.Broadcast(text)) end
function shim.BNSetFriendNote(pid, note) send(P.Note(pid, note)) end
function shim.BNRemoveFriend(pid) send(P.Remove(pid)) end
function shim.BNSendFriendInvite(tag, message) send(P.Add(tag, message)) end
function shim.BNSendFriendInviteByID() end

function shim.BNGetNumFriendInvites()
    local n = 0
    for _ in pairs(store.requests) do
        n = n + 1
    end
    return n
end

-- The pending list shows givenName..surname; the number helps tell people apart.
function shim.BNGetFriendInviteInfo(index)
    local r = P.SortedRequests(store)[index]
    if r then
        return r.id, r.name, "#" .. r.code, r.message, r.time
    end
end

function shim.BNAcceptFriendInvite(id) send(P.Accept(id)) end
function shim.BNDeclineFriendInvite(id) send(P.Decline(id)) end

-- No blocking or reporting: removing a friend or declining a request is the way out.
function shim.BNSetBlocked() end
function shim.BNSetToonBlocked() end
function shim.BNIsToonBlocked() return false end
function shim.BNGetNumBlocked() return 0 end
function shim.BNGetNumBlockedToons() return 0 end
function shim.BNGetBlockedInfo() end
function shim.BNGetBlockedToonInfo() end
function shim.BNReportPlayer() end
function shim.BNGetNumFOF() return 0, 0 end
function shim.BNGetFOFInfo() end
function shim.BNRequestFOFInfo() end

function shim.BNIsSelf(pid)
    return store.self ~= nil and tonumber(pid) == store.self.pid
end

function shim.BNIsFriend(pid)
    return friendBy(pid) ~= nil
end

function shim.BNSendWhisper(pid, text)
    local f = friendBy(pid)
    if not f or not P.Online(f) then
        system(format(BN_UNABLE_TO_RESOLVE_NAME, f and f.display or tostring(pid)))
        return
    end
    text = text:sub(1, P.Cut(text, P.MAX_WHISPER))
    -- The "To ..." line comes once the server has sent it (an I line), so a refused whisper
    -- shows only the server's reason.
    for _, body in ipairs(P.EncodeWhisper(f.pid, text)) do
        send(body)
    end
end

-- These two also answer for names and characters that aren't account friends, so anything
-- not ours goes to the original.
local fallthrough = {}

-- A whisper target typed or clicked as a name: only the full Name#1234.
function fallthrough.GetAutoCompletePresenceID(original, name, ...)
    if type(name) == "string" and name:find("#", 1, true) then
        local wanted = name:gsub("^%s+", ""):gsub("%s+$", ""):lower()
        for _, f in ipairs(P.Sorted(store)) do
            if f.display:lower() == wanted then
                return f.pid
            end
        end
    end
    if original then
        return original(name, ...)
    end
end

function fallthrough.CanCooperateWithToon(original, toonID, ...)
    local f = friendBy(toonID)
    if f then
        return canCooperate(f)
    end
    if original then
        return original(toonID, ...)
    end
end

-- Before the first friend list (or with no module on the server) the originals answer.
local function install(name, fn, passOriginal)
    local original = _G[name]
    _G[name] = function(...)
        if store.connected then
            if passOriginal then
                return fn(original, ...)
            end
            return fn(...)
        end
        if original then
            return original(...)
        end
    end
end

for name, fn in pairs(shim) do
    install(name, fn, false)
end
for name, fn in pairs(fallthrough) do
    install(name, fn, true)
end

---------------------------------------------------------------------------------------------
-- The add-friend dialog takes tags (Name#1234) where Real ID took e-mail addresses.

local originalAccept = AddFriendFrame_Accept
function AddFriendFrame_Accept()
    local name = AddFriendNameEditBox:GetText()
    if store.connected and name:find("#", 1, true) then
        BNSendFriendInvite(name, AddFriendNoteEditBox:GetText())
        StaticPopupSpecial_Hide(AddFriendFrame)
        return
    end
    return originalAccept()
end

local originalTextChanged = AddFriendNameEditBox_OnTextChanged
function AddFriendNameEditBox_OnTextChanged(self, userInput)
    originalTextChanged(self, userInput)
    if store.connected and AddFriendFrame.BNconnected and self:GetText():find("#", 1, true) then
        AddFriendEntryFrame_Expand()
    end
end

hooksecurefunc("AddFriendFrame_ShowEntry", function()
    if not store.connected then
        return
    end
    AddFriendEntryFrameLeftTitle:SetText("Account Friend")
    if store.self and store.self.pid then
        AddFriendEntryFrameLeftDescription:SetText("Their friend tag, like Name#1234")
    else
        AddFriendEntryFrameLeftDescription:SetText("Pick your own tag first:\n.friends tag <name>")
        AddFriendEntryFrameLeftDescription:SetTextColor(1, 0.82, 0)
    end
    AddFriendNameEditBoxFill:SetText("Name#1234 or character name")
end)

---------------------------------------------------------------------------------------------
-- The stock pieces that only exist for Real ID, switched on with the first friend list.

local interfaceReady = false
local tagButton

local function ownTag()
    local s = store.self
    return s and s.pid and (s.name .. "#" .. s.code)
end

local function updateTagButton()
    if tagButton then
        tagButton:SetText(ownTag() or "Pick a friend tag")
        tagButton:SetWidth(tagButton:GetTextWidth() + 4)
    end
end

-- Your own tag beside the Friends, Ignore and Pending tabs: click to put it in chat.
local function createTagButton()
    tagButton = CreateFrame("Button", "AccountFriendsTagButton", FriendsTabHeader)
    tagButton:SetHeight(20)
    tagButton:SetPoint("LEFT", FriendsTabHeaderTab3, "RIGHT", 4, 0)
    tagButton:SetNormalFontObject(GameFontNormalSmall)
    tagButton:SetHighlightFontObject(GameFontHighlightSmall)
    tagButton:SetScript("OnShow", updateTagButton)
    tagButton:SetScript("OnClick", function()
        -- Not ChatEdit_InsertLink: with the macro window open it writes into the macro.
        local text = ownTag() or ".friends tag "
        local box = ChatEdit_GetActiveWindow()
        if box then
            box:Insert(text)
        else
            ChatFrame_OpenChat(text)
        end
    end)
    tagButton:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_BOTTOM")
        if ownTag() then
            GameTooltip:SetText("Your friend tag")
            GameTooltip:AddLine("Click to put it in chat. Friends add you with it.", 1, 1, 1)
        else
            GameTooltip:SetText("No friend tag yet")
            GameTooltip:AddLine("Click to choose one. Friends add you with it.", 1, 1, 1)
        end
        GameTooltip:Show()
    end)
    tagButton:SetScript("OnLeave", function()
        GameTooltip:Hide()
    end)
end

local function dropMenuEntries(menu, drop)
    if not menu then
        return
    end
    for i = #menu, 1, -1 do
        if drop[menu[i]] then
            table.remove(menu, i)
        end
    end
end

local function enableInterface()
    if interfaceReady then
        return
    end
    interfaceReady = true

    -- Names show as "Pawster#1234": the given name and "#1234" as the surname.
    BATTLENET_NAME_FORMAT = "%1$s%2$s"
    for _, key in ipairs({ "BN_INLINE_TOAST_FRIEND_ADDED", "BN_INLINE_TOAST_FRIEND_REMOVED", "REMOVE_FRIEND_CONFIRMATION" }) do
        if type(_G[key]) == "string" then
            _G[key] = _G[key]:gsub("a Real ID", "an account"):gsub("Real ID", "account")
        end
    end

    -- The first-time Real ID privacy notes don't apply.
    SetCVar("addFriendInfoShown", "1")
    SetCVar("pendingInviteInfoShown", "1")

    -- FriendsFrame hid the pending requests tab at load, when Battle.net was still off.
    if FriendsTabHeaderTab3 then
        FriendsTabHeaderTab3:Show()
        createTagButton()
    end

    -- Battle.net features the server doesn't have: conversations, friends of friends,
    -- blocking, reporting. Targeting by name is protected and would be blocked from here.
    local unsupported = { CREATE_CONVERSATION_WITH = true, BN_TARGET = true, BN_VIEW_FRIENDS = true,
        BLOCK_COMMUNICATION = true, BN_REPORT = true }
    dropMenuEntries(UnitPopupMenus["BN_FRIEND"], unsupported)
    dropMenuEntries(UnitPopupMenus["BN_FRIEND_OFFLINE"], unsupported)
    for i = 1, PENDING_INVITES_TO_DISPLAY or 0 do
        for _, part in ipairs({ "ReportButton", "BlockButton" }) do
            local button = _G["FriendsFramePendingButton" .. i .. part]
            if button then
                button:Hide()
            end
        end
    end

    -- The toast settings panel registers itself only when Battle.net is on at load.
    local panel = InterfaceOptionsBattlenetPanel
    if panel and not panel.name and BattlenetPanelOptions and InterfaceOptionsPanel_OnLoad then
        panel.name = "Account Friends"
        panel.options = BattlenetPanelOptions
        local ok, err = pcall(function()
            InterfaceOptionsPanel_OnLoad(panel)
            -- Its controls read their settings on PLAYER_ENTERING_WORLD, which has passed.
            BlizzardOptionsPanel_OnEvent(panel, "PLAYER_ENTERING_WORLD")
        end)
        if not ok then
            geterrorhandler()(err)
        end
    end

    -- Both are bound at load to the functions replaced above.
    AddFriendEntryFrameAcceptButton:SetScript("OnClick", AddFriendFrame_Accept)
    AddFriendNameEditBox:SetScript("OnTextChanged", AddFriendNameEditBox_OnTextChanged)
end

---------------------------------------------------------------------------------------------
-- Server messages turned into the client's Battle.net events.

local function alert(kind, f)
    if f then
        chat("CHAT_MSG_BN_INLINE_TOAST_ALERT", kind, f)
    end
end

local function handle(events)
    for _, e in ipairs(events) do
        local kind, a, b = e[1], e[2], e[3]
        if kind == "synced" then
            enableInterface()
            updateTagButton()
            if a then
                fire("BN_CONNECTED")
            end
            fire("BN_FRIEND_LIST_SIZE_CHANGED")
            fire("BN_CUSTOM_MESSAGE_LOADED")
            local pending = BNGetNumFriendInvites()
            if pending > 0 then
                fire("BN_FRIEND_INVITE_LIST_INITIALIZED", pending)
            end
        elseif kind == "self" then
            updateTagButton()
            fire("BN_CUSTOM_MESSAGE_CHANGED")
        elseif kind == "info" then
            fire("BN_FRIEND_INFO_CHANGED")
        elseif kind == "added" then
            fire("BN_FRIEND_LIST_SIZE_CHANGED")
            alert("FRIEND_ADDED", friendBy(a))
        elseif kind == "removed" then
            fire("BN_FRIEND_LIST_SIZE_CHANGED")
        elseif kind == "inviteAdded" then
            fire("BN_FRIEND_INVITE_ADDED", a)
            fire("CHAT_MSG_BN_INLINE_TOAST_ALERT", "FRIEND_REQUEST", "", "", "", "", "", 0, 0, "", 0, nextLine(), "", 0)
        elseif kind == "inviteRemoved" then
            fire("BN_FRIEND_INVITE_REMOVED")
        elseif kind == "online" then
            fire("BN_FRIEND_ACCOUNT_ONLINE", a)
            alert("FRIEND_ONLINE", friendBy(a))
        elseif kind == "offline" then
            fire("BN_FRIEND_ACCOUNT_OFFLINE", a)
            alert("FRIEND_OFFLINE", friendBy(a))
        elseif kind == "broadcast" then
            local f = friendBy(a)
            fire("BN_CUSTOM_MESSAGE_CHANGED", a)
            if f and f.broadcast ~= "" then
                chat("CHAT_MSG_BN_INLINE_TOAST_BROADCAST", f.broadcast, f)
            end
        elseif kind == "sent" then
            local f = friendBy(a)
            if f then
                chat("CHAT_MSG_BN_WHISPER_INFORM", b, f)
            end
        elseif kind == "whisper" then
            local f = friendBy(a)
            if f and chat("CHAT_MSG_BN_WHISPER", b, f) == 0 then
                -- No chat window listens to Battle.net whispers: show it anyway.
                local info = ChatTypeInfo["BN_WHISPER"]
                DEFAULT_CHAT_FRAME:AddMessage(format(CHAT_BN_WHISPER_GET, f.display) .. b, info.r, info.g, info.b)
            end
        end
    end
end

-- Hello once per UI load; again after 10 and 20 seconds if no list came (a busy login).
local function hello()
    helloTries = helloTries + 1
    send(P.Hello())
end

local frame = CreateFrame("Frame")
local waited = 0
frame:RegisterEvent("PLAYER_ENTERING_WORLD")
frame:RegisterEvent("CHAT_MSG_ADDON")
frame:SetScript("OnEvent", function(self, event, prefix, message, channel, sender)
    if event == "PLAYER_ENTERING_WORLD" then
        self:UnregisterEvent("PLAYER_ENTERING_WORLD")
        hello()
        self:SetScript("OnUpdate", function(_, elapsed)
            waited = waited + elapsed
            if store.connected or helloTries >= 3 then
                self:SetScript("OnUpdate", nil)
            elseif waited >= 10 then
                waited = 0
                hello()
            end
        end)
    elseif prefix == P.PREFIX and channel == "WHISPER" and sender == UnitName("player") then
        handle(P.Apply(store, message))
    end
end)
