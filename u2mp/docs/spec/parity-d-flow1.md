# Spec: PS3 parity, group D (flow.cpp part 1: menu callbacks, bootstrap, MP control registration, net state helpers)

Clean-room parity check of the 60 PS3 1.10 functions `0x438350..0x43ba44` (rows of
`maintainer-tools/research/u2-parity/groupD.tsv`) against the UCMR U2 runtime (`gow3_pc/src/u2mp`, branch
`u2mp-ps3-parity-2`). It says what a PS3 player saw and what our port must show; it does not prescribe code,
structure or names. Addresses are PS3 1.10 (`NPUA80698` EBOOT) unless marked PS4 (PS4 1.02 image offset).
Background: `FUNCTIONS-FRONTEND-NET.md` (section 3 call graph, section 4a), `PORT-FRONTEND2.md`,
`PORT-FRONTEND3.md`, `PORT-FRONTEND6.md`, `PORT-GAMEPLAY3.md` 1.1, `PORT-MATCHMAKING.md` 1.7 / 2.2,
`PORT-ONLINE.md`, specs `remaining.md`, `session.md`, `parity-b-menus.md`.

Status words (as in `parity-c-match.md`):

- **COVERED**: our code, or the intact PS4 code it relies on, gives the same visible result.
- **PLATFORM**: PS3 plumbing (PSN, NP Basic, Matching2, NetCtl, sockets, threads, debug output) that the PC
  design replaces. Only the visible result has to match; the replacement is named.
- **MISSING**: absent, or present with a different visible result in the part named.

## 0. Facts every item uses

**Module data.** This PS3 module's TOC base is `0xdb82b8` (not the `0xe183b8` of the Ghidra default; the
decompiler shows it as the word at `0xe10adc`). Globals named below were resolved through it.

| PS3 global | meaning | PS4 / ours |
|---|---|---|
| `0xf11764` (s32) | NET_SM state | PS4 `0x14ce97c`; `session.cpp set_net_sm`, `remaining.cpp kNetState` |
| `0xf0e800` | menu manager | PS4 `addr::menu_manager` |
| `0xf1fa48` | netInfo (first word = "game data version", the build key every room and presence carries) | PS4 `0x104f1c8` (`kNetInfo`); fields `+0x1c` error flag, `+0x34`, `+0x18` same offset on both; PS3 `+0x2b4` page mode = PS4 `+0x2c4` |
| `0xf2c8a0` / `0xf20c48` | party session / match session | PS4 `0x105efb8` / `0x1065b08` |
| `0xf1dc80` | NetGameManager (current net game index PS3 `+0x18ec`) | PS4 `0x113b760` (`+0x1ca0`) |
| `0xf119c4` (u8) | match abort byte | PS4 `0x113a270` |
| `0xf119c5` (u8) | "loading screen up" byte | loading.cpp (spec `loading.md`) |
| `0xf116d4` | net clock object pointer (clock word at object `+0xc`) | session.cpp `update_net_clock` |
| `0xf10e14` (u8) | NetInit finished | netinit.cpp |
| `0xf11bf8` (s32) | "multiplayer memory context pushed" (1 while in MP) | netheap.cpp |
| `0xe61ed8 +0x2319`, `+0x22b9` (u8) | boot / developer flags: "AutoMatchmaking" and "start in multiplayer" | none (section 2.9) |
| `0xe1cef5` (u8) | an NP Basic system dialog (invite sender) is open | none (PC has no system dialog) |

**Message box** (PS3 `0x42a3e8(menu, text, button, arg4, arg5, arg6)`): page `0x719e9fec` with body text,
button text `0xc6467f2d` ("OK"), second text `arg4`. The 7 words PS3 writes at box `+0x2c..+0x44` are the 7
words PS4 has at box `+0x40..+0x58` (remaining.cpp `message_box` writes the "no second text" form: word 6 of
that block, PS4 box `+0x54`, is where `arg4` goes). Every message box below is "body / second text".

**Party mailbox codes** (PS3 `0x4c2a18` = take a message of a code, `0x4c38f0` = send to every party member):
`0x102` join this host, `0x107` start matchmaking, `0x110` page mode, `0x114` leader pressed Start Match,
`0x115` leader cancelled. PC session messages replace them (`PORT-FRONTEND3.md` table: kMsgStart = 0x107,
kLeaderBack = 0x115, `frontend6::kStartingMessage` = 0x114, the mode message = 0x110).

**Room hide** (PS3 `0x4d5338(session, on)`, Matching2 room flags CLOSED|HIDDEN): see `PORT-MATCHMAKING.md` 1.7.
Ours: `online.cpp room_state` hides owned rooms while NET_SM is 26..30.

## 1. Rows with nothing to do (summary)

- **Net state helpers** `0x4383cc` set, `0x4383e4` get, `0x4384b0` "is online" (state > 1): PS4 inlines all
  three on `0x14ce97c`; our writers use the same word. COVERED.
