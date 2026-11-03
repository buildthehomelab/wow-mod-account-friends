-- The addon's Battle.net shim against stand-ins for the 3.3.5 client API and the FrameXML bits
-- it touches. Runs under plain Lua 5.1: lua5.1 tests/shim.lua

local frames = {}
local sent = {}
local printed = {}

local Frame = {}
Frame.__index = Frame
function Frame:RegisterEvent(e) self.events[e] = true end
function Frame:UnregisterEvent(e) self.events[e] = nil end
function Frame:IsEventRegistered(e) return self.events[e] == true end
function Frame:SetScript(k, f) self.scripts[k] = f end
function Frame:GetScript(k) return self.scripts[k] end
function Frame:Hide() self.shown = false end
function Frame:Show() self.shown = true end
function Frame:SetText(t) self.text = t end
function Frame:GetText() return self.text or "" end
function Frame:SetTextColor() end
function Frame:SetTexture(t) self.texture = t end
function Frame:AddMessage(t) printed[#printed + 1] = t end

local function newFrame(name)
    local f = setmetatable({ events = {}, scripts = {}, shown = true, name = name }, Frame)
    frames[#frames + 1] = f
    if name then
        _G[name] = f
    end
    return f
end

function CreateFrame(_, name) return newFrame(name) end
function EnumerateFrames(prev)
    if not prev then
        return frames[1]
    end
    for i, f in ipairs(frames) do
        if f == prev then
            return frames[i + 1]
        end
    end
end
function geterrorhandler() return error end
function hooksecurefunc(name, hook)
    local original = _G[name]
    _G[name] = function(...)
        local r = { original(...) }
        hook(...)
        return unpack(r)
    end
end
function SendAddonMessage(prefix, text, channel, target)
    assert(#prefix + 1 + #text <= 254, "addon message too long")
    sent[#sent + 1] = { prefix = prefix, text = text, channel = channel, target = target }
end
function UnitName() return "Me" end
function UnitFactionGroup() return "Alliance" end
function UnitIsAFK() return nil end
function UnitIsDND() return nil end
function GetRealmName() return "Realm" end
function SetCVar(k, v) _G["cvar_" .. k] = v end
function StaticPopupSpecial_Hide(f) f.shown = false end
function AddFriendEntryFrame_Expand() AddFriendFrame.expanded = true end
format = string.format

ChatTypeInfo = { SYSTEM = { r = 1, g = 1, b = 0 }, BN_WHISPER = { r = 0, g = 1, b = 1 } }
LOCALIZED_CLASS_NAMES_MALE = { HUNTER = "Hunter" }
BNET_CLIENT_WOW = "WoW"
BN_UNABLE_TO_RESOLVE_NAME = "Unable to whisper '%s'."
CHAT_BN_WHISPER_GET = "%s whispers: "
BATTLENET_NAME_FORMAT = "%1$s %2$s"
BN_INLINE_TOAST_FRIEND_ADDED = "%s is now a Real ID friend."
PENDING_INVITES_TO_DISPLAY = 1
UnitPopupMenus = {
    BN_FRIEND = { "WHISPER", "POP_OUT_CHAT", "CREATE_CONVERSATION_WITH", "BN_INVITE", "BN_TARGET", "BN_SET_NOTE",
        "BN_VIEW_FRIENDS", "BLOCK_COMMUNICATION", "BN_REPORT", "BN_REMOVE_FRIEND", "CANCEL" },
    BN_FRIEND_OFFLINE = { "BN_SET_NOTE", "BN_VIEW_FRIENDS", "BN_REPORT", "BN_REMOVE_FRIEND", "CANCEL" },
}

DEFAULT_CHAT_FRAME = newFrame("ChatFrame1")
newFrame("FriendsTabHeaderTab3"):Hide()
newFrame("FriendsFramePendingButton1ReportButton")
newFrame("FriendsFramePendingButton1BlockButton")
newFrame("AddFriendFrame")
newFrame("AddFriendNameEditBox")
newFrame("AddFriendNoteEditBox")
newFrame("AddFriendNameEditBoxFill")
newFrame("AddFriendEntryFrameLeftTitle")
newFrame("AddFriendEntryFrameLeftDescription")
newFrame("AddFriendEntryFrameAcceptButton")

-- The client's own functions, as they answer without Battle.net.
function BNFeaturesEnabled() return false end
function BNConnected() return false end
function BNGetNumFriends() return 0, 0 end
function GetAutoCompletePresenceID(name) return name == "RealBN" and 777 or nil end
function CanCooperateWithToon() return "original" end
local characterFriendAdded
function AddFriendFrame_Accept() characterFriendAdded = AddFriendNameEditBox:GetText() end
function AddFriendNameEditBox_OnTextChanged() end
function AddFriendFrame_ShowEntry() end

dofile("addon/AccountFriends/Protocol.lua")
dofile("addon/AccountFriends/AccountFriends.lua")

-- Frames listening for what the shim raises.
local seen = {}
local listener = newFrame()
for _, e in ipairs({ "BN_CONNECTED", "BN_FRIEND_LIST_SIZE_CHANGED", "BN_FRIEND_INFO_CHANGED", "BN_FRIEND_ACCOUNT_ONLINE",
    "BN_FRIEND_ACCOUNT_OFFLINE", "BN_FRIEND_INVITE_ADDED", "BN_FRIEND_INVITE_LIST_INITIALIZED", "BN_CUSTOM_MESSAGE_CHANGED",
    "CHAT_MSG_BN_WHISPER", "CHAT_MSG_BN_WHISPER_INFORM", "CHAT_MSG_BN_INLINE_TOAST_ALERT" }) do
    listener:RegisterEvent(e)
end
listener:SetScript("OnEvent", function(_, event, ...)
    seen[#seen + 1] = { event = event, args = { ... } }
end)
local function lastSeen(event)
    for i = #seen, 1, -1 do
        if seen[i].event == event then
            return seen[i]
        end
    end
end

local addonFrame
for _, f in ipairs(frames) do
    if f.events.CHAT_MSG_ADDON then
        addonFrame = f
    end
end
local function server(body, sender)
    addonFrame:GetScript("OnEvent")(addonFrame, "CHAT_MSG_ADDON", "ACFR", body, "WHISPER", sender or "Me")
end

-- Before any friend list the client's own answers stand, and the addon says hello.
assert(BNFeaturesEnabled() == false and BNConnected() == false)
assert(GetAutoCompletePresenceID("RealBN") == 777)
addonFrame:GetScript("OnEvent")(addonFrame, "PLAYER_ENTERING_WORLD")
assert(#sent == 1 and sent[1].prefix == "ACFR" and sent[1].text == "H\t1" and sent[1].channel == "WHISPER" and sent[1].target == "Me")

-- Someone else's addon whisper with our prefix is ignored.
server("Y", "Stranger")
server("E", "Stranger")
assert(BNFeaturesEnabled() == false)

-- The sync.
for _, body in ipairs({
    "Y",
    "S\t1\tMe\t1111\t0\t0\t",
    "F\t2\tPawster\t1234\t1700000000\t1700000100\tat the inn",
    "T\t2\tW\tBirta\t60\t3\tNight Elf\t0\t1\tMolten Core\tThe Pandas",
    "F\t3\tHordey\t2222\t0\t0\t",
    "T\t3\tW\tGrom\t70\t1\tOrc\t1\t0\tOrgrimmar\t",
    "F\t4\tAnna\t3333\t1690000000\t0\t",
    "T\t4\tW\tAnnie\t10\t8\tHuman\t0\t2\tStormwind\t",
    "F\t5\tZed\t4444\t1690000000\t0\t",
    "T\t5\t-",
    "R\t10\tNewbie\t5555\t1700000200\thi",
    "E",
}) do
    server(body)
end
assert(BNFeaturesEnabled() == true and BNConnected() == true and BNFeaturesEnabledAndConnected() == true)
assert(lastSeen("BN_CONNECTED") and lastSeen("BN_FRIEND_LIST_SIZE_CHANGED"))
assert(lastSeen("BN_FRIEND_INVITE_LIST_INITIALIZED").args[1] == 1)
assert(not lastSeen("BN_FRIEND_ACCOUNT_ONLINE"), "no toasts for the sync itself")

-- The interface switched on.
assert(FriendsTabHeaderTab3.shown and not FriendsFramePendingButton1ReportButton.shown)
assert(BATTLENET_NAME_FORMAT == "%1$s%2$s" and BN_INLINE_TOAST_FRIEND_ADDED == "%s is now an account friend.")
assert(cvar_addFriendInfoShown == "1")
assert(table.concat(UnitPopupMenus.BN_FRIEND, ",") == "WHISPER,POP_OUT_CHAT,BN_INVITE,BN_SET_NOTE,BN_REMOVE_FRIEND,CANCEL")
assert(table.concat(UnitPopupMenus.BN_FRIEND_OFFLINE, ",") == "BN_SET_NOTE,BN_REMOVE_FRIEND,CANCEL")

-- The list: online by name (Anna, Hordey, Pawster), then Zed offline.
local total, online = BNGetNumFriends()
assert(total == 4 and online == 3)
local pid, given, surname, toonName, toonID, client, isOnline, lastOnline, afk, dnd, broadcast, note, isFriend, broadcastTime = BNGetFriendInfo(3)
assert(pid == 2 and given == "Pawster" and surname == "#1234" and toonName == "Birta" and toonID == 2 and client == "WoW")
assert(isOnline == true and afk == true and dnd == false and broadcast == "at the inn" and isFriend and broadcastTime == 1700000100)
pid, given, _, toonName, _, client, isOnline, _, afk, dnd = BNGetFriendInfo(1)
assert(pid == 4 and given == "Anna" and toonName == "Annie" and client == "WoW" and isOnline and dnd and not afk)
pid, _, _, toonName, toonID, client, isOnline, lastOnline = BNGetFriendInfo(4)
assert(pid == 5 and toonName == nil and toonID == nil and client == nil and isOnline == false and lastOnline == 1690000000)
assert(select(2, BNGetFriendInfoByID(2)) == "Pawster")

local hasFocus, name, toonClient, realm, faction, race, class, guild, zone, level, gameText = BNGetToonInfo(2)
assert(hasFocus and name == "Birta" and toonClient == "WoW" and realm == "Realm" and faction == 1 and race == "Night Elf")
assert(class == "Hunter" and guild == "The Pandas" and zone == "Molten Core" and level == 60 and gameText == "Molten Core")
assert(select(5, BNGetToonInfo(3)) == 0, "Horde is faction 0")
assert(BNGetToonInfo(5) == nil, "offline: no character")
assert(BNGetNumFriendToons(1) == 1 and BNGetNumFriendToons(4) == 0)

-- Cooperation: same faction only (no cross-faction flag); other ids go to the original.
assert(CanCooperateWithToon(2) == true and CanCooperateWithToon(3) == false and CanCooperateWithToon(5) == false)
assert(CanCooperateWithToon(999) == "original")
server("S\t1\tMe\t1111\t1\t0\t")
assert(CanCooperateWithToon(3) == true, "flag 1: groups may cross factions")
server("S\t1\tMe\t1111\t0\t0\t")

-- Names resolve to presence ids; anything else goes to the original.
assert(GetAutoCompletePresenceID("pawster#1234") == 2 and GetAutoCompletePresenceID(" Pawster#1234 ") == 2)
assert(GetAutoCompletePresenceID("Pawster") == nil, "a bare name stays a character name")
assert(GetAutoCompletePresenceID("RealBN") == 777 and GetAutoCompletePresenceID("Nobody") == nil)

-- Broadcast table lines up with list indices.
local t, onlineB, offlineB = BNGetCustomMessageTable({})
assert(t[3] == true and onlineB == 1 and offlineB == 0)

-- Requests.
assert(BNGetNumFriendInvites() == 1)
local inviteID, iname, isurname, message, sentAt = BNGetFriendInviteInfo(1)
assert(inviteID == 10 and iname == "Newbie" and isurname == "#5555" and message == "hi" and sentAt == 1700000200)

-- Actions go to the server.
sent = {}
BNAcceptFriendInvite(10); BNDeclineFriendInvite(10); BNRemoveFriend(5); BNSetFriendNote(2, "note\t"); BNSetCustomMessage("hello")
BNSendFriendInvite("Someone#1234", "hey")
local texts = {}
for _, s in ipairs(sent) do texts[#texts + 1] = s.text end
assert(table.concat(texts, "|") == "Y\t10|D\t10|X\t5|N\t2\tnote|C\thello|A\tSomeone#1234\they", table.concat(texts, "|"))

-- Whispering: pieces to the server, the INFORM line locally.
sent = {}
BNSendWhisper(2, string.rep("x", 230))
assert(#sent == 2 and sent[1].text:sub(1, 4) == "w\t2\t" and sent[2].text:sub(1, 4) == "W\t2\t")
assert(not lastSeen("CHAT_MSG_BN_WHISPER_INFORM"), "no 'To ...' line before the server sent it")
-- The server echoes it back in pieces once it went out.
server("i\t2\t" .. string.rep("x", 220))
server("I\t2\t" .. string.rep("x", 10))
local inform = lastSeen("CHAT_MSG_BN_WHISPER_INFORM")
assert(inform.args[1] == string.rep("x", 230) and inform.args[2] == "Pawster#1234" and inform.args[13] == 2)
assert(arg13 == nil and event == nil, "event globals put back")
sent = {}
BNSendWhisper(5, "you there?")
assert(#sent == 0 and printed[#printed]:find("Zed#4444"), "offline friends can't be whispered")

-- Over 255 bytes, cut on a character boundary.
sent = {}
BNSendWhisper(2, string.rep("a", 254) .. "\195\169")
assert(#sent == 2 and (sent[1].text .. sent[2].text):find(string.rep("a", 34) .. "$"), "the half character is dropped")

-- Incoming whisper and toasts.
server("W\t2\thello there")
local w = lastSeen("CHAT_MSG_BN_WHISPER")
assert(w.args[1] == "hello there" and w.args[2] == "Pawster#1234" and w.args[13] == 2 and w.args[11] > 900000000)
server("O\t2")
assert(lastSeen("BN_FRIEND_ACCOUNT_ONLINE").args[1] == 2)
assert(lastSeen("CHAT_MSG_BN_INLINE_TOAST_ALERT").args[1] == "FRIEND_ONLINE")
server("R\t11\tOther\t6666\t0\t")
assert(lastSeen("BN_FRIEND_INVITE_ADDED").args[1] == 11)

-- A whisper nobody listens for still shows up.
listener:UnregisterEvent("CHAT_MSG_BN_WHISPER")
printed = {}
server("W\t2\tanyone?")
assert(printed[1] == "Pawster#1234 whispers: anyone?")

-- Old-style handlers read the event globals; a handler that errors doesn't count as shown.
local oldStyle = newFrame()
oldStyle:RegisterEvent("CHAT_MSG_BN_WHISPER")
local got
oldStyle:SetScript("OnEvent", function() got = { event, arg1, arg2, arg13 } end)
printed = {}
server("W\t2\tglobals")
assert(got[1] == "CHAT_MSG_BN_WHISPER" and got[2] == "globals" and got[3] == "Pawster#1234" and got[4] == 2)
assert(#printed == 0)
oldStyle:SetScript("OnEvent", function() error("broken chat frame") end)
geterrorhandler = function() return function() end end
server("W\t2\tstill shown")
assert(printed[1] == "Pawster#1234 whispers: still shown", "an erroring frame doesn't block the fallback")

-- The add-friend dialog: tags become requests, names stay character friends.
sent = {}
AddFriendNameEditBox:SetText("Buddy#4321")
AddFriendNoteEditBox:SetText("from the raid")
AddFriendEntryFrameAcceptButton:GetScript("OnClick")()
assert(sent[1].text == "A\tBuddy#4321\tfrom the raid" and not AddFriendFrame.shown)
AddFriendNameEditBox:SetText("Birta")
AddFriendFrame_Accept()
assert(characterFriendAdded == "Birta")
AddFriendFrame.BNconnected = true
AddFriendNameEditBox:SetText("Buddy#")
AddFriendNameEditBox:GetScript("OnTextChanged")(AddFriendNameEditBox, true)
assert(AddFriendFrame.expanded)
AddFriendFrame_ShowEntry()
assert(AddFriendEntryFrameLeftTitle.text == "Account Friend")

print("shim tests passed")
