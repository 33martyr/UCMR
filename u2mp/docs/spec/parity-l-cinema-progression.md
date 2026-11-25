# Spec: PS3 parity, group L (cinema playback, career storage, trophies)

Clean-room behaviour description of the PS3 1.10 functions of `cinema.cpp` (the multiplayer film / theater
playback task), `net-player-data.cpp` (career save and publish), the trophy manager and one "net stat game"
accessor, measured against what PS4 1.02 and our runtime do. It says what the player must see and what game
memory must hold; it does not prescribe code. Conventions as in `cinema.md` and `parity-a-leaderboards.md`:
addresses are PS4 1.02 image offsets unless marked "PS3"; "C" = the cinema manager `0x113a990`; "stream" = the
replay stream `0x113a258` {data, bits @+8, max bits @+0xc, bytes @+0x10}; "NPD" = NetPlayerData `0x11474f8`,
"Data" = the career block at NPD+0x44 (u32 version 10, then 0x224 bytes); "NET_SM" = s32 `0x14ce97c`; log
texts are printf formats passed to `u2mp::log` and must match exactly.

Sources: rows of `maintainer-tools/research/u2-parity/groupL.tsv`, plus (read for this spec only) the PS3 replay
Go methods and state functions `0x433d3c..0x433f70`, `0x436028`, `0x436220`, `0x437fb0`, the trophy thread
loop and its callers (`0x173860`, `0x427e28`, `0x4c5268` and the two stat updaters that tail-call it), the PS4
functions `0x5e9630`, `0x5e8740`, `0x5e5c80`, `0x5e56a0`, `0x661a50`, `0x661c10`, `0x661dd0`, and the PS3
trophy list of NPUA80698 (`TROPDIR/NPWR00462_00`). Name ids are the CRC-32 (0x04c11db7, MSB first, init 0) used
everywhere: `05a6eec1` = "Resetting", `2fdd8926` = "RePlay", `b630d01c` = `*net-trophies*`.

Already specified and implemented (not repeated): `cinema.md` (recorder, store, menus, E1..E20),
`CINEMA.md` sections 1-5, `parity-a-leaderboards.md` (every NetPlayerData row of this group and `4e3648`; its
sections 2.6, 2.7 and open questions 1, 5, 6, 7 stay the owner's decisions), `remaining.cpp` B28/B29 (cinema
files to and from friends over the online program).

## 1. Status table

COVERED = same visible result already. PLATFORM = Sony plumbing or a dead Naughty Dog server; our design
replaces it (named), with the listed deltas. MISSING = player-visible PS3 behaviour we lack or break.

