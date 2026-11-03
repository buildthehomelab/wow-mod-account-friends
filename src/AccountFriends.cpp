/*
 * mod-account-friends: friends by account instead of by character, in the style of Real ID.
 *
 * A player picks a friend tag (Name#1234) with `.friends tag`. Others add the tag, the player
 * accepts, and from then on each sees the other online whichever character they play: name,
 * level, class, zone, AFK/DND, a broadcast line and a note of their own. Whispers go to the
 * account, so they reach whatever character the friend is on.
 *
 * The AccountFriends addon shows all of this in the stock Friends window's Real ID rows (the
 * blue ones), with the stock toasts and Battle.net whisper chat. Without the addon the same
 * happens through system messages and `.friends` commands.
 *
 * Everything runs on the world thread: logins, logouts, chat and commands are world-thread
 * handlers, and character details are read in WorldScript::OnUpdate.
 *
 *   addon -> server  (addon whisper to self, prefix ACFR)
 *     H <version>          hello; the server answers with a full sync
 *     A <tag> <message>    friend request       Y <request>  accept    D <request>  decline
 *     X <friend>           remove friend        N <friend> <note>      C <broadcast>
 *     w|W <friend> <text>  whisper, in pieces; W ends it
 *   server -> addon
 *     Y ... E              full sync: S self, F friend, N note, T where they are, R request
 *     F N T R              one friend or request changed     X <friend>  Q <request>  removed
 *     O <friend>  Z <friend>   came online / went offline (the toast)
 *     B <friend>           broadcast changed (the toast)
 *     w|W <friend> <text>  whisper from a friend      i|I <friend> <text>  your whisper, sent
 *
 * Released under the MIT License, see LICENSE.
 */

#include "AccountFriendsText.h"

#include "AsyncCallbackProcessor.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "GuildMgr.h"
#include "Hyperlinks.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "QueryCallback.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Transaction.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <map>
#include <set>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace Acore::ChatCommands;

namespace AccountFriends
{
    namespace
    {
        using namespace Text;

        constexpr char const* Prefix = "ACFR";
        constexpr char const* Blue = "|cff82c5ff";  // FRIENDS_BNET_NAME_COLOR
        constexpr char const* Label = "|cff82c5ffFriends:|r ";

        constexpr uint32 RefreshInterval = 2 * IN_MILLISECONDS;
        constexpr uint32 SummaryDelay = 10 * IN_MILLISECONDS;


        struct Config
        {
            bool enable = true;
            uint32 offlineDelay = 30;
            uint32 maxFriends = 100;
            uint32 maxSentRequests = 20;
            bool tagHint = true;
        };

        Config config;

        struct Profile
        {
            uint32 id = 0;
            uint32 account = 0;
            std::string name;
            uint16 code = 0;
            std::string broadcast;
            uint32 broadcastTime = 0;
            uint32 lastOnline = 0;
        };

        struct Friend
        {
            Profile profile;
            std::string note;
        };

        struct Request
        {
            uint32 id = 0;
            Profile from;
            std::string message;
            uint32 time = 0;
        };

        // Where a character is, kept as ids and turned into text in the reader's locale.
        struct Toon
        {
            std::string name;
            uint8 level = 0;
            uint8 cls = 0;
            uint8 race = 0;
            uint8 team = 0;
            uint32 zone = 0;
            std::string guild;
            bool afk = false;
            bool dnd = false;

            bool operator==(Toon const&) const = default;
        };

        // An account with a character in the game, or at character select inside the offline
        // delay (character switches don't count as going offline).
        struct Online
        {
            uint32 account = 0;
            uint64 token = 0;            // tells a stale database answer from a fresh login
            ObjectGuid guid;             // empty at character select
            uint32 leftAt = 0;           // getMSTime when the character logged out
            uint32 loginAt = 0;
            bool loaded = false;
            bool hasAddon = false;
            bool announced = false;      // friends were told this account came online
            bool summarySent = false;
            bool reload = false;         // changed by others while loading: load again
            uint32 lastSync = 0;         // getMSTime of the last full sync
            std::optional<Profile> self;
            std::map<uint32, Friend> friends;  // by friend account
            std::vector<Request> requests;     // received
            std::optional<Toon> toon;
            std::string whisperPieces;
            uint32 whisperTarget = 0;
            RateLimit actions{ 10, 1000 };
            RateLimit whispers{ 8, 1000 };
        };

        std::unordered_map<uint32, Online> online;  // by account
        uint64 nextToken = 0;
        uint32 nextRequestId = 0;
        uint32 refreshTimer = 0;
        std::map<std::pair<uint32, uint32>, uint32> declined;     // (from, to) -> unix time
        std::set<std::pair<uint32, uint32>> requestsInFlight;     // (from, to) being checked
        std::unordered_set<uint32> hinted;
        uint32 pruneTimer = 0;

        constexpr uint32 SyncInterval = 3 * IN_MILLISECONDS;
        constexpr uint32 RerequestDelay = HOUR;   // after a decline, before the same pair again
        constexpr uint32 PruneInterval = HOUR * IN_MILLISECONDS;

        // Never destroyed: a static processor could outlive the database pools at shutdown.
        QueryCallbackProcessor& Queries()
        {
            static QueryCallbackProcessor* processor = new QueryCallbackProcessor();
            return *processor;
        }

        AsyncCallbackProcessor<TransactionCallback>& Transactions()
        {
            static AsyncCallbackProcessor<TransactionCallback>* processor = new AsyncCallbackProcessor<TransactionCallback>();
            return *processor;
        }

        // Bots are sessions without a socket: WorldSession::IsHeadless() on current cores,
        // IsBot() on older playerbots forks, neither on older stock cores.
        template <typename Session, typename = void>
        struct HasIsHeadless : std::false_type { };

        template <typename Session>
        struct HasIsHeadless<Session, std::void_t<decltype(std::declval<Session&>().IsHeadless())>> : std::true_type { };

        template <typename Session, typename = void>
        struct HasIsBot : std::false_type { };

        template <typename Session>
        struct HasIsBot<Session, std::void_t<decltype(std::declval<Session&>().IsBot())>> : std::true_type { };

        template <typename Session>
        bool IsBotSession(Session* session)
        {
            if constexpr (HasIsHeadless<Session>::value)
                return session->IsHeadless();
            else if constexpr (HasIsBot<Session>::value)
                return session->IsBot();
            else
                return false;
        }

        bool IsRealPlayer(Player const* player)
        {
            WorldSession* session = player ? player->GetSession() : nullptr;
            return session && !IsBotSession(session);
        }

        uint32 Now()
        {
            return uint32(GameTime::GetGameTime().count());
        }