- **Empty callbacks** (PS3 body is a bare return or "return 0"): `0x4384e8` (41103666 BUTTON_MULTIPLAYER_START),
  `0x4384ec` (ee8f5f9b, the member's countdown "done"), `0x4385d8` (053e5690), `0x4385dc`, `0x4385e0`,
  `0x4385e8` (callbacks passed by PS4-intact callers `0x4399e0` / `0x43fb38` / `0x443248`). COVERED.
- **Match session listener** (vtable PS3 `0xd70408`, PS4 `0x103fa50`): `0x438424` slot 4 "room gone" sets the
  abort byte to 1 (PS4 `0x5e9f10` sets `0x113a270`), `0x438440` / `0x438444` slots 7 / 8 empty (PS4
  `0x5ea040` / `0x5ea050`). COVERED.
- **TaskManagerOnline state Exits** (`PORT-GAMEPLAY3.md` 1.1): `0x438448` LoadLevels::Exit (clears the
  loading byte `0xf119c5`; also sets word `+8` of the object at `0xe9c1f0` to 2 and its byte `+0x1e3` to 1),
  `0x438478` ShowResults::Exit and `0x438488` ShowRoundResults::Exit (owner `+0xe4` = 1.0, the fade back),
  `0x4385ec` Leave::Exit (clears three MP bytes and resets one subsystem). PS4 `0x5f2e30`, `0x5f0e90`,
  `0x5f0ae0`, `0x5f0050` are intact; loading.cpp hooks `0x5f2e30`. COVERED (open question 5 on `0xe9c1f0`).
- **TaskManagerOnline state constructors** `0x43b738 .. 0x43ba44`: each allocates one state object (0x0c, 0x10
  or 0x14 bytes), stores the state's vtable, the owner at `+8` and zero in the extra word. In order: Leave
  (`0xd70438`, 0x10), ShowRoundResults (`0xd70458`, 0x10), ShowResults (`0xd70478`), ResettingReady
  (`0xd70498`), Resetting (`0xd704d8`), PreResetting (`0xd704f8`), Playing (`0xd70518`, 0x14), WaitReady
  (`0xd70538`), LoadLevels (`0xd70558`), EstablishConnection (`0xd70578`). Their only callers are the state
  factories `0x43d6b0..0x43f528`, intact on PS4 (`0x5ea250..0x5ea750`). COVERED.
- `0x438498` returns one byte of the NetGameManager (PS3 `+0x1ba4`) for menu code `0x423f20`. No behaviour of
  its own. COVERED.
- `0x438638` (56126d00): answers "is the local player the party host" (no room yet, or own member id = owner
  id). remaining.cpp `party_is_host`. COVERED.
- `0x438a10` (37c9cb76): prints "Save" on the debug channel, nothing else. remaining.cpp `save_button` logs
  "Save". COVERED.
- `0x438b3c` (705e651e) shows PAGE_MULTIPLAYER_PLAYER_OPTIONS `0xed61e469`; `0x438b80` (2c2f1bb7) shows
  `0xc89b86fc`. PS4 bodies intact (`PORT-FRONTEND2.md`). COVERED.
- `0x43a3dc` (8e2767d9): prints "Replay", then plays the game task (set `0x9f2aeafb`, task `0xad76460e`).
  PS4 `0x5ebdb0` keeps the task start. COVERED.
- `0x439a88` (bbb649ef) / `0x439b04` (bf6ce4d5), the leader cancels the "Starting in" countdown: when party
  host, mailbox 0x115 to the party and **show the party room again**; then go back one page. frontend3.cpp
  `leader_back`. COVERED except the room part (section 2.2, item c).
- `0x439b80`, the member countdown's per-frame callback: on page event 6, when a 0x115 message is waiting it
  is taken and the page goes back. frontend3.cpp (kMsgLeaderBack pops the popup or message box). COVERED.

## 2. MISSING and PLATFORM items

### 2.1 BUTTON_MULTIPLAYER `0x43b150` and the bootstrap `0x438bc8` (PLATFORM)

PS3, on the Multiplayer button:

1. Print "Multiplayer". Run the game data / content permission check (`0x93904`); while it is busy or shows a
   system dialog, stop (the player stays in the main menu).
2. Bootstrap, with the "AutoMatchmaking" boot flag as argument. Its steps, in order:
   1. Push the "multi-player" memory context, mark it pushed (`0xf11bf8` = 1). Empty the party mailbox. Zero
      netInfo `+0x2b0` (PS4 most likely `+0x2c0`).
   2. Network check (NetCtl): not connected (state other than "IP obtained") -> message box `0x88a082af` /
      OK, result "failed".
   3. PSN sign-in check: not signed in -> the sign-in prompt page `0x471007a1` with text `0x64b1c3be`, the
      system sign-in dialog is started, result "failed" (section 2.8 handles the dialog's end).
   4. Parental control: account restricted and its age below the game's rating -> message box `0x817e4e50`,
      result "failed".
   5. Load the net game data; on failure result "failed" with no message.
   6. Build the "game data version": an FNV-1a style hash (offset basis `0x811c9dc5`) over the net data
      records of one type of the loaded packages, plus the "AutoMatchmaking" string when that flag is set.
      Store it as netInfo's first word; print "Game Data Version = %x". Every room and presence carries it
      and only equal values meet.
   7. Remember the AutoMatchmaking flag; point the net clock at the game clock; show
      PAGE_MULTIPLAYER_CONNECTING `0x9e1863b1`; reset the join flags and targets (`0xe15650` = -1, several
      bytes 0); start the "NetInit" thread; enable one more subsystem (`0x775370(.., 1)`). Result "ok".
3. Bootstrap failed -> the multiplayer teardown (section 2.3). The player sees the message box over the main
   menu and is not in multiplayer.

Ours: session.cpp `button_multiplayer` (netheap, netinit, net patch, transport start, party session, NET_SM
18, page handler, packages, PAGE_MULTIPLAYER; the CONNECTING page is bypassed, `PORT-FRONTEND2.md`). The
build key is our own (online.cpp `room_key`). PSN sign-in, parental control and NetCtl have no PC
counterpart. **Parity note for the failure path**: when our transport cannot start we set netInfo `+0x1c`
and still show PAGE_MULTIPLAYER. PS3 instead shows a message box and stays in the main menu. To match, a
start failure should show message box `0x88a082af` / OK (the PS4 bank has it, it is the PS4 system-message
poller's id) and not open PAGE_MULTIPLAYER. Owner decision (open question 1).

Helpers on this path, all PLATFORM: `0x4383a4` and `0x4383fc` clear the join flags (`0xf11758`, `0xf1175c`)
and the StartConnection target (`0xe15594` = -1, `0xf12620` = 0) before a custom game search (`0x4655b0`)
or a join; `0x438fa0` (NP manager callback: on "got ticket" prints the ticket size or error and wakes the
waiting NetInit); `0x438a50` (NP Basic event callback: negative result -> prints "basicCb gets error. ret =
0x%x" and queues nothing; else appends {event, connection, argument} to a 128-entry queue, full -> prints
"too many basic handler. ignore ...."). Ours: netinit.cpp, online.cpp, session.cpp.

### 2.2 BUTTON_MULTIPLAYER_STARTMATCH `0x43aab0` (MISSING: checks before the countdown)

Inputs: page mode (netInfo PS3 `+0x2b4`), party member count (PS3 `0x4d24a8`: 1 with no room, else the used
member slots), the current net game, its game type and map, the playlist. Our `session.cpp
button_start_match` returns at once for a non-host; PS3 does not test that (members have no Start button).

**Mode 1, matchmaking** (also taken whatever the mode when the AutoMatchmaking flag is set):

1. Party size check: party members > the current playlist's maximum party size (playlist record PS3 `+0x1c`;
   playlist found by the id in the local member data, PS3 NetPlayerData `+0xc0`) -> message box `0x58b7e078`
   / `0x3a586923`, stop. The PS3 *playlists* records hold 5 for the competitive playlists (Deathmatch,
   Elimination, Plunder, Objectives, Ranked, All Competitive Modes, The Lab) and 3 for the co-op ones; their
   maximum player count (`+0x18`, read by PS3 `0x49a958`, 0 for an unknown id) is 10 and 3. Done (2026-11-25):
   menus.cpp `make_record` writes both fields, `playlist_max_party` / `playlist_max_players` read them.
2. Party host: hide the party room. **[missing]** (item c)
3. Mailbox 0x114 to the party; close an open NP dialog; the 5 s "Starting in [A]..." countdown (done
   `4055f45f`, cancel `bbb649ef`). COVERED: `frontend6::leader_starting(true)`.
4. Pick the Matching2 world for the search: from the playlist's list of world ids the first whose current
   population is below a threshold, else a random one, stored in the match session. PLATFORM: our matchmaking
   (frontend3.cpp, online.cpp `mm_begin`) has no worlds.

**Mode 2, custom game**, in this order, each failure shows its message box and stops:

1. Content of the net game (PS3 net game record `+0x7c`, a set of map pack bits) and then of its map (map
   record `+0x10`, map pack 1 or 2 used as bit 1 / 2):
   - the local player lacks it -> body `0x0e39fdf3` when the value is 1 (map pack 1), else `0x2341ea7b`;
     second text `0x13cabc15`;
   - some party member lacks it (the AND of every member's published content flags) -> `0x58d7fbb3` /
     `0x13cabc15`.
   **[missing]** On PC dlc.cpp keeps absent maps off the host's own list, so the local case cannot occur for
   maps, but members' packs are not known to the host. Needed: every member publishes its present packs
   (dlc.cpp `pack_available`) in its session info; the host checks the map's pack against all of them.
2. Too many: members > the game type's maximum (10 for types 0, 1, 2, 3, 5, 6, 7, 10, 11; 3 for co-op types
   4, 8, 9, 12; 0 for any other type, so any party fails) -> `0x4a936499` / `0x13cabc15`. **[missing]**
3. Too few (skipped when a tuning byte, PS3 `0xf2264c+0x7c`, is set; 0 in retail): members < the type's
   minimum (1 for types 0, 1, 2, 3, 5, 6, 7, 11; 2 for 4, 8, 9, 10, 12) -> `0xb90de2b1` / `0xcca3ca1f`.
   **[missing]** Conflict: our solo co-op (solocoop.cpp) starts co-op with 1 player on purpose (open
   question 2).
4. Game type 10 only: count the members of the 10 party slots that have member data; if all of them are on
   team 1, or all on team 2 (team byte `+9` of the member data; a member with team 0 breaks both counts),
   -> `0x101c7f10` / `0x5350146e`. Edge: no member with data at all also counts as "all on one team".
   **[missing]**
5. Party host: hide the party room (item c). Mailbox 0x114; close an open NP dialog; the countdown with done
   `e2707dbe`, cancel `bf6ce4d5`. COVERED except the hide: `frontend6::leader_starting(false)`.

**Mode 3, cinema**: clear one netInfo flag (PS3 `0x4a9c64(netInfo, 0)`), refresh the menu's list, print
"Cinema", play the game task (set `0x9f2aeafb`, task `0xad76460e`), exactly as `0x43a3dc`. **[missing]**: our
handler sends mode 3 into the custom game countdown and then creates a match. Only matters if Start Match can
be pressed in cinema mode on PS4's pages (open question 3).

**Other modes**: nothing.

**Item c (room hide around the countdown)**: PS3 hides the party room when the party host presses Start Match
(modes 1 and 2) and shows it again when the leader cancels (`0x439a88` / `0x439b04`). Ours hides only from
NET_SM 26 on, so for the 5 s of the countdown a stranger can still find and join the room. Wanted: hide the
owned online room at the countdown start, show it again on cancel (online.cpp owns rooms). LAN: nothing.

What the player sees: the leader presses Start Match with an unfit party and gets the message box instead of
the countdown; the members see nothing. Text ids `0x58b7e078`, `0x3a586923`, `0x0e39fdf3`, `0x2341ea7b`,
`0x58d7fbb3`, `0x13cabc15`, `0x4a936499`, `0xb90de2b1`, `0xcca3ca1f`, `0x101c7f10`, `0x5350146e` are not in
`texts.cpp`; whether the PS4 banks have them is open question 4.

### 2.3 Multiplayer teardown `0x4386d0` (PLATFORM, MISSING: releases)

PS3 order: unregister the NP Basic handler (if NetInit ran); when the party is in a room, leave it and wait
until left; score service term; abort a running cinema transfer; commerce term; party reset; match session
term; session manager term; close both sockets; signaling term; stop the player-data worker thread and wait;
wait for one more job; reset the friends list (free its buffers); NET_SM 0; voice end; free net player
data; clear one menu word (PS3 menu `+0x6fc`); NetInit flag 0; clear the AutoMatchmaking boot flag; disable
the subsystem of 2.1 step 7; close a held cinema file handle (`0xf11bf4`) if any; then, **unless** a global
keep byte (PS3 `0xe1cef1`, meaning not identified) is set:

- the "net" package: clear its "keep for multiplayer" flag and release it;
- each package of the 21-name preload list (PS3 `0xe155d0`, the list in session.cpp `preload_packages`): when
  present, clear the same flag and release it;
- release the net game level preload (PS3 `0x4a12e8(ngm, request 0, keep menu 1, flags only 0)` = PS4
  `0x641690`, ours `frontend6::preload_net_game(false, true, false)`);

and finally pop the "multi-player" memory context if pushed (`0xf11bf8` back to 0) and the "net" context.

Ours: session.cpp `teardown` (page handler, gamenet, voice, transport, online, sessions, netInfo `+0x2c4`,
net game index, ClearNetPlayers, progression publish, NET_SM 0, members, pop). **[missing]**:

1. The package releases: the "net" package (and the extra net package of `keep_net_package`) and the 21
   preload packages keep the flag our requests set (PS4 package byte `+0x18` bit `0x40`) for the rest of
   the process. Wanted: clear that bit on each package we flagged and release it the way the game releases a
   package request (PS4 function: open question 6). Order: after NET_SM 0, before the page pop. A package
   that is absent is skipped.
2. The level preload release (call above) when the module installed it.
3. A cinema transfer still running (cinema.cpp / cinema_store.cpp) is aborted.

Effect a player can notice: memory held by MP packages after leaving; it is a candidate cause of the known
"campaign after an MP match crashes" problem (memory `mp-to-campaign-reset`). Not verified.

### 2.4 Party member popup actions (MISSING: Promote, Kick, Mute)

The selected member is the one picked in a party or match row (ours `g_selected_member`; PS3 `0xf11a40` for
the party, `0xf11a1c` = row in the match player list `0xf21de8`).

- **Promote To Party Leader `0x439c7c` (2d32c8b0)**: find the selected member in the party; if found, hand
  room ownership to it (Matching2 GrantRoomOwner) **and then go to PAGE_LOBBY `0xb99a3235`** (back to it when
  it is on the page stack, else opened). Not found: nothing (no message). Ours (`remaining.cpp
  promote_leader`): `online::promote`, message box `kTextLeader` on failure, no page change. **[missing]**:
  the PAGE_LOBBY step after a successful promote. Our failure box is a PC addition (LAN has no promote).
- **Kick From Party `0x439cf8` (7cc46b94)**: find the member; kick it from the room **with the block flag**,
  so the kicked player cannot join that room again. No message. Ours (`session::kick`): kick without block.
  **[missing]**: block the kicked player's online id until the session ends, as `session::kick_griefer`
  already does (log line of that path can be reused).
- **Mute `0x439bf0` (6547d63c, party rows) and `0x439d60` (7273924c, match player list)**: find the member
  (party: by id; match list: the selected row's entry, used only when it exists, has a connection and is not
  the local player) and flip the "muted" flag of its voice connection. **No message box**; the change shows
  as the muted icon in the rows. Ours (`remaining.cpp mute_member`): toggles mute and then shows a message box
  "name: Muted/Unmuted", and with voice off shows message box `kTextVoice` instead. **[missing / differs]**:
  PS3 shows nothing. Parity: toggle silently (the roster's talking / muted icon already follows
  `voice.cpp`). Keep or drop our boxes: open question 7.

### 2.5 Lobby status handler `0x43a444` and PAGE_MULTIPLAYER_FRIENDS_LIST `0x43aa30` (PLATFORM)

`0x43a444` runs every frame from PAGE_LOBBY (`0x443bc8`, PS4 intact) and from the friends list page.

- Party host: builds a 0x28-byte status block (page mode, net game index, playlist id, current page id,
  NET_SM, four game option words and one more of the NetGameManager); when it differs from the last one, or a
  "force" byte is set (cleared then), prints "Callback_Multiplayer, Update: Lobby status changed" and the
  block goes to the members. Ours: session.cpp sends mode, roster, teams and follow messages on change.
- Member, only while no system message dialog is up:
  - 0x114 -> close an open NP dialog; the countdown (done `ee8f5f9b`, per-frame `0x439b80`). Ours
    `frontend6::member_starting`. COVERED.
  - 0x110 -> page mode = the value. Ours session.cpp posts 0x110 into the game mailbox. COVERED.
  - 0x107 -> refresh the menu, NET_SM 19 (print "NET_SM_START_MATCHMAKING"), show
    PAGE_MULTIPLAYER_MATCHMAKING `0xe35f0f1a`. Ours frontend3.cpp kMsgStart. COVERED.
  - page mode 2, not on the matchmaking page, 0x102 with valid data -> reset join flags, store the host's
    room id (netInfo `+0x58`), connect signaling to the host, NET_SM 7 (print
    "NET_SM_CLIENT_CONNECT_TO_HOST"), the wait page `0x471007a1` with "Joining Game..." (`0xd5969d29`).
    Ours: session.cpp follow path (states 12 / 14 / 17, PAGE_MULTIPLAYER_JOINING). PLATFORM.

`0x43aa30`: on page event 6, when netInfo's error byte (`+0x1c`) is set, go back one page; then run
`0x43a444`. PS4 has no registration for this page (`c3815c33`). Ours: session.cpp's handler for every page
processes the same messages. PLATFORM.

### 2.6 Party invite `0x43a164`, its text helper `0x4384f0`, counts `0x43b200`, friends helper `0x43a340` (PLATFORM)

`0x43a164` (from the invite page's per-frame callback `0x4400bc`, once the party has a room): find the chosen
friend in the friends list; take text `0x82c61354` ("Party Invite") cut to 18 characters as subject and
`0xe2069b80` (the "join my party" line) cut to 512 characters as body, attach the room id and the build key,
open the system "send message" dialog for that friend. Failure prints "Error %x sending invite to %s"; success
sets the "NP dialog open" byte. `0x4384f0` cuts a UTF-8 string after N characters without splitting a
multi-byte sequence (an encoded NUL also ends it). Ours: `remaining.cpp invite_to_party` ->
`online::invite_friend` (RPCN invite). `0x43b200` (called from NetInit and the NP Basic handler) stores the
counts of waiting invitations and of waiting data messages (cinema files) in netInfo. Ours: online.cpp
`invite_count`, `cinema_received`. `0x43a340` tells LIST_FRIENDS whether a friend has usable game presence.
Ours: `online::friend_joinable` / `friend_online`.

### 2.7 Store `0x4399a0` (a857e46b) (PLATFORM)

PS3 opens the PlayStation Store browser at the category id held in netInfo (PS3 `+0x4c`). Ours: message box
`kTextStore`.

### 2.8 Sign-in dialog watcher `0x439044` (PLATFORM)

Per-frame callback of the sign-in prompt page of 2.1 step 3: on page event 0 it remembers the "system dialog
running" byte; on event 6, when that byte has gone from set to clear (the dialog closed): signed in now ->
run the bootstrap again (no AutoMatchmaking), else go back one page. Ours: no PSN; RPCN sign-in is the
launcher's / online program's job.

### 2.9 Boot into multiplayer `0x43b1dc` (PLATFORM)

Game start-up (`0x25718`), after registering the MP controls: when one of the boot flags of section 0 is set,
it presses BUTTON_MULTIPLAYER itself. Developer / boot-option path. Ours: none (open question 8).

### 2.10 Friends stats ticker line `0x43b30c` and bounded append `0x43b290` (MISSING)

PAGE_MULTIPLAYER's bottom strip is a ticker control (PS4 kind 24; texts of `texts.cpp` "Stats ticker k24").
PS3 fills it from the per-frame NET_SM update `0x447248` through `0x446c60` (both outside this group; their
behaviour is given here so the two rows can be implemented):

- Refresh: at most every 10 s of game clock, only while the ticker is visible and has finished scrolling,
  only in multiplayer after NetInit, and only when the category texts are loaded.
- Pick a category at random, 0..6: 0 Cash `0xc0cd1b00`, 1 Kills `0x49f5e231`, 2 Wins `0xb9602dbb`, 3 Losses
  `0x966fa9ae`, 4 Deaths `0xcd3394e9`, 5 Headshots `0x99e71e1b`, 6 Time Played `0x780b62e0`.
- Text = heading `|@CFFFFFFFF|<category>:|@CFF999999|` followed by 6 spaces, then entries for up to 5
  friends picked at random (no friend twice) among those whose presence is from this game build and whose
  presence state is "in this game's multiplayer" (PS3 state 2), scanning at most 101 friends. No such friend:
  heading only. The text goes to the ticker.

`0x43b290` appends a string only if the result stays below 511 characters; it answers whether it appended.

`0x43b30c` appends one friend entry: the friend's online id, then `: |@CFFF0C828|`, then the value, then
`|@CFF999999|` followed by 5 spaces. Value by category: 0 = the currency prefix text `0x9b5b3c66` (as the
HUD uses it) followed by the cash amount formatted as the game formats money; 1..5 = the friend's count as a
plain decimal; 6 = total time played in seconds as `Nd Nh Nm` when at least one day, else `Nh Nm` when at
least one hour, else `Nm` (minutes 0..59, hours 0..23 after taking whole days); a category above 6 gives
"Unknown Value!". If any of the four appends does not fit, the buffer is cut back to its length before this
entry and the answer is "false" (the caller then stops adding friends); else "true".

Ours: nothing fills the ticker (texts only). **[missing]**: the friends' stats (cash, kills, wins, losses,
deaths, headshots, time played) are not in our presence (`online.cpp PresenceData`, compare
`parity-b-menus.md` section 5); the PS4 entry point to set the ticker's text is not identified (open question
9). LAN: no friends, heading-only text or the strip left empty (owner decision, question 9).

### 2.11 Debug only (PLATFORM)

`0x43b698`: when a debug pad bit is held, prints "Net Clock = %f" (clock in seconds) and the socket bandwidth
statistics. `0x438350`: ascending order by the s32 at `+8`, used to sort inside NetConnectionUpdate
(`0x43cdb8`). Ours: net.cpp / gamenet.cpp own packet order; no debug overlay.

## 3. Test scenarios

1. Custom game, party of 4 on a co-op map type 8 -> Start Match shows `0x4a936499` / `0x13cabc15`; party of 3
   -> countdown.
2. Custom game type 10 with all members on team 2 -> `0x101c7f10`; one member moved to team 1 -> countdown.
3. Custom game on a map-pack-2 map, one member without pack 2 -> `0x58d7fbb3`; all have it -> countdown.
4. Matchmaking with a party larger than the playlist's maximum -> `0x58b7e078` / `0x3a586923`.
5. Online host presses Start Match: the room is hidden at once (a search from another PC does not find it);
   the leader cancels: the room is found again.
6. Promote a member from PAGE_LOBBY: ownership moves and PAGE_LOBBY is the top page (popup gone). From
   PAGE_MULTIPLAYER's party panel: PAGE_LOBBY opens.
7. Kick a member online; the kicked player tries to join the same room again: refused until the session ends.
8. Mute a member: no message box; the row's muted icon changes; mute again: icon back.
9. Leave Multiplayer, then check the package table: "net" and the 21 preload packages no longer carry the
   keep bit and are released; the level preload flags are cleared.
10. Ticker with 2 friends in MP: within 10 s the strip scrolls `Kills:` (or another category) with both names
    and values; time 3725 s shows `1h 2m`; 90000 s shows `1d 1h 0m`; 59 s shows `0m`.
11. Transport start failure on BUTTON_MULTIPLAYER (if question 1 says so): message box `0x88a082af`, main menu
    stays.

## 4. Open questions

1. Start failure: show message box `0x88a082af` and stay in the main menu (PS3) instead of opening
   PAGE_MULTIPLAYER with the error byte set (ours)?
2. Minimum players for co-op custom games (PS3: 2) against our solo co-op (1 player). Keep solo co-op as a
   deliberate difference (and skip check 3 for co-op types), or follow PS3?
3. Can Start Match (70e80076) be pressed in page mode 3 on the PS4 pages, or does cinema mode only offer
   BUTTON_MULTIPLAYER_STARTCINEMA (cinema.cpp)? If it can, add the mode 3 branch of 2.2.
4. Are the eleven Start Match message ids of 2.2 in the PS4 text banks? If not, they need PC texts in
   `texts.cpp` (the PS3 bank text, translated as the others).
5. LoadLevels::Exit also writes the object at PS3 `0xe9c1f0` (`+8` = 2, `+0x1e3` = 1): does PS4 `0x5f2e30`
   still do it (object not identified)?
6. Which PS4 function releases a package request (PS3 `0xaf2cc` for one package, `0xb3df8` by name), and does
   the PS4 package keep a 64-bit flag word like PS3 (`+0x10`, bit 57) besides the byte `+0x18` our requests
   set?
7. Mute: drop our "Muted / Unmuted" and "voice off" message boxes for PS3 parity?
8. Should the launcher's "start with multiplayer" (memory `mp-auto-on-rule`) open PAGE_MULTIPLAYER at boot the
   way PS3's boot flag does, or is it only about the import?
9. Ticker: PS4 call that sets the ticker control's text (PS3 `0x42da98`, visible / idle tests `0x42d870` /
   `0x42da40`); presence fields for the seven stats; what LAN shows.