| PS3 | Role (what it does) | Status | Where in our code / to do |
|---|---|---|---|
| `1731a0` | trophy manager: read the unlock state of the own trophy list into a one-byte-per-trophy cache | PLATFORM | PS4 trophy manager (`0xb894a8` family, cache read by `0x661a50`) + runtime `trophy_unlock_state` |
| `173290` | trophy manager: check another title's trophy list (NPWR00449): 1 error, 3 none unlocked, 4 some unlocked, 5 one of its trophies 44..47 unlocked; result kept in the manager | PLATFORM (no equivalent) | sent from the menu code `0x427e28`; not multiplayer; open question 4.4 |
| `1735b8` | trophy manager thread: commands 0 init (context, handle, register; one register error ends the thread), 1 unlock trophy n then refresh the cache, 2 refresh, 3 the check above | PLATFORM | PS4 manager does the same for its own list; multiplayer awards (L-T1, 2.2) dropped by the owner: PC has no trophies |
| `1739f0` | trophy manager term (stop thread, drop handle/context/queue, trophy term) | PLATFORM | PS4 manager / runtime |
| `173b88` | trophy manager init (zero fields, event queue of 127, port) | PLATFORM | PS4 manager / runtime |
| `433c54` | replay Leave::Update: back to the menus once 150 frames passed with nothing loading | COVERED | PS4 `0x5e5490` |
| `43402c` | send a film to a friend: system message (subject "Check out this Uncharted 2 cinema file!", tag "Cinema File") carrying the sender's server folder and the film's server name; failure only logged ("Error %x sending cinema message") | PLATFORM | remaining.cpp `cinema_send` (`0x5e4510`) -> online.cpp `send_cinema`; deltas 2.3 |
| `434324` | download thread: copy the chosen server film into the local folder (page text "Downloading to Local Files..."), then refresh | PLATFORM (no equivalent) | no film server; 2.3 |
| `434608` | replay Resetting::Enter (150-tick fade, reset the net game's round state, a roster flag) | COVERED | PS4 `0x5e5b00`; the file stays open through it (L-C1) |
| `4346cc` | replay Resetting::Exit | COVERED | PS4 `0x5e5ad0` |
| `434848` | replay Leave::Enter (speeds 1.0, close menus, end the net game, clear "quit") | COVERED | PS4 `0x5e5520` + E9 close |
| `4348f0` | RePlaying::Enter ("multi-player-cinema", start net game, cinema camera, clock reset unless resuming after Resetting, re-find the followed player by name) | COVERED | PS4 `0x5e89d0` |
| `434e50` | upload thread: copy the chosen local film to the player's server folder, then add it to the server list | PLATFORM (no equivalent) | no film server; 2.3 |
| `4350ac` | RePlay::Update: pump; once every player is loaded wait the settle time, then go RePlaying | COVERED | PS4 `0x5e9420` |
| `4355d0`, `4355e0` | static init / term of the module's globals (compiler) | COVERED | PS4 static init `0x5ef970` |
| `4355f0` | RePlaying event handler: the theater controls (play, pause, slower, faster, restart, follow next player, free camera, HUD) and three queries | COVERED | PS4 handler (all 13 ids, e.g. `0x5e904a`); Restart reopens the film through E21 (L-C1) |
| `435be8` | RePlay::Enter: net state REPLAY, reset the stream, **open the selected film from its start**, wait for data, check tag 5, set the game, rebuild the players from the header, reset the clock | COVERED (L-C1 done) | PS4 `0x5e9630` lost the open; cinema.cpp E21 (slot `0x100a7d8`) reopens the film before it; E8 removed: 2.1 |
| `4c5e08` .. `4c7820` (7 rows) | NetPlayerData HDD copy, TUS slot write, Publish, FetchTusData, term, TUS init, Connect | as parity-a | COVERED 4 (`4c5e08`, `4c6298`, `4c6d18`, `4c7820`), PLATFORM 3 (`4c5f48`, `4c65f8`, `4c6e18`); never write TUS slots 1/2 of NPWR00462 in our format (parity-a 2.6) |
| `4e3648` | net stat game vtable slot returning 16 (PS4 `0x673900` returns 32) | COVERED | parity-a open question 6; extra fact: PS3 vtable `0xd71f98`, slot 9 (+0x24) |

Counts (26 rows): COVERED 15 (with `435be8`, done as L-C1), PLATFORM 11 (5 with an equivalent, 3 without, 3
NetPlayerData as parity-a), MISSING 0. The delta L-T1 of `1735b8` is dropped for good (owner, 2026-11-25: PC
has no trophies; trophy code stays untouched).

## 2. Behaviour to implement

### 2.1 L-C1: a film opens from its start on every entry into RePlay (DONE; PS3 `435be8`, `436028`, `437fb0`)

Implemented in `cinema.cpp` (E21, E8 removed so the install count stays 20; `cinema.md` 4.5-4.7, 5) and tested on
the fake image by the new-only scenarios L-C1a..h of `gow3_pc/tests/u2mp_rewrite/test_cinema.cpp`. Not yet seen
in the game.

PS3 facts (what the player sees):

- The replay task's states go RePlay -> RePlaying, and from RePlaying to RePlay again in two cases: the film
  reached its end marker (u32 time 0), or the player chose Restart in the theater controls (`682c6868`). Both
  restart the film from the beginning, so **a film loops until the player quits** (pause menu -> Leave).
- A recorded round reset sends RePlaying -> Resetting -> RePlaying; the film continues without reopening.
  Resetting also goes to RePlay when the end marker comes during it.
- RePlaying::Exit tears down (speeds, camera, net game) and closes the film unless the next state is
  "Resetting" (`05a6eec1`). PS4 `0x5e8740` keeps that test (its close call was dropped).
- RePlay::Enter first zeroes the stream's bit cursor and byte count, then opens the selected film (C+0x1a4) and
  waits until the first data arrived. PS4 `0x5e9630` zeroes bits (+8) and bytes (+0x10) and then calls the pump
  and sleeps 1 ms in a loop until bytes != 0; the open is gone.

What our runtime did before L-C1 (static analysis, not run): E19 opened the film and filled the stream before
PlayTask; RePlay::Enter then zeroed the counters and the next pump appended data from the middle of the file, so
the tag check read mid-stream data and left the film (a film shorter than the first fill hit end of file and
the wait loop never ended). E8 closed the film on every RePlaying::Exit, so the end of a film, Restart and every
recorded round reset ended in the endless wait loop.

Behaviour required:

1. New entry E21: slot `0x100a7d8` (RePlay state vtable `0x100a7d0` + 8, same slot layout as E8's
   `0x100a7a0`), expected `0x5e9630`, swapped like the other slots (name `"RePlay::Enter"`). It does, in order:
   open the selected film for reading (below), then call the original RePlay::Enter with the state.
2. Opening for reading (used by E19 and E21) no longer puts any data into the guest stream: it closes a playback
   file that is still open, reads the 0x4b0-byte header into C+0x1c8 and leaves the file positioned at the first
   stream byte. Flags on success as today (C+0x94 = 1, C+0x95 = 0, C+0x00 = 1). The first pump after the game
   reset the counters fills the stream from the film's first stream word, exactly as on PS3.
3. When E21's open fails (selected out of range, file gone or unreadable): log `"cinema: cannot reopen %s;
   the film ends"` (path, or `"(none)"` when out of range) and arm an "empty film" mark. While the mark is set
   and no file is open, the pump puts one zero u32 into the stream (bytes = 4), sets C+0x95 = 1 and clears the
   mark. The game then reads tag 0 != 5 and goes to Leave; nothing spins.
4. RePlaying::Exit (E8) must not close the film. Either drop E8 (installed count one lower) or keep it as a
   plain call of the original. The film is closed only by E9 (replay Leave::Enter) and by the next open.
5. E19 keeps its rule "the film cannot be read -> stay on the page, no PlayTask", and its log lines.
6. On every successful E21 open other than the first one after E19, log `"cinema: replay restarts %s"` (path),
   so tests can count loops.

Nothing else changes: the PS4 code keeps the speeds, cameras, followed player (re-found by name on RePlaying
entry) and the settle wait of RePlay::Update (900 ticks of the clock it reads).

### 2.2 L-T1: multiplayer trophies (DROPPED; PS3 `1735b8`, `4c5268`, stat updaters)

Dropped for good by the owner (2026-11-25): PC has no trophies. Nothing below is implemented and no trophy code
is changed; the text stays as a record of the PS3 behaviour. PAGE_NET_TROPHIES and its Trophies button
(BUTTON_NETTROPHIES on PAGE_MULTIPLAYER) are DROPPED (owner) too: the import's menu overlay removes the button, so
the page named below is never shown on PC (MENUS.md 3.1, 3.8).

PS3 facts:

- The PS3 list NPWR00462 has 71 trophies. Multiplayer ones: 46 (complete one competitive game), 47 (complete
  one co-op game), 48..59 (group 001, needs the Drake's Fortune multiplayer pack: medal counts, mode wins,
  waves, kills, co-op Hard), 60..70 (group 002, needs the Siege expansion: medal counts, Siege waves, co-op
  Crushing). Names, texts and icons are in the player's own PS3 TROPHY.TRP; never ship them.
- Progress lives in the career Data: stat slots 90..111 (u32 at Data+8+4*slot, so it is saved, published and
  shown on PAGE_NET_TROPHIES with the rest of the career). The DC table `*net-trophies*` has one row per slot
  (row = slot - 90): threshold, PSN trophy id, a game-mode mask.
- Add rule: only while no film plays (net game manager film flag; PS4 ngm+0x209c), only for slots 90..111, and
  only when the row's mode mask matches the current mode: slot += 1, then check. Bit rule (map sets such as
  "all three co-op maps"): slot |= 1 << bit, then check.