        std::string Escaped(std::string text)
        {
            CharacterDatabase.EscapeString(text);
            return text;
        }

        bool SameTag(Profile const& profile, Tag const& tag)
        {
            return profile.code == tag.code && StringEqualI(profile.name, tag.name);
        }

        std::string TagOf(Profile const& profile)
        {
            return FormatTag(profile.name, profile.code);
        }

        std::string Colored(Profile const& profile)
        {
            return Acore::StringFormat("{}{}|r", Blue, profile.name);
        }

        Online* Find(uint32 account)
        {
            auto it = online.find(account);
            return it == online.end() ? nullptr : &it->second;
        }

        // The character, when the account is in the world right now.
        Player* PlayerOf(Online const& entry)
        {
            if (entry.guid.IsEmpty())
                return nullptr;
            Player* player = ObjectAccessor::FindConnectedPlayer(entry.guid);
            return player && player->IsInWorld() ? player : nullptr;
        }

        Friend* FriendByPid(Online& entry, uint32 pid)
        {
            for (auto& [account, buddy] : entry.friends)
                if (buddy.profile.id == pid)
                    return &buddy;
            return nullptr;
        }

        void LoadConfig()
        {
            config.enable = sConfigMgr->GetOption<bool>("AccountFriends.Enable", true);
            config.offlineDelay = sConfigMgr->GetOption<uint32>("AccountFriends.OfflineDelay", 30);
            config.maxFriends = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AccountFriends.MaxFriends", 100));
            config.maxSentRequests = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AccountFriends.MaxSentRequests", 20));
            config.tagHint = sConfigMgr->GetOption<bool>("AccountFriends.TagHint", true);
        }

        // ---- sending ------------------------------------------------------------------------

        void Say(Player* player, std::string const& text)
        {
            if (player)
                ChatHandler(player->GetSession()).SendSysMessage(Label + text);
        }

        void SayTo(Online const& entry, std::string const& text)
        {
            Say(PlayerOf(entry), text);
        }

        void SendAddon(Player* player, std::string const& payload)
        {
            if (!player)
                return;
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, std::string(Prefix) + "\t" + payload);
            player->GetSession()->SendPacket(&data);
        }

        // Free text in pieces, `lower` for all but the last, `upper` for the last.
        void SendPieces(Player* player, char lower, char upper, uint32 id, std::string_view text)
        {
            std::vector<std::string> const pieces = Chunks(text);
            for (std::size_t i = 0; i < pieces.size(); ++i)
                SendAddon(player, Acore::StringFormat("{}\t{}\t{}", i + 1 < pieces.size() ? lower : upper, id, pieces[i]));
        }