## 5. Status table

| PS3 | role | status | where in our port / what to do |
|---|---|---|---|
| 438350 | sort order by s32 `+8` (NetConnectionUpdate) | P | net.cpp / gamenet.cpp (2.11) |
| 4383a4 | clear join flags `0xf11758` / `0xf1175c` | P | session.cpp join path (2.1) |
| 4383cc | SetNetState | C | session.cpp `set_net_sm`, PS4 `0x14ce97c` |
| 4383e4 | GetNetState | C | PS4 inlined reads |
| 4383fc | reset StartConnection target (`0xe15594`, `0xf12620`) | P | session.cpp join path (2.1) |
| 438424 | match listener: room gone -> abort byte | C | PS4 `0x5e9f10` |
| 438440 | match listener slot 7 (empty) | C | PS4 `0x5ea040` |
| 438444 | match listener slot 8 (empty) | C | PS4 `0x5ea050` |
| 438448 | LoadLevels::Exit | C | PS4 `0x5f2e30`, loading.cpp (question 5) |
| 438478 | ShowResults::Exit (fade 1.0) | C | PS4 `0x5f0e90` |
| 438488 | ShowRoundResults::Exit (fade 1.0) | C | PS4 `0x5f0ae0` |
| 438498 | NetGameManager byte getter | C | no own behaviour |
| 4384b0 | IsOnline (state > 1) | C | PS4 inlined |
| 4384e8 | 41103666 BUTTON_MULTIPLAYER_START (empty) | C | PS4 ret, same |
| 4384ec | ee8f5f9b member countdown done (empty) | C | PS4 ret, same |
| 4384f0 | UTF-8 cut to N characters (invite texts) | P | online.cpp invites (2.6) |
| 4385d8 | 053e5690 (empty) | C | PS4 ret, same |
| 4385dc | empty callback | C | PS4 callers intact |
| 4385e0 | callback answering 0 | C | PS4 callers intact |
| 4385e8 | empty callback | C | PS4 callers intact |
| 4385ec | Leave::Exit | C | PS4 `0x5f0050` |
| 438638 | 56126d00 party IsHost | C | remaining.cpp `party_is_host` |
| 4386d0 | multiplayer teardown | C* | session.cpp `teardown`: keep bit 0x40 cleared on net / extra net / 21 preload packages, `preload_net_game(false, true, false)`; PS4 package release call unknown (question 6), not called; no cinema transfer to abort on PC (2.3) |
| 438a10 | 37c9cb76 "Save" (print only) | C | remaining.cpp `save_button` |
| 438a50 | NP Basic event callback | P | online.cpp (2.1) |
| 438b3c | 705e651e -> PLAYER_OPTIONS page | C | PS4 `0x5ed8d0` |
| 438b80 | 2c2f1bb7 -> page `c89b86fc` | C | PS4 `0x5eb790` |
| 438bc8 | MP bootstrap | P | session.cpp `button_multiplayer`, netinit.cpp; start failure: message box `0x88a082af`, main menu stays (2.1, done) |
| 438fa0 | NP manager callback (ticket) | P | online program (2.1) |
| 439044 | sign-in dialog watcher | P | none needed (2.8) |
| 4399a0 | a857e46b store browse | P | remaining.cpp `store_browse` box (2.7) |
| 439a88 | bbb649ef leader cancels (matchmaking) | C | frontend3.cpp `leader_back` + `online::hide_for_start(false)` |
| 439b04 | bf6ce4d5 leader cancels (custom) | C | frontend3.cpp `leader_back` + `online::hide_for_start(false)` |
| 439b80 | member countdown: pop on 0x115 | C | frontend3.cpp kMsgLeaderBack |
| 439bf0 | 6547d63c Mute (party) | C | remaining.cpp `mute_member`: silent toggle (2.4) |
| 439c7c | 2d32c8b0 Promote To Party Leader | C | remaining.cpp `promote_leader`: PAGE_LOBBY after success (back to it on the stack, else opened); failure box kept (PC) (2.4) |
| 439cf8 | 7cc46b94 Kick From Party | C | session.cpp `kick`: blocks the online id until the session ends (2.4) |
| 439d60 | 7273924c Mute (match player list) | C | remaining.cpp `mute_member`: silent toggle, not for this PC (2.4) |
| 43a164 | send party invite | P | `online::invite_friend` (2.6) |
| 43a340 | LIST_FRIENDS presence helper | P | `online::friend_joinable` / `friend_online` (2.6) |
| 43a3dc | 8e2767d9 Replay | C | PS4 `0x5ebdb0` |
| 43a444 | lobby status / party mailbox handler | P | session.cpp, frontend3.cpp, frontend6.cpp (2.5) |
| 43aa30 | PAGE_MULTIPLAYER_FRIENDS_LIST page callback | P | session.cpp page handler (2.5) |
| 43aab0 | BUTTON_MULTIPLAYER_STARTMATCH | C* | session.cpp `button_start_match`: all checks in PS3 order (playlist maximum party size 5 / co-op 3), room hide, mode 3 plays the cinema, other modes nothing; gaps: net game content bits (PS3 `+0x7c`) not checked (map pack only), mode 3 netInfo flag; deliberate: co-op starts with 1 player (2.2) |
| 43b150 | BUTTON_MULTIPLAYER | P | session.cpp `button_multiplayer` (2.1) |
| 43b1dc | boot into multiplayer | P | none (2.9, question 8) |
| 43b200 | invite / message counts | P | online.cpp (2.6) |
| 43b290 | bounded append (511) | M | online.cpp ticker builder done; nothing shows it: PS4 ticker text call unknown (2.10, question 9) |
| 43b30c | friends stats ticker entry | M | online.cpp `ticker_append_entry`, presence version 3 carries the stats; not shown (2.10, question 9) |
| 43b698 | "Net Clock" debug print | P | none (2.11) |
| 43b738 | new Leave state | C | PS4 state factories |
| 43b794 | new ShowRoundResults state | C | PS4 state factories |
| 43b7f0 | new ShowResults state | C | PS4 state factories |
| 43b844 | new ResettingReady state | C | PS4 state factories |
| 43b898 | new Resetting state | C | PS4 state factories |
| 43b8ec | new PreResetting state | C | PS4 state factories |
| 43b940 | new Playing state | C | PS4 state factories |
| 43b99c | new WaitReady state | C | PS4 state factories |
| 43b9f0 | new LoadLevels state | C | PS4 state factories |
| 43ba44 | new EstablishConnection state | C | PS4 state factories |