- Check: when the row's trophy is not yet unlocked (manager cache) and slot >= threshold, ask the trophy
  manager to unlock it (command 1). The system shows the trophy popup once; later checks see the cache.

PS4 / our runtime today: PS4 kept all three (add `0x661dd0`, called from the medal path `0x577be1`, `0x5781d3`,
from `0x5f08d7`, `0x62d320..0x62d34c`, `0x66e18e..0x66e1cd`; bit `0x661c10` from `0x62d38b`; check `0x661a50`)
and sends the unlock to its own trophy manager (`0xb89540`, command 1, the row's id) when the byte at
`0x1059640+0x39c2` is set (the medal path reads the same byte). The PS4 check also resets Data to a new profile
when NPD state is 3 while NET_SM <= 1 (PS4 remnant, parity-a 1.5.5). Our runtime's `sceNpTrophyUnlockTrophy`
(`runtime_services.c`) marks that number in the PS4 Uncharted 2 list and shows its popup. The PS4 list has no
multiplayer trophies, so with PS3 ids in the table a multiplayer match would award whichever PS4 single-player
trophy has the same number (to verify, 4.1).

Behaviour required (minimum, independent of the owner's choice in 4.2):

1. No multiplayer progress may unlock a trophy of the PS4 list. Unlock requests that come from the
   `*net-trophies*` check go to the multiplayer award below, never to the PS4 trophy manager.
2. The progress rules above stay exactly as PS4 has them (same slots, same table, same film and mode gates), so
   PAGE_NET_TROPHIES and the saved career keep matching PS3.
3. Multiplayer award (owner choice 4.2, default "own list"): a separate "Uncharted 2 Multiplayer" trophy list of
   the 25 PS3 trophies 46..70, built at import from the player's own PS3 TROPHY.TRP (names, texts, grades,
   icons), stored and shown by the runtime like the PS4 lists. Award = mark PS3 id n earned once (saved per
   user), show the runtime's earned popup for it, log `"progression: multiplayer trophy %d earned"`. A request
   for an id already earned logs nothing and shows nothing. Without an imported list: log
   `"progression: multiplayer trophy %d reached (no trophy list imported)"` once per id per session.
4. The "already unlocked" answer the check reads (manager cache byte for the id) must come from this
   multiplayer store, so an earned trophy is not requested again on every later increment.

### 2.3 Film server, upload, download, send (PLATFORM; PS3 `434324`, `434e50`, `43402c`)

PS3 had a Naughty Dog file server: Upload copied a local film to the player's server folder (it then appeared
in the server list, toggle `fb637366`); Download copied a server film back into the local list ("Downloading to
Local Files..."); Send to Friend sent an XMB message that referred to a server film, so any friend (also an
offline one) could fetch it later from the in-game Messages entry. The server is gone.