        std::string SelfLine(Online const& entry)
        {
            // 1 = groups may cross factions.
            uint32 const flags = sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GROUP) ? 1 : 0;
            if (!entry.self)
                return Acore::StringFormat("S\t0\t\t0\t{}\t0\t", flags);
            Profile const& self = *entry.self;
            return Acore::StringFormat("S\t{}\t{}\t{}\t{}\t{}\t{}", self.id, self.name, self.code, flags, self.broadcastTime, self.broadcast);
        }

        std::string FriendLine(Profile const& profile)
        {
            return Acore::StringFormat("F\t{}\t{}\t{}\t{}\t{}\t{}", profile.id, profile.name, profile.code, profile.lastOnline,
                profile.broadcastTime, profile.broadcast);
        }

        std::string RequestLine(Request const& request)
        {
            return Acore::StringFormat("R\t{}\t{}\t{}\t{}\t{}", request.id, request.from.name, request.from.code, request.time, request.message);
        }

        // W = in the game (or switching characters), - = offline.
        std::string ToonLine(Profile const& profile, LocaleConstant locale)
        {
            Online const* entry = Find(profile.account);
            if (entry && entry->toon)
            {
                Toon const& toon = *entry->toon;
                std::string race, zone;
                if (ChrRacesEntry const* raceEntry = sChrRacesStore.LookupEntry(toon.race))
                    race = raceEntry->name[locale] && *raceEntry->name[locale] ? raceEntry->name[locale] : raceEntry->name[LOCALE_enUS];
                if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(toon.zone))
                    zone = area->area_name[locale] && *area->area_name[locale] ? area->area_name[locale] : area->area_name[LOCALE_enUS];
                uint32 const flags = (toon.afk ? 1 : 0) | (toon.dnd ? 2 : 0);
                return Acore::StringFormat("T\t{}\tW\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", profile.id, toon.name, toon.level, toon.cls,
                    race, toon.team, flags, zone, toon.guild);
            }
            return Acore::StringFormat("T\t{}\t-", profile.id);
        }

        LocaleConstant LocaleOf(Player* player)
        {
            return player ? player->GetSession()->GetSessionDbcLocale() : LOCALE_enUS;
        }

        void SendFriend(Player* player, Friend const& buddy)
        {
            SendAddon(player, FriendLine(buddy.profile));
            if (!buddy.note.empty())
                SendAddon(player, Acore::StringFormat("N\t{}\t{}", buddy.profile.id, buddy.note));
            SendAddon(player, ToonLine(buddy.profile, LocaleOf(player)));
        }

        void SendSync(Online& entry)
        {
            Player* player = PlayerOf(entry);
            if (!player || !entry.loaded || !entry.hasAddon)
                return;
            entry.lastSync = getMSTime();
            SendAddon(player, "Y");
            SendAddon(player, SelfLine(entry));
            for (auto const& [account, buddy] : entry.friends)
                SendFriend(player, buddy);
            for (Request const& request : entry.requests)
                SendAddon(player, RequestLine(request));
            SendAddon(player, "E");
        }

        // Every loaded, in-world account that has `account` as a friend.
        template <typename Fn>
        void ForEachWatcher(uint32 account, Fn&& fn)
        {
            for (auto& [other, entry] : online)
            {
                if (!entry.loaded)
                    continue;
                auto it = entry.friends.find(account);
                if (it == entry.friends.end())
                    continue;
                if (Player* player = PlayerOf(entry))
                    fn(entry, it->second, player);
            }
        }

        // Tells the friends of `account` (addon users) where it is now.
        void PushToon(uint32 account)
        {
            ForEachWatcher(account, [](Online& watcher, Friend& buddy, Player* player)
            {
                if (watcher.hasAddon)
                    SendAddon(player, ToonLine(buddy.profile, LocaleOf(player)));
            });
        }

        // Updates every cached copy of a profile (the account's own and its friends' copies).
        template <typename Fn>
        void EditProfile(uint32 account, Fn&& fn)
        {
            if (Online* entry = Find(account); entry && entry->self)
                fn(*entry->self);
            for (auto& [other, entry] : online)
                if (auto it = entry.friends.find(account); it != entry.friends.end())
                    fn(it->second.profile);
        }

        // ---- coming online, going offline ---------------------------------------------------

        std::string ToonSummary(Toon const& toon, LocaleConstant locale)
        {
            std::string zone;
            if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(toon.zone))
                zone = area->area_name[locale] && *area->area_name[locale] ? area->area_name[locale] : area->area_name[LOCALE_enUS];
            return zone.empty() ? toon.name : toon.name + ", " + zone;
        }

        void AnnounceOnline(Online& entry)
        {
            entry.announced = true;
            if (!entry.self)
                return;
            Profile const& self = *entry.self;

            for (auto const& [account, buddy] : entry.friends)
            {
                Online* watcher = Find(account);
                Player* player = watcher && watcher->loaded ? PlayerOf(*watcher) : nullptr;
                if (!player)
                    continue;
                if (watcher->hasAddon)
                {
                    SendAddon(player, ToonLine(self, LocaleOf(player)));
                    SendAddon(player, Acore::StringFormat("O\t{}", self.id));
                }
                else if (entry.toon)
                    Say(player, Acore::StringFormat("{} ({}) has come online.", Colored(self), entry.toon->name));
            }
        }

        void GoOffline(Online& entry)
        {
            if (entry.self && entry.announced)
            {
                Profile const& self = *entry.self;
                uint32 const now = Now();
                EditProfile(entry.account, [now](Profile& profile) { profile.lastOnline = now; });
                // Gone from the map first, so ToonLine reports offline.
                uint32 const account = entry.account;
                uint32 const pid = self.id;
                std::string const colored = Colored(self);
                online.erase(account);
                ForEachWatcher(account, [&](Online& watcher, Friend& buddy, Player* player)
                {
                    if (watcher.hasAddon)
                    {
                        SendAddon(player, FriendLine(buddy.profile));
                        SendAddon(player, ToonLine(buddy.profile, LocaleOf(player)));
                        SendAddon(player, Acore::StringFormat("Z\t{}", pid));
                    }
                    else
                        Say(player, colored + " has gone offline.");
                });
                return;
            }
            online.erase(entry.account);
        }

        // ---- loading an account -------------------------------------------------------------

        Profile ReadProfile(Field* fields)
        {
            Profile profile;
            profile.account = fields[0].Get<uint32>();
            profile.id = fields[1].Get<uint32>();
            profile.name = fields[2].Get<std::string>();
            profile.code = fields[3].Get<uint16>();
            profile.broadcast = fields[4].Get<std::string>();
            profile.broadcastTime = fields[5].Get<uint32>();
            profile.lastOnline = fields[6].Get<uint32>();
            return profile;
        }

        // The seven columns ReadProfile reads, from `account_friends_profile` aliased as `alias`.
        std::string ProfileColumns(char const* alias)
        {
            return Acore::StringFormat(
                "{0}.`account_id`, {0}.`id`, {0}.`tag_name`, {0}.`tag_code`, {0}.`broadcast`, {0}.`broadcast_time`, {0}.`last_online`",
                alias);
        }

        void Finish(Online& entry);

        // Three queries in a row: own profile, friends, received requests.
        void Load(uint32 account, uint64 token)
        {
            auto stale = [account, token]() -> Online*
            {
                Online* entry = Find(account);
                return entry && entry->token == token ? entry : nullptr;
            };

            Queries().AddCallback(CharacterDatabase.AsyncQuery(Acore::StringFormat(
                "SELECT {} FROM `account_friends_profile` p WHERE p.`account_id` = {}", ProfileColumns("p"), account))
                .WithCallback([account, stale](QueryResult result)
                {
                    Online* entry = stale();
                    if (!entry)
                        return;
                    if (result)
                        entry->self = ReadProfile(result->Fetch());

                    Queries().AddCallback(CharacterDatabase.AsyncQuery(Acore::StringFormat(
                        "SELECT {}, l.`note` FROM `account_friends_link` l "
                        "JOIN `account_friends_profile` p ON p.`account_id` = l.`friend_account_id` WHERE l.`account_id` = {}",
                        ProfileColumns("p"), account))
                        .WithCallback([account, stale](QueryResult result)
                        {
                            Online* entry = stale();
                            if (!entry)
                                return;
                            if (result)
                            {
                                do
                                {
                                    Field* fields = result->Fetch();
                                    Friend buddy{ ReadProfile(fields), fields[7].Get<std::string>() };
                                    entry->friends[buddy.profile.account] = std::move(buddy);
                                } while (result->NextRow());
                            }

                            Queries().AddCallback(CharacterDatabase.AsyncQuery(Acore::StringFormat(
                                "SELECT {}, r.`id`, r.`message`, r.`created_at` FROM `account_friends_request` r "
                                "JOIN `account_friends_profile` p ON p.`account_id` = r.`from_account_id` WHERE r.`to_account_id` = {} ORDER BY r.`id`",
                                ProfileColumns("p"), account))
                                .WithCallback([stale](QueryResult result)
                                {
                                    Online* entry = stale();
                                    if (!entry)
                                        return;
                                    if (result)
                                    {
                                        do
                                        {
                                            Field* fields = result->Fetch();
                                            entry->requests.push_back({ fields[7].Get<uint32>(), ReadProfile(fields),
                                                fields[8].Get<std::string>(), fields[9].Get<uint32>() });
                                        } while (result->NextRow());
                                    }
                                    Finish(*entry);
                                }));
                        }));
                }));
        }

        void Finish(Online& entry)
        {
            // A friend or request was added while the queries ran; they may have missed it.
            if (entry.reload)
            {
                entry.reload = false;
                entry.self.reset();
                entry.friends.clear();
                entry.requests.clear();
                entry.token = ++nextToken;
                Load(entry.account, entry.token);
                return;
            }
            entry.loaded = true;
            SendSync(entry);
            if (!entry.announced && entry.toon && !entry.guid.IsEmpty())
                AnnounceOnline(entry);
        }

        // Without the addon: who's online and what's waiting, once per character login.
        void SendSummary(Online& entry)
        {
            entry.summarySent = true;
            Player* player = PlayerOf(entry);
            if (!player)
                return;

            if (!entry.self)
            {
                if (config.tagHint && hinted.insert(entry.account).second)
                    Say(player, "pick a friend tag so friends can add you across all your characters: .friends tag <name>");
                return;
            }
            if (entry.hasAddon)
                return;

            std::vector<std::string> names;
            for (auto const& [account, buddy] : entry.friends)
            {
                Online const* other = Find(account);
                if (other && other->toon)
                    names.push_back(Colored(buddy.profile) + " (" + ToonSummary(*other->toon, LocaleOf(player)) + ")");
            }
            if (!names.empty())
            {
                std::string line = "online: ";
                for (std::size_t i = 0; i < names.size(); ++i)
                    line += (i ? ", " : "") + names[i];
                Say(player, line);
            }
            if (!entry.requests.empty())
                Say(player, Acore::StringFormat("{} friend request(s) waiting; see .friends list", entry.requests.size()));
        }

        // ---- characters entering and leaving ------------------------------------------------

        std::optional<Toon> ReadToon(Player* player)
        {
            if (!player)
                return std::nullopt;
            Toon toon;
            toon.name = player->GetName();
            toon.level = player->GetLevel();
            toon.cls = player->getClass();
            toon.race = player->getRace();
            toon.team = player->GetTeamId() == TEAM_HORDE ? 1 : 0;
            toon.zone = player->GetZoneId();
            // Not Player::GetGuildName(): it follows a null guild for a character with none.
            toon.guild = sGuildMgr->GetGuildNameById(player->GetGuildId());
            toon.afk = player->isAFK();
            toon.dnd = player->isDND();
            return toon;
        }

        void OnLogin(Player* player)
        {
            uint32 const account = player->GetSession()->GetAccountId();
            auto [it, created] = online.try_emplace(account);
            Online& entry = it->second;
            entry.guid = player->GetGUID();
            entry.leftAt = 0;
            entry.loginAt = getMSTime();
            entry.summarySent = false;
            entry.hasAddon = false;  // the addon says hello again on every character
            entry.toon = ReadToon(player);

            if (created)
            {
                entry.account = account;
                entry.token = ++nextToken;
                Load(account, entry.token);
                return;
            }
            // A character switch: the friends just see the new character.
            if (entry.loaded)
            {
                if (entry.announced)
                    PushToon(account);
                else
                    AnnounceOnline(entry);
            }
        }

        void OnLogout(Player* player)
        {
            uint32 const account = player->GetSession()->GetAccountId();
            Online* entry = Find(account);
            if (!entry || entry->guid != player->GetGUID())
                return;
            entry->guid.Clear();
            entry->leftAt = getMSTime();
            entry->whisperPieces.clear();
            if (entry->self)
                CharacterDatabase.Execute("UPDATE `account_friends_profile` SET `last_online` = {} WHERE `account_id` = {}", Now(), account);
        }

        void Refresh()
        {
            uint32 const nowMs = getMSTime();
            std::vector<uint32> gone, changed;
            for (auto& [account, entry] : online)
            {
                if (entry.guid.IsEmpty())
                {
                    if (getMSTimeDiff(entry.leftAt, nowMs) >= config.offlineDelay * IN_MILLISECONDS)
                        gone.push_back(account);
                    continue;
                }
                Player* player = PlayerOf(entry);
                if (!player)
                    continue;  // between maps
                if (!entry.summarySent && entry.loaded && getMSTimeDiff(entry.loginAt, nowMs) >= SummaryDelay)
                    SendSummary(entry);
                std::optional<Toon> toon = ReadToon(player);
                if (toon != entry.toon)
                {
                    entry.toon = std::move(toon);
                    if (entry.announced)
                        changed.push_back(account);
                }
            }
            for (uint32 account : changed)
                PushToon(account);
            for (uint32 account : gone)
                if (Online* entry = Find(account))
                    GoOffline(*entry);
        }

        // ---- actions (from the addon or from commands) --------------------------------------

        // The caller's account entry, ready for actions, or nothing (with a message).
        Online* Actor(Player* player, bool needTag)
        {
            if (!config.enable || !IsRealPlayer(player))
                return nullptr;
            Online* entry = Find(player->GetSession()->GetAccountId());
            if (!entry || !entry->loaded)
            {
                Say(player, "your friend list is still loading; try again in a moment.");
                return nullptr;
            }
            if (!entry->actions.Allow(getMSTime()))
            {
                Say(player, "slow down a little.");
                return nullptr;
            }
            if (needTag && !entry->self)
            {
                Say(player, "pick a friend tag first: .friends tag <name>");
                return nullptr;
            }
            return entry;
        }

        void SetTag(Player* player, std::string_view name)
        {
            Online* entry = Actor(player, false);
            if (!entry)
                return;
            if (!ValidTagName(name))
            {
                Say(player, "a tag is 2-12 letters and digits, starting with a letter.");
                return;
            }

            uint32 const account = entry->account;
            uint64 const token = entry->token;
            std::string const wanted(name);
            Queries().AddCallback(CharacterDatabase.AsyncQuery(Acore::StringFormat(
                "SELECT `tag_code` FROM `account_friends_profile` WHERE `tag_name` = '{}'", Escaped(wanted)))
                .WithCallback([account, token, wanted](QueryResult result)
                {
                    Online* entry = Find(account);
                    if (!entry || entry->token != token)
                        return;
                    std::unordered_set<uint16> used;
                    if (result)
                        do used.insert(result->Fetch()[0].Get<uint16>()); while (result->NextRow());
                    if (used.size() > 8000)
                    {
                        SayTo(*entry, "that name is too popular; pick another.");
                        return;
                    }
                    uint16 code;
                    do code = uint16(urand(TagCodeMin, TagCodeMax)); while (used.count(code));

                    // IGNORE: someone may take the same tag meanwhile; the read below tells.
                    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
                    if (entry->self)
                        trans->Append("UPDATE IGNORE `account_friends_profile` SET `tag_name` = '{}', `tag_code` = {} WHERE `account_id` = {}",
                            Escaped(wanted), code, account);
                    else
                        trans->Append("INSERT IGNORE INTO `account_friends_profile` (`account_id`, `tag_name`, `tag_code`) VALUES ({}, '{}', {})",
                            account, Escaped(wanted), code);
                    TransactionCallback callback = CharacterDatabase.AsyncCommitTransaction(trans);
                    callback.AfterComplete([account, token, wanted, code](bool)
                    {
                        Queries().AddCallback(CharacterDatabase.AsyncQuery(Acore::StringFormat(
                            "SELECT {} FROM `account_friends_profile` p WHERE p.`account_id` = {}", ProfileColumns("p"), account))
                            .WithCallback([account, token, wanted, code](QueryResult result)
                            {
                                Online* entry = Find(account);
                                if (!entry || entry->token != token)
                                    return;
                                if (!result)
                                {
                                    SayTo(*entry, "that tag was just taken; try again.");
                                    return;
                                }
                                Profile const profile = ReadProfile(result->Fetch());
                                if (profile.code != code || !StringEqualI(profile.name, wanted))
                                {
                                    SayTo(*entry, "that tag was just taken; try again.");
                                    return;
                                }
                                bool const first = !entry->self;
                                EditProfile(account, [&profile](Profile& cached)
                                {
                                    cached.name = profile.name;
                                    cached.code = profile.code;
                                });
                                entry->self = profile;
                                for (auto& [other, receiver] : online)
                                    for (Request& request : receiver.requests)
                                        if (request.from.account == account)
                                        {
                                            request.from.name = profile.name;
                                            request.from.code = profile.code;
                                            if (Player* receiverPlayer = PlayerOf(receiver); receiverPlayer && receiver.hasAddon)
                                                SendAddon(receiverPlayer, RequestLine(request));
                                        }
                                SayTo(*entry, Acore::StringFormat("your friend tag is now {}{}|r. Friends add you with it.", Blue, TagOf(profile)));
                                if (Player* player = PlayerOf(*entry); player && entry->hasAddon)
                                    SendAddon(player, SelfLine(*entry));
                                ForEachWatcher(account, [&profile](Online& watcher, Friend&, Player* player)
                                {
                                    if (watcher.hasAddon)
                                        SendAddon(player, FriendLine(profile));
                                });
                                if (first && !entry->announced && entry->toon)
                                    AnnounceOnline(*entry);
                            }));
                    });
                    Transactions().AddCallback(std::move(callback));
                }));
        }

        void Accept(Online& entry, Request const request);

        void AddFriend(Player* player, std::string_view tagText, std::string_view messageText)
        {
            Online* entry = Actor(player, true);
            if (!entry)
                return;
            std::optional<Tag> tag = ParseTag(tagText);
            if (!tag)
            {
                Say(player, "that's not a friend tag; it looks like Name#1234.");
                return;
            }
            if (SameTag(*entry->self, *tag))
            {
                Say(player, "that's your own tag.");
                return;
            }
            for (auto const& [account, buddy] : entry->friends)
                if (SameTag(buddy.profile, *tag))
                {
                    Say(player, Colored(buddy.profile) + " is already your friend.");
                    return;
                }
            for (Request const& request : entry->requests)
                if (SameTag(request.from, *tag))
                {
                    Accept(*entry, request);
                    return;
                }
            if (entry->friends.size() >= config.maxFriends)
            {
                Say(player, "your friend list is full.");
                return;
            }
            std::string const message = CleanPlain(messageText, RequestMessageMax).value_or("");
            if (!message.empty() && !player->CanSpeak())
            {
                Say(player, "you can't send a request message while muted; send the request without one.");
                return;
            }

            uint32 const account = entry->account;
            uint64 const token = entry->token;
            Queries().AddCallback(CharacterDatabase.AsyncQuery(Acore::StringFormat(
                "SELECT {}, "
                "(SELECT COUNT(*) FROM `account_friends_request` WHERE `from_account_id` = {}), "
                "(SELECT COUNT(*) FROM `account_friends_request` WHERE `from_account_id` = {} AND `to_account_id` = p.`account_id`), "
                "(SELECT COUNT(*) FROM `account_friends_link` WHERE `account_id` = p.`account_id`) "
                "FROM `account_friends_profile` p WHERE p.`tag_name` = '{}' AND p.`tag_code` = {}",
                ProfileColumns("p"), account, account, Escaped(tag->name), tag->code))
                .WithCallback([account, token, message, tagName = FormatTag(tag->name, tag->code)](QueryResult result)
                {
                    Online* entry = Find(account);
                    if (!entry || entry->token != token || !entry->self)
                        return;
                    if (!result)
                    {
                        SayTo(*entry, "nobody has the tag " + tagName + ".");
                        return;
                    }
                    Field* fields = result->Fetch();
                    Profile const target = ReadProfile(fields);
                    uint64 const sent = fields[7].Get<uint64>();
                    uint64 const theirFriends = fields[9].Get<uint64>();
                    if (target.account == account || entry->friends.count(target.account))
                        return;

                    // Lookups for the same pair can overlap (several requests in one tick); the
                    // database count can't see an INSERT that is still queued.
                    std::pair<uint32, uint32> const pair{ account, target.account };
                    bool duplicate = fields[8].Get<uint64>() != 0 || requestsInFlight.count(pair);
                    if (Online const* other = Find(target.account))
                        for (Request const& request : other->requests)
                            duplicate = duplicate || request.from.account == account;
                    if (auto it = declined.find(pair); it != declined.end() && Now() - it->second < RerequestDelay)
                    {
                        SayTo(*entry, Colored(target) + " declined your request recently; try again later.");
                        return;
                    }
                    if (duplicate)
                    {
                        SayTo(*entry, "you already sent " + Colored(target) + " a request.");
                        return;
                    }
                    // Every request may be accepted: friends plus unanswered requests stay within the limit.
                    if (sent >= config.maxSentRequests || entry->friends.size() + sent >= config.maxFriends
                        || theirFriends >= config.maxFriends)
                    {
                        SayTo(*entry, theirFriends >= config.maxFriends ? Colored(target) + "'s friend list is full."
                            : "you have too many unanswered requests for your friend list; wait for some answers first.");
                        return;
                    }

                    // The pair stays taken until the INSERT has run.
                    Request request{ ++nextRequestId, *entry->self, message, Now() };
                    requestsInFlight.insert(pair);
                    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
                    trans->Append(
                        "INSERT IGNORE INTO `account_friends_request` (`id`, `from_account_id`, `to_account_id`, `message`, `created_at`) "
                        "VALUES ({}, {}, {}, '{}', {})", request.id, account, target.account, Escaped(message), request.time);
                    TransactionCallback inserted = CharacterDatabase.AsyncCommitTransaction(trans);
                    inserted.AfterComplete([pair](bool) { requestsInFlight.erase(pair); });
                    Transactions().AddCallback(std::move(inserted));
                    SayTo(*entry, "friend request sent to " + Colored(target) + ".");

                    if (Online* other = Find(target.account); other && !other->loaded)
                        other->reload = true;
                    else if (other)
                    {
                        other->requests.push_back(request);
                        if (Player* otherPlayer = PlayerOf(*other))
                        {
                            if (other->hasAddon)
                                SendAddon(otherPlayer, RequestLine(request));
                            else
                                Say(otherPlayer, Acore::StringFormat("{} sent you a friend request{}{}. Answer with .friends accept {} or .friends decline {}",
                                    Colored(request.from), message.empty() ? "" : ": ", message, TagOf(request.from), TagOf(request.from)));
                        }
                    }
                }));
        }

        void Accept(Online& entry, Request const request)
        {
            uint32 const account = entry.account;
            uint32 const other = request.from.account;
            Online const* requester = Find(other);
            if (entry.friends.size() >= config.maxFriends || (requester && requester->loaded && requester->friends.size() >= config.maxFriends))
            {
                SayTo(entry, entry.friends.size() >= config.maxFriends ? "your friend list is full; remove someone first."
                    : Colored(request.from) + "'s friend list is full.");
                return;
            }
            std::erase_if(entry.requests, [&](Request const& r) { return r.from.account == other; });

            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            trans->Append("INSERT IGNORE INTO `account_friends_link` (`account_id`, `friend_account_id`) VALUES ({}, {}), ({}, {})",
                account, other, other, account);
            trans->Append("DELETE FROM `account_friends_request` WHERE (`from_account_id` = {0} AND `to_account_id` = {1}) "
                "OR (`from_account_id` = {1} AND `to_account_id` = {0})", other, account);
            CharacterDatabase.CommitTransaction(trans);

            Player* player = PlayerOf(entry);
            Friend& added = entry.friends[other] = Friend{ request.from, "" };
            if (player && entry.hasAddon)
            {
                SendAddon(player, Acore::StringFormat("Q\t{}", request.id));
                SendFriend(player, added);
            }
            Say(player, Colored(request.from) + " is now your friend.");

            if (Online* them = Find(other); them && !them->loaded)
                them->reload = true;
            else if (them && entry.self)
            {
                std::erase_if(them->requests, [&](Request const& r) { return r.from.account == account; });
                Friend& mirror = them->friends[account] = Friend{ *entry.self, "" };
                if (Player* theirPlayer = PlayerOf(*them))
                {
                    if (them->hasAddon)
                        SendFriend(theirPlayer, mirror);
                    Say(theirPlayer, Colored(*entry.self) + " accepted your friend request.");
                }
            }
        }

        Request const* RequestById(Online& entry, uint32 id)
        {
            for (Request const& request : entry.requests)
                if (request.id == id)
                    return &request;
            return nullptr;
        }

        Request const* RequestByTag(Online& entry, Tag const& tag)
        {
            for (Request const& request : entry.requests)
                if (SameTag(request.from, tag))
                    return &request;
            return nullptr;
        }

        void Decline(Online& entry, Request const request)
        {
            CharacterDatabase.Execute("DELETE FROM `account_friends_request` WHERE `id` = {} AND `to_account_id` = {}", request.id, entry.account);
            std::erase_if(entry.requests, [&](Request const& r) { return r.id == request.id; });
            declined[{ request.from.account, entry.account }] = Now();
            if (Player* player = PlayerOf(entry))
            {
                if (entry.hasAddon)
                    SendAddon(player, Acore::StringFormat("Q\t{}", request.id));
                Say(player, "declined the request from " + Colored(request.from) + ".");
            }
        }

        void Remove(Online& entry, uint32 other)
        {
            auto it = entry.friends.find(other);
            if (it == entry.friends.end())
                return;
            Profile const removed = it->second.profile;
            entry.friends.erase(it);
            CharacterDatabase.Execute("DELETE FROM `account_friends_link` WHERE (`account_id` = {0} AND `friend_account_id` = {1}) "
                "OR (`account_id` = {1} AND `friend_account_id` = {0})", entry.account, other);
            if (Player* player = PlayerOf(entry))
            {
                if (entry.hasAddon)
                    SendAddon(player, Acore::StringFormat("X\t{}", removed.id));
                Say(player, Colored(removed) + " is no longer your friend.");
            }
            if (Online* them = Find(other); them && !them->loaded)
                them->reload = true;
            else if (them && entry.self)
            {
                them->friends.erase(entry.account);
                if (Player* theirPlayer = PlayerOf(*them); theirPlayer && them->hasAddon)
                    SendAddon(theirPlayer, Acore::StringFormat("X\t{}", entry.self->id));
            }
        }

        void SetNote(Online& entry, Friend& buddy, std::string_view text)
        {
            buddy.note = CleanPlain(text, NoteMax).value_or("");
            CharacterDatabase.Execute("UPDATE `account_friends_link` SET `note` = '{}' WHERE `account_id` = {} AND `friend_account_id` = {}",
                Escaped(buddy.note), entry.account, buddy.profile.account);
            if (Player* player = PlayerOf(entry); player && entry.hasAddon)
                SendAddon(player, Acore::StringFormat("N\t{}\t{}", buddy.profile.id, buddy.note));
        }

        void SetBroadcast(Online& entry, std::string_view text)
        {
            std::string const broadcast = CleanPlain(text, BroadcastMax).value_or("");
            if (Player* player = PlayerOf(entry); player && !broadcast.empty() && !player->CanSpeak())
            {
                Say(player, "you can't broadcast while muted.");
                return;
            }
            uint32 const now = Now();
            CharacterDatabase.Execute("UPDATE `account_friends_profile` SET `broadcast` = '{}', `broadcast_time` = {} WHERE `account_id` = {}",
                Escaped(broadcast), now, entry.account);
            EditProfile(entry.account, [&](Profile& profile)
            {
                profile.broadcast = broadcast;
                profile.broadcastTime = now;
            });
            Player* player = PlayerOf(entry);
            if (player && entry.hasAddon)
                SendAddon(player, SelfLine(entry));
            Say(player, broadcast.empty() ? "broadcast cleared." : "broadcast sent to your friends.");

            Profile const self = *entry.self;
            ForEachWatcher(entry.account, [&](Online& watcher, Friend&, Player* watcherPlayer)
            {
                if (watcher.hasAddon)
                {
                    SendAddon(watcherPlayer, FriendLine(self));
                    if (!broadcast.empty())
                        SendAddon(watcherPlayer, Acore::StringFormat("B\t{}", self.id));
                }
                else if (!broadcast.empty())
                    Say(watcherPlayer, Acore::StringFormat("{}: {}", Colored(self), broadcast));
            });
        }

        // The sender's own copy of a whisper, once it has gone out.
        void Echo(Player* player, Online const& entry, Friend const& buddy, std::string const& text)
        {
            if (entry.hasAddon)
                SendPieces(player, 'i', 'I', buddy.profile.id, text);
            else
                ChatHandler(player->GetSession()).SendSysMessage(Acore::StringFormat("{}To {}:|r {}", Blue, TagOf(buddy.profile), text));
        }

        // A whisper to a friend's account: whichever character they're on.
        void Whisper(Player* player, Online& entry, Friend const& buddy, std::string const& text)
        {
            if (!player->CanSpeak())
            {
                Say(player, "you can't whisper while muted.");
                return;
            }
            if (!ValidWhisper(text) || !Acore::Hyperlinks::CheckAllLinks(text))
            {
                Say(player, "that whisper has text or links that aren't allowed.");
                return;
            }
            if (!entry.whispers.Allow(getMSTime()))
            {
                Say(player, "slow down a little.");
                return;
            }

            Online* them = Find(buddy.profile.account);
            Player* target = them ? PlayerOf(*them) : nullptr;
            if (target && them->loaded && entry.self)
            {
                LOG_INFO("chat.whisper", "Player {} tells {} (account friend): {}", player->GetName(), target->GetName(), text);
                Echo(player, entry, buddy, text);
                if (them->hasAddon)
                    SendPieces(target, 'w', 'W', entry.self->id, text);
                else
                {
                    // A plain whisper from the character, so /r works without the addon.
                    WorldPacket data;
                    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_UNIVERSAL, player, target, text);
                    target->SendDirectMessage(&data);
                }
                return;
            }
            Say(player, Colored(buddy.profile) + (them ? " is switching characters; try again in a moment." : " is offline."));
        }

        // ---- addon messages -----------------------------------------------------------------

        void HandleAddon(Player* player, std::string_view body)
        {
            std::vector<std::string_view> const parts = SplitFields(body, '\t', 3);
            if (parts[0].size() != 1)
                return;
            char const op = parts[0][0];
            Online* entry = Find(player->GetSession()->GetAccountId());
            if (!entry)
                return;

            // A sync is a few packets per friend: at most one every few seconds.
            if (op == 'H')
            {
                entry->hasAddon = true;
                if (!entry->lastSync || getMSTimeDiff(entry->lastSync, getMSTime()) >= SyncInterval)
                    SendSync(*entry);
                return;
            }

            auto field = [&](std::size_t i) { return i < parts.size() ? parts[i] : std::string_view(); };

            // Whisper pieces don't count against the action limit; they have their own.
            if (op == 'w' || op == 'W')
            {
                std::optional<uint32> pid = ParseId(field(1));
                Friend* buddy = pid && entry->loaded ? FriendByPid(*entry, *pid) : nullptr;
                if (!buddy)
                    return;
                if (entry->whisperTarget != *pid)
                    entry->whisperPieces.clear();
                entry->whisperTarget = *pid;
                entry->whisperPieces.append(field(2));
                if (entry->whisperPieces.size() > WhisperMax)
                {
                    entry->whisperPieces.clear();
                    return;
                }
                if (op == 'W')
                {
                    std::string text = std::move(entry->whisperPieces);
                    entry->whisperPieces.clear();
                    Whisper(player, *entry, *buddy, text);
                }
                return;
            }

            switch (op)
            {
                case 'A':
                    AddFriend(player, field(1), field(2));
                    break;
                case 'Y':
                case 'D':
                {
                    Online* actor = Actor(player, true);
                    std::optional<uint32> id = ParseId(field(1));
                    Request const* request = actor && id ? RequestById(*actor, *id) : nullptr;
                    if (request)
                        op == 'Y' ? Accept(*actor, *request) : Decline(*actor, *request);
                    break;
                }
                case 'X':
                {
                    Online* actor = Actor(player, true);
                    std::optional<uint32> pid = ParseId(field(1));
                    if (Friend* buddy = actor && pid ? FriendByPid(*actor, *pid) : nullptr)
                        Remove(*actor, buddy->profile.account);
                    break;
                }
                case 'N':
                {
                    Online* actor = Actor(player, true);
                    std::optional<uint32> pid = ParseId(field(1));
                    if (Friend* buddy = actor && pid ? FriendByPid(*actor, *pid) : nullptr)
                        SetNote(*actor, *buddy, field(2));
                    break;
                }
                case 'C':
                {
                    if (Online* actor = Actor(player, true))
                        SetBroadcast(*actor, body.size() > 2 ? body.substr(2) : std::string_view());
                    break;
                }
                default:
                    break;
            }
        }

        void Update(uint32 diff)
        {
            Queries().ProcessReadyCallbacks();
            Transactions().ProcessReadyCallbacks();
            if (!config.enable)
                return;

            pruneTimer += diff;
            if (pruneTimer >= PruneInterval)
            {
                pruneTimer = 0;
                uint32 const now = Now();
                std::erase_if(declined, [now](auto const& item) { return now - item.second >= RerequestDelay; });
            }

            refreshTimer += diff;
            if (refreshTimer >= RefreshInterval)
            {
                refreshTimer = 0;
                Refresh();
            }
        }

        void Startup()
        {
            if (QueryResult result = CharacterDatabase.Query("SELECT COALESCE(MAX(`id`), 0) FROM `account_friends_request`"))
                nextRequestId = result->Fetch()[0].Get<uint32>();
        }

        // ---- commands -----------------------------------------------------------------------

        Online* CommandActor(ChatHandler* handler, bool needTag)
        {
            Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
            return player ? Actor(player, needTag) : nullptr;
        }

        Friend* FriendByTag(Online& entry, Tag const& tag)
        {
            for (auto& [account, buddy] : entry.friends)
                if (SameTag(buddy.profile, tag))
                    return &buddy;
            return nullptr;
        }
    }

    class AccountFriendsPlayerScript : public PlayerScript
    {
    public:
        AccountFriendsPlayerScript() : PlayerScript("AccountFriendsPlayerScript",
            {
                PLAYERHOOK_ON_LOGIN,
                PLAYERHOOK_ON_LOGOUT,
                PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT
            }) { }

        void OnPlayerLogin(Player* player) override
        {
            if (config.enable && IsRealPlayer(player))
                OnLogin(player);
        }

        void OnPlayerLogout(Player* player) override
        {
            if (IsRealPlayer(player))
                OnLogout(player);
        }

        // The addon whispers itself; those whispers are swallowed here.
        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver) override
        {
            if (type != CHAT_MSG_WHISPER || language != LANG_ADDON || receiver != player)
                return true;
            std::string_view const view(msg);
            std::string_view const prefix(Prefix);
            if (view.size() <= prefix.size() || view.substr(0, prefix.size()) != prefix || view[prefix.size()] != '\t')
                return true;
            if (config.enable && IsRealPlayer(player))
                HandleAddon(player, view.substr(prefix.size() + 1));
            return false;
        }
    };

    class AccountFriendsWorldScript : public WorldScript
    {
    public:
        AccountFriendsWorldScript() : WorldScript("AccountFriendsWorldScript",
            {
                WORLDHOOK_ON_AFTER_CONFIG_LOAD,
                WORLDHOOK_ON_STARTUP,
                WORLDHOOK_ON_UPDATE
            }) { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            LoadConfig();
        }

        // Synchronous, once, before players are in.
        void OnStartup() override
        {
            Startup();
        }

        void OnUpdate(uint32 diff) override
        {
            Update(diff);
        }
    };

    class AccountFriendsCommandScript : public CommandScript
    {
    public:
        AccountFriendsCommandScript() : CommandScript("AccountFriendsCommandScript") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable friendsTable =
            {
                { "tag",       HandleTag,       SEC_PLAYER, Console::No },
                { "add",       HandleAdd,       SEC_PLAYER, Console::No },
                { "accept",    HandleAccept,    SEC_PLAYER, Console::No },
                { "decline",   HandleDecline,   SEC_PLAYER, Console::No },
                { "remove",    HandleRemove,    SEC_PLAYER, Console::No },
                { "note",      HandleNote,      SEC_PLAYER, Console::No },
                { "whisper",   HandleWhisper,   SEC_PLAYER, Console::No },
                { "w",         HandleWhisper,   SEC_PLAYER, Console::No },
                { "broadcast", HandleBroadcast, SEC_PLAYER, Console::No },
                { "list",      HandleList,      SEC_PLAYER, Console::No },
                { "",          HandleHelp,      SEC_PLAYER, Console::No },
            };
            static ChatCommandTable commandTable =
            {
                { "friends", friendsTable },
            };
            return commandTable;
        }

        static Player* Me(ChatHandler* handler)
        {
            return handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        }

        static bool HandleHelp(ChatHandler* handler)
        {
            Online* entry = CommandActor(handler, false);
            if (!entry)
                return true;
            if (entry->self)
                handler->PSendSysMessage("{}your tag is {}{}|r ({} friends, {} requests waiting).", Label, Blue, TagOf(*entry->self),
                    entry->friends.size(), entry->requests.size());
            else
                handler->PSendSysMessage("{}you have no friend tag yet.", Label);
            handler->SendSysMessage(".friends tag <name> | add <Name#1234> [message] | accept <tag> | decline <tag> | remove <tag>");
            handler->SendSysMessage(".friends whisper <tag> <text> | note <tag> [text] | broadcast [text] | list");
            return true;
        }

        static bool HandleTag(ChatHandler* handler, Optional<std::string> name)
        {
            Player* player = Me(handler);
            if (!player)
                return true;
            if (!name)
            {
                Online* entry = CommandActor(handler, false);
                if (entry && entry->self)
                    handler->PSendSysMessage("{}your tag is {}{}|r.", Label, Blue, TagOf(*entry->self));
                else if (entry)
                    handler->PSendSysMessage("{}pick one with .friends tag <name>; you get a number after it, like Name#1234.", Label);
                return true;
            }
            SetTag(player, *name);
            return true;
        }

        static bool HandleAdd(ChatHandler* handler, std::string tag, Optional<Tail> message)
        {
            if (Player* player = Me(handler))
                AddFriend(player, tag, message ? std::string_view(*message) : std::string_view());
            return true;
        }

        static bool HandleAccept(ChatHandler* handler, std::string tagText)
        {
            Online* entry = CommandActor(handler, true);
            std::optional<Tag> tag = ParseTag(tagText);
            if (!entry)
                return true;
            if (Request const* request = tag ? RequestByTag(*entry, *tag) : nullptr)
                Accept(*entry, *request);
            else
                handler->PSendSysMessage("{}no request from {}.", Label, tagText);
            return true;
        }

        static bool HandleDecline(ChatHandler* handler, std::string tagText)
        {
            Online* entry = CommandActor(handler, true);
            std::optional<Tag> tag = ParseTag(tagText);
            if (!entry)
                return true;
            if (Request const* request = tag ? RequestByTag(*entry, *tag) : nullptr)
                Decline(*entry, *request);
            else
                handler->PSendSysMessage("{}no request from {}.", Label, tagText);
            return true;
        }

        static bool HandleRemove(ChatHandler* handler, std::string tagText)
        {
            Online* entry = CommandActor(handler, true);
            std::optional<Tag> tag = ParseTag(tagText);
            if (!entry)
                return true;
            if (Friend* buddy = tag ? FriendByTag(*entry, *tag) : nullptr)
                Remove(*entry, buddy->profile.account);
            else
                handler->PSendSysMessage("{}{} isn't on your friend list.", Label, tagText);
            return true;
        }

        static bool HandleNote(ChatHandler* handler, std::string tagText, Optional<Tail> note)
        {
            Online* entry = CommandActor(handler, true);
            std::optional<Tag> tag = ParseTag(tagText);
            if (!entry)
                return true;
            if (Friend* buddy = tag ? FriendByTag(*entry, *tag) : nullptr)
            {
                SetNote(*entry, *buddy, note ? std::string_view(*note) : std::string_view());
                handler->PSendSysMessage("{}note saved.", Label);
            }
            else
                handler->PSendSysMessage("{}{} isn't on your friend list.", Label, tagText);
            return true;
        }

        // Whispers the account: whichever character the friend is on.
        static bool HandleWhisper(ChatHandler* handler, std::string tagText, Tail text)
        {
            Player* player = Me(handler);
            Online* entry = CommandActor(handler, true);
            std::optional<Tag> tag = ParseTag(tagText);
            if (!entry || !player)
                return true;
            if (Friend* buddy = tag ? FriendByTag(*entry, *tag) : nullptr)
                Whisper(player, *entry, *buddy, std::string(text));
            else
                handler->PSendSysMessage("{}{} isn't on your friend list.", Label, tagText);
            return true;
        }

        static bool HandleBroadcast(ChatHandler* handler, Optional<Tail> text)
        {
            if (Online* entry = CommandActor(handler, true))
                SetBroadcast(*entry, text ? std::string_view(*text) : std::string_view());
            return true;
        }

        static bool HandleList(ChatHandler* handler)
        {
            Online* entry = CommandActor(handler, true);
            Player* player = Me(handler);
            if (!entry || !player)
                return true;
            handler->PSendSysMessage("{}you are {}{}|r.", Label, Blue, TagOf(*entry->self));
            if (entry->friends.empty())
                handler->SendSysMessage("No account friends yet: .friends add <Name#1234>");
            for (auto const& [account, buddy] : entry->friends)
            {
                Online const* other = Find(account);
                std::string where;
                if (other && other->toon)
                    where = "online: " + ToonSummary(*other->toon, LocaleOf(player));
                else
                    where = "offline";
                handler->PSendSysMessage("  {}{}|r - {}{}{}", Blue, TagOf(buddy.profile), where,
                    buddy.note.empty() ? "" : " - ", buddy.note);
            }
            for (Request const& request : entry->requests)
                handler->PSendSysMessage("  request from {}{}|r{}{}", Blue, TagOf(request.from),
                    request.message.empty() ? "" : ": ", request.message);
            return true;
        }
    };
}

void AddAccountFriendsScripts()
{
    new AccountFriends::AccountFriendsPlayerScript();
    new AccountFriends::AccountFriendsWorldScript();
    new AccountFriends::AccountFriendsCommandScript();
}