Counts (after the implementation of 2026-11-25): COVERED 41 (2 of them C*: covered with the gaps named), PLATFORM 17, MISSING 2. Before: COVERED 35, PLATFORM 17, MISSING 8.

`C*` = covered, with the gaps or deliberate differences named in the row.

## 6. Implementation status (2026-11-25)

Owner decisions applied: question 1 yes (start failure shows `0x88a082af`, the main menu stays); question 2
solo co-op stays (a co-op type starts with 1 player: the "too few players" check uses minimum 1 for types 4,
8, 9, 12, a deliberate difference; type 10 keeps minimum 2); question 7 yes (Mute is a silent toggle, both
boxes dropped). "MP auto-on" (question 8) unchanged.

- 2.1: session.cpp `button_multiplayer`: transport start failure -> log, close, `game_message_box(0x88a082af,
  0)`, no PAGE_MULTIPLAYER, no error byte.
- 2.2: session.cpp `button_start_match` with the checks of mode 1 (party size against
  `playlist_max_party`, menus.cpp: the playlist record's `+0x1c`, 5 for competitive playlists and 3 for co-op
  ones as in the PS3 *playlists* records; 0 = no limit only for an unknown playlist id) and mode 2 (map pack of
  the map: this PC via `dlc_net_game_pack`, every member via the packs byte our Info message now carries;
  too many; too few; type 10 one team), each with the game's text ids through `game_message_box`
  (remaining.cpp; it logs an id the PS4 banks lack, so question 4 is answered by the first run's log). Item c:
  `online::hide_for_start(true)` at every countdown start, `false` in `leader_back`; the hide also ends when
  the match runs (the match hide takes over), on leaving matchmaking and when the transport closes. Mode 3:
  `cinema::start_playback()` (BUTTON_MULTIPLAYER_STARTCINEMA's action). Other modes: nothing (PS3). Not done:
  the net game record's own content bits (PS3 `+0x7c`, PS4 offset not known) and mode 3's netInfo flag
  (PS3 `0x4a9c64`, PS4 counterpart not known).
- 2.3: session.cpp `teardown` (not on the client's "leave the match room" path, which stays in
  Multiplayer): after NET_SM 0 the keep bit 0x40 at package `+0x18` is cleared on `net`, the extra net
  package and the 21 preload packages that are present, then `frontend6::preload_net_game(false, true,
  false)`, then our own net game index reset. The preload sets the bit again on packages already present
  at the next entry. Open: the PS4 call that releases a package request (question 6); the global keep byte
  (PS3 `0xe1cef1`) has no PC counterpart. No cinema transfer can be running on PC. The PS4 level manager
  (`0x1130390`) is expected to drop a package nothing asks for any more at its next update (its slot bitmap
  `+0x80..+0x97`: free = 180 - the bits set), so clearing the bit may be the whole release. Check (diagnostics
  only, no behaviour change): 5 s after the teardown a page handler of its own logs "leaving multiplayer, 5 s
  later: of N released package(s) G gone, L still loaded, O in another state; level manager packages in use
  A -> B" (taken out of the handler table when Multiplayer is entered again). G = N with B < A confirms it.
- 2.4: Promote -> PAGE_LOBBY after a successful `online::promote` (pages above it popped, else opened);
  Kick From Party blocks the online id (session.cpp `kick`, shared with the griefer kick); Mute silent.
- 2.10: online.cpp presence version 3 (80 bytes, within the ABI's 128; versions 1 and 2 still read; a
  version 2 reader takes the first 48 bytes) carries cash, kills, wins, losses, deaths, headshots and seconds
  played; `stats_ticker_text`, `ticker_append_entry`, `ticker_time` build the text as described. Not shown:
  the PS4 call that sets the ticker control's text, and its visible / idle tests (question 9), are not
  known. Headshots: no NetPlayerData offset is known for them; the local progression store keeps the
  career count (progression.cpp `match_headshots` at Leave: the match's kill records, NetStatGame `0x1147bd0`
  count `+0xe8`, records of 0x1c from `+0xec`, kind byte 1 = a headshot, `+2` killer net id, `+4` victim net
  id; the local player's kills of kind 1, not its own deaths; competitive matches only, as the career kills;
  whether PS3 also counted co-op kills is not known), sent in presence v3 and shown as the local entry of the
  Headshots category. Time played uses the
  competitive seconds (NetPlayerData `+0x198`); whether PS3 added the co-op seconds is not known. Friends
  with an older presence (no stats) are left out of the ticker. LAN: no friends, heading only.
