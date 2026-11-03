# mod-account-friends

Friends by account instead of by character for AzerothCore (3.3.5a), in the style of Real ID:
add someone once and see them online whichever character they play.

- **Friend tags.** Each player picks a tag once (`.friends tag Pawster` gives them something like
  `Pawster#1234`). Friends add the tag, never a login name, and the other side has to accept
  the request.
- **Online on any character.** Account friends see each other's character, level, class, zone,
  guild and AFK/DND, a broadcast line, and a note of their own. They get a toast when a friend
  comes online or goes offline. Switching characters only shows the new character; nobody sees
  you go offline and back.
- **Whispers to the account.** A whisper reaches whichever character the friend is on.
- **AccountFriends addon.** The 3.3.5a client already has the whole Real ID interface: blue
  friend rows, toasts, a pending requests tab, Battle.net whisper chat. It is switched off on
  private servers. The addon fills it with your account friends, so they look and work like
  Real ID friends (and any UI that skins the Friends window skins them too). Without the addon,
  the same comes through system messages and `.friends` commands.

## Requirements

- AzerothCore wotlk (master) with the WotLK 3.3.5a (12340) client, MySQL 8 (or MariaDB).
- The addon is optional but recommended. Players without it use the commands.
- Optional: [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots). Bots never get
  account friends. The module recognizes them with `WorldSession::IsHeadless()` when the core
  has it.

## Install the module

```sh
cd <azerothcore>/modules
git clone https://github.com/buildthehomelab/wow-mod-account-friends.git mod-account-friends
```

Clone into `mod-account-friends` (without `wow-`): AzerothCore derives the loader name from the
folder. Re-run CMake, rebuild, and copy `conf/mod_account_friends.conf.dist` to
`mod_account_friends.conf`. The database import creates the tables (`data/sql/db-characters`).

| Setting | Default | |
| --- | --- | --- |
| `AccountFriends.Enable` | 1 | The whole module. |
| `AccountFriends.OfflineDelay` | 30 | Seconds at character select before friends see you go offline. |
| `AccountFriends.MaxFriends` | 100 | Account friends per account. |
| `AccountFriends.MaxSentRequests` | 20 | Unanswered requests one account can have out. |
| `AccountFriends.TagHint` | 1 | Once per account and server start, tell players without a tag how to pick one. |

## The addon

Copy `addon/AccountFriends` into the client's `Interface/AddOns`, or hand it out with
Portalkeeper (`sql/portalkeeper_addon.sql`).

| Where | What it does |
| --- | --- |
| Friends list | Account friends in blue at the top, as `Pawster#1234 (Birta)`. The tooltip shows the character, level, race, class, zone and your note. |
| Add Friend | Type `Name#1234` (plus an optional message) for an account friend, or a character name for a normal friend. |
| Pending tab | Requests you got: accept or decline. |
| Right-click a friend | Whisper, invite, set note, remove. |
| Broadcast box | A line all your account friends see (with a toast). |
| Chat | `/w Pawster#1234`, clicking a name or replying with `/r` whispers the account. A plain `/w Pawster` stays a whisper to the character named Pawster. Toasts follow the client's own toast settings, shown as Interface Options > Account Friends. |

Battle.net features the server doesn't have (conversations, friends of friends, blocking and
reporting) are taken out of the menus.

How it works: the addon replaces the client's Battle.net functions (`BNGetFriendInfo` and the
rest) with versions that answer from the list the server sends. It also raises the client's own
Battle.net events, so the stock Friends window draws everything itself. Before the server has
sent a list, and on servers without the module, every replaced function passes through to the
client's own, so nothing changes there.

Known limit: the Friends window now runs addon code, so the 3.3.5 dropdown taint can apply. If a
unit menu's **Set Focus** is ever blocked after you used an account friend's right-click menu,
`/reload` clears it (or use `/focus`). Typed `/cast` and macros are not affected.

## Commands

| Command | |
| --- | --- |
| `.friends` | Your tag and counts, and the command list. |
| `.friends tag <name>` | Pick or change your tag: 2-12 letters and digits. The number is added for you. |
| `.friends add <Name#1234> [message]` | Send a request. If they already asked you, this accepts. |
| `.friends accept <Name#1234>` / `decline <Name#1234>` | Answer a request. |
| `.friends remove <Name#1234>` | End the friendship (for both). |
| `.friends whisper <Name#1234> <text>` (or `.friends w`) | Whisper the account: whichever character they're on. |
| `.friends note <Name#1234> [text]` | Your private note on a friend. |
| `.friends broadcast [text]` | A line for all your account friends; empty clears it. |
| `.friends list` | Friends with where they are, and requests. |

Without the addon you get system messages: who is online a few seconds after login,
online/offline lines, requests and broadcasts. A whisper from an account friend arrives as a
normal whisper from their character, so `/r` works.

## Troubleshooting

- **No blue rows, the Friends window looks stock.** The addon got no list from the server: is
  the module installed and `AccountFriends.Enable = 1`? The addon asks three times in the first
  20 seconds after login. `/reload` asks again.
- **"pick a friend tag first".** Requests and the list need your own tag: `.friends tag <name>`.
- **"nobody has the tag ...".** Tags are `Name#1234`; ask your friend for theirs (`.friends`).

## Uninstall

Remove the module folder, rebuild, then run
`data/sql/uninstall/mod_account_friends_uninstall_characters.sql` on the characters database.
Tags, friendships and requests are deleted.

## Tests

```sh
c++ -std=c++17 -Wall -Wextra -Werror -Isrc tests/text.cpp -o /tmp/text && /tmp/text
lua5.1 tests/protocol.lua && lua5.1 tests/shim.lua
```

`tests/sql_smoke.py` runs every SQL statement of the module (read from the C++) against a
throwaway MySQL 8 server; see the top of the file for the `docker run` line. It drops and
recreates its own test database.

## Credits

Author: [buildthehomelab](https://github.com/buildthehomelab). The Friends window, toasts and
Battle.net chat are Blizzard's own 3.3.5 interface; the addon only feeds them.

## License

MIT, see [LICENSE](LICENSE).