Our design (keep): Upload and Send both open the RPCN friend picker and send the whole local file through the
online program; the receiver stores it in the local list at once and the Messages entry counts it. Deltas the
owner may want (4.3): (a) offline friends cannot receive; PS3 messages waited; (b) no server list (the toggle
shows the local list only); (c) PS3's texts: the receiver's notice could use the PS3 subject text
`4162699b` and the tag `1a587883` already in texts.cpp instead of our English sentence; (d) PS3 showed no
in-game error when sending failed (only a log line); ours shows a notice, which is acceptable as a PC message.

## 3. Test scenarios

- L-C1a film larger than 128 KB: start -> the first record read is the stream start (tag 5), players from the
  header, events play from time 0. L-C1b film smaller than 64 KB: starts and plays, no hang.
- L-C1c film reaches its end: plays again from the start, log "replay restarts" once per loop; three loops,
  one open file at any time.
- L-C1d Restart in the theater controls mid-film: from the start, followed-player camera kept by name.
- L-C1e film of a multi-round Elimination match: Resetting between rounds, file stays open, no log line.
- L-C1f quit from PAGE_PAUSE_CINEMA: Leave, file closed (E9), back to the cinema list after 150 frames.
- L-C1g film removed while looping (reopen fails): "cannot reopen" log, Leave, no spin.
- L-C1h start with an unreadable film: stays on PAGE_CINEMA, no task (unchanged E19).
- The stream side of L-C1a..h runs in the rewrite test (`run.sh cinema cinema.cpp cinema_store.cpp`, new-only
  scenarios "L-C1 ..."; a fake RePlay::Enter zeroes the counters, pumps and reads the tag). The camera kept by
  name (d), the 150-frame return (f) and multi-round content (e, open question 5) are game side: check in game.
- L-T1a..e: dropped with L-T1 (2.2).

## 4. Open questions

Questions 1, 2 and 4 are closed: L-T1 and every trophy question are dropped (owner, 2026-11-25: PC has no
trophies).

1. Which trophy ids does `*net-trophies*` hold in the net data the import installs (PS3 GOTY / net-patch9):
   PS3 46..70 as assumed? Check one match with a stock runtime: does boot.log show
   `Runtime: trophy 46 of list N earned`, and which PS4 trophy is that? Also: what is the gate byte at
   `0x1059640+0x39c2`, and is it set during our matches?
2. Multiplayer award: own imported "Uncharted 2 Multiplayer" list (2.2 step 3), or progress only (no popup)?
3. Film sharing: keep direct sends only, or add a server film store (needs an online-program service; the
   community server is not ours) and offline delivery?
4. `173290` (another title's list NPWR00449, its trophies 44..47): what the result enables in the PS3 menus
   (`0x427e28` sends the request). Not multiplayer as far as seen; confirm before calling it irrelevant.
5. PORT-GAMEPLAY3.md says PS3 `0x443e40` is the online Resetting::Enter, not ShowResults::Enter; `cinema.md`
   E6 finishes the recording at ShowResults::Enter. Which state ends a PS3 recording decides whether multi-round
   films contain the later rounds (owner of the online-flow group to settle; affects L-C1e).
