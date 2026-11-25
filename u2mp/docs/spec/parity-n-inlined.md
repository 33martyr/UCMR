# Spec: PS3 parity, group N (functions the PS4 build inlined)

Clean-room check of the section "[INLINED - functionality may be in a caller, not a missing function]" of
`maintainer-tools/research/u2-parity/ps4-missing-functions-audit.tsv` (lines 690-804: 115 rows, 23 frontend-net and
92 match-gameplay; PS3 `0x468c20` is listed in both halves, so 114 distinct PS3 functions). The other sections of the
audit are handled by `parity-a` .. `parity-m`. This file says, for every row, whether the PS3 behaviour still runs on
PC and where; it does not prescribe code. Addresses without "PS3" are PS4 1.02 image addresses (file offset of
`boot-linked.bin` minus `0x183ce8`); PS3 addresses are the 1.10 EBOOT (`NPUA80698`). "NGM" is the NetGameManager
(PS4 `0x113b760`); "the net-games table" is the DC table with id `3b9a067d` (PS4 records `0x90` bytes, game type at
record `+0xc`, current index s16 at NGM `+0x1ca0`; PS3 records `0x80` bytes, index at NGM `+0x18ec`). Co-op game types
are 4, 8, 9 and 12.

Status words:

- **COVERED-INLINED**: the PS3 behaviour is present inside a live PS4 function (the one named, or the one corrected in
  section 2) and our runtime lets that PS4 code run (wrappers call the original; only byte checks or call retargets
  that keep the original's work sit inside it).
- **COVERED-OURS**: our C++ (`gow3_pc/src/u2mp`) implements the behaviour.
- **PLATFORM**: PS3 plumbing with no visible effect on PC.
- **DROPPED**: trophies, Twitter (owner decisions; no trophies on PC, the Twitter service is gone).
- **MISSING**: PS3 behaviour that neither PS4 nor our code has; a behaviour item follows in section 4.

Sources and method. Every PS3 function was decompiled once (headless Ghidra, project `u2ps3`, TOC `0xe183b8`, plus
llvm-objdump where Ghidra dropped code). For each one its string ids and constants were searched in the named PS4
function and in the whole PS4 disassembly; every PS4 function named here was checked for callers (direct calls or a
pointer in the image's pointer table) and against the addresses our runtime touches (`install.cpp`, `nullsites.cpp`,
`remaining.cpp`, `session.cpp`, `netanim.cpp`, `hud.cpp`, `unlocks.cpp`, `natives.cpp`, `spawn.cpp`, `netinit.cpp`,
`visibility.cpp`, `solocoop.cpp`, `menus.cpp`, `scoreboard.cpp`, `rosternames.cpp`, `render.cpp`).

## 1. Result

| Status | Rows |
|---|---|
| COVERED-INLINED | 97 |
| COVERED-OURS | 7 |
| PLATFORM | 3 |
| DROPPED | 7 |
| MISSING | 0 (NI1 implemented 2026-11-25) |
| total | 115 |

Our runtime replaces or wraps these PS4 hosts: the match state updates (`0x5f1000` Resetting and the other nine,
`session.cpp state_update`, original always called except while it holds the round results), WaitReady::Exit
`0x5f2830` (`cinema.cpp`, original called; `nullsites.cpp` patches only its replay-stream writer at `0x5f2a8c..`),
the profile query `0x65e890` (`menus.cpp query_profile`, original called), the tracker state helpers `0x634970` /
`0x60ded0` (`session.cpp` traces that call the originals; `netanim.cpp` stubs at other sites of `0x634970`), and the
co-op minimum at `0x63f60b` (`solocoop.cpp`, 2 -> 1, owner rule). No host named in this file is stubbed.

## 2. Corrections to the audit's PS4 column

The feature matcher paired several small PS3 helpers with an unrelated PS4 function that happens to share one common
id. The real PS4 site of the behaviour:

| PS3 | Audit says | Real PS4 site | How it was found |
|---|---|---|---|
| `0x4729f0` kill-info fill (killer = player with the most damage) | `0xaaaa40` (engine code, no NGM use) | `0x60f480` (builds the NetEventKillInfoClient: its vtable `0x10412f0` is stored at `0x60f50b`; the ten-tracker "most damage" loop at `0x60f6d0..0x60f8ec`); called from `0x7a23ed` | vtable of the type 16 Write `0x633ef0` |
| `0x4958c8` send event `fb4207d2` to every net player's character | `0x64b640` (no callers) | inline in `0x2618e0` (loop `0x261b17..0x261bf7`: `bb 28 00 00 00` .. `be d2 07 42 fb` call `0x4c4310`, stride `0x2c0`, only when the net state `0x14ce97c` >= 2); `0x2618e0` is reached through the pointer table | all five PS4 uses of `fb4207d2` |
| `0x49f9e0` round reset (clear pickup lists, state 5, kill every player character) | `0x5c8840` (script callback dispatcher) | `0x641b50` (writes 5 to NGM `+0x1e80` at `0x641f63`: `41 c7 84 24 80 1e 00 00 05 00 00 00`), called from `0x5e5bbc` (replay Resetting::Enter) and `0x5f12dd` (online Resetting::Enter) | stores of 5 to the match state |
| `0x4c9e98` death camera on a teammate | `0x407d20` | `0x664550`, called at `0x665734` / `0x6657a0` from the tracker update `0x664bc0` (`parity-m` 2.0) | callers of the tracker update |
| `0x4cbc10` tracker overlay draw (dead-player hint) | `0x5a0620` (menu system-message poller) | absent on PS4; ours (`scoreboard.cpp`, `parity-m` M1 / M2) | `parity-m` 2.0 |
| `0x49c8dc` score limit by player count | `0x5f1000` | inline in `0x63e6a0` at `0x63e707..0x63e776` (`8b 41 14 41 89 84 24 b4 21 00 00 41 83 ff 06 77`), only in matchmaking (netInfo `+0x2c4` == 1) | stores to NGM `+0x21b4` |
| `0x49e518` minimum players per game type | `0x5f1000` | inline in `0x63e6a0` at `0x63f5c4..0x63f634` (masks `0x8ef` / `0x1310`); ours for Start Match | `parity-d` 2.2 |
| `0x491ea8` lookup in table `a977dad3` | `0x64ab70` (no callers) | inline at `0x5ded06`, `0x645d5e`, `0x645e9b` | uses of the id |
| `0x4e05e0` stats packet writer | `0x642370` (no callers) | none: the SQS upload is gone on PS4 too | callers |
| `0x435468`, `0x44421c` | "TaskManagerReplay / -Online" | these are the two modules' static initializers (run with "initialise, priority 0xffff"): default values of file-scope flags, ten slot ids set to -1 (Online only) and the class registration. PS4 registers the classes lazily at `0x5e3980` / `0x5ea890` (guarded statics, handlers `0x5e3910` / `0x5ea820`) | shape of both functions |

The tiny accessors of the net-games table (`0x499858` .. `0x4a5548` rows) are listed with `0x5f1000`, `0x5f2830`,
`0x7d6170`, `0x5c8840`, `0x7a2000` and similar only because those PS4 functions also read the net-games table (the
id `3b9a067d` occurs 714 times in the PS4 code). The accessors are inlined at every PS4 use; the evidence that matters
is that every PS3 caller's PS4 counterpart is alive or replaced by our code (column "where").

## 3. Status of every row

"where" names the PS4 host (alive unless said) and the evidence: ids found inside it, callers, or the spec / file of
ours. Ids are the DC string ids the PS3 function uses.

### 3.1 frontend-net (23 rows)

| PS3 | PS4 | Role (PS3 behaviour, short) | Status | Where / evidence |
|---|---|---|---|---|
| 439df8 | 5ec010 | Playing::Update helper: round-time bookkeeping of the local NetPlayerData (two counters clamped, a deadline from tunables), then the TUS publish | COVERED-INLINED | all five ids `33d2252a d34ac18e 6badead5 7265ff1f be2ca34b` at `0x5ec021..0x5ec05c` (`bf 2a 25 d2 33 e8`); `0x5ec010` is called at `0x5f1700` inside Playing::Update `0x5f14d0` (session.cpp calls the original). The TUS publish (PS3 `0x4c6298`) is PLATFORM (`parity-e` 3.2) |
| 43a398 | 5f1000 | value lookup by the id pair `9f2aeafb` / `d3b86d60`, used by the lobby-wait / custom-game / matchmaking code | COVERED-INLINED | both ids inside `0x5f1000`; callers: `0x465b70` = PS4 `0x609d00` (`parity-f`), `0x465da0` PLATFORM (`parity-f` F2), `0x4b0a58` ours (frontend3.cpp) |
| 44421c | 5ea890 | TaskManagerOnline static initializer | COVERED-INLINED | section 2 |
| 466fe0 | b88ec0 | PS3 transport: decrypt 8-byte blocks, check the 20-byte digest | PLATFORM | our transport (net.cpp / online program) has its own framing |
| 467160 | b88ec0 | PS3 transport: pad, digest, encrypt | PLATFORM | as above |
| 468c20 | 634970 | "is this anim id one of the excluded set" (two id ranges and nine single ids) | COVERED-INLINED | range tests `8d 88 06 f5 1c 4b 83 f9 0f` at `0x6350c9` (15 ids from `b4e30afa`) and `0x6350d8`, again at `0x637874` / `0x637fce`; single ids inside `0x634970` |
| 491d70 | 5f6880 | find a row by id in table `829f99c6` (36-byte rows) | COVERED-INLINED | id inside; `0x5f6880` called at `0x871a7c` |
| 491e00 | 5f1000 | return the net-games table | COVERED-INLINED | inlined at every use (`bf 7d 06 9a 3b` + call `0xa15e70`) |
| 491e30 | 5b0070 | row k of table `5a4731d2` (0x24 bytes) | COVERED-INLINED | id inside; callers `0x5afe62`, `0x5b03bb` |
| 491ea8 | 64ab70 | find a row by id in table `a977dad3` | COVERED-INLINED | section 2 |
| 491f38 | 5f2830 | find a map record by id (table `0dec97a4`, 40-byte rows) | COVERED-INLINED | id inside; WaitReady::Exit original runs (cinema.cpp) |
| 491fc4 | 559850 | find an entry by id in the `9353ed16` list | COVERED-INLINED | id inside; `0x559850` reached through the pointer table |
| 492058 | 655810 | row k of table `f985c7b6` (0x44 bytes), else a zeroed static row | COVERED-INLINED | id inside; callers `0x655355`, `0x65550c`, `0x6557be` |
| 4b7b3c | 65ecc0 | mode label list: pick the entry for a slot, hiding two special entries when their condition fails | COVERED-INLINED | `1842529c` inside; `a9ac1ac2` compared at `0x65ee24`, `0x65ef13`, `0x65f0bf`; `parity-b` row 4bc310 |
| 4b7e80 | 658070 | "%i  %s" row text (row number + net game name) | COVERED-INLINED | struct-size multiplier `3fb47f69` inside; rows of `parity-b` 4bafa0 / 4bb170 |
| 4b8cc0 | 659bd0 | booster list of a slot (always-free ids `05454b23`, `01845694`, else unlocked) | COVERED-INLINED | ids inside; lock answer from unlocks.cpp IsLocked (`parity-b` 4be788) |
| 4b9774 | 658070 | result / film row text (five text ids, minutes:seconds) | COVERED-INLINED | `0fa7ca1a fb7b7911 10fea3a7 4a8de2ea c040a9ee` inside |
| 4b99ac | 655f90 | banner lookup by two names (table `a4b36cdb`) | COVERED-INLINED | id inside; `parity-b` "%s/banners/%s" |
| 4bd4a8 | 65bf00 | co-op results text dispatcher (state 3, co-op game) | COVERED-INLINED | `0x65bf00` reached through the menu query table |
| 4eeee0 | 675d90 | Twitter auto-post day counter ("[#EARNED]" templates `b2bbfe1d`, `b67ae3aa`, `9b5b3c66`) | DROPPED | Twitter (`parity-k` covers the page) |
| 4efc2c | 5b0410 | Twitter page "Test Account" label (`4db5091c`) | DROPPED | Twitter (`parity-k` 1.5) |
| 4efc6c | 5b0410 | Twitter Options entry label (`168207da`) | DROPPED | Twitter (`parity-k` K1) |
| 4efd00 | 9ef610 | "user:password@" text for the Twitter HTTP login | DROPPED | Twitter; also PLATFORM |

### 3.2 match-gameplay (92 rows)

| PS3 | PS4 | Role (PS3 behaviour, short) | Status | Where / evidence |
|---|---|---|---|---|
| c2030 | 5779a0 | MedalManager: award a medal (rules table `980d6543`, mode flags, count / modulo, show the pop-up; for some medals also counts a trophy stat) | COVERED-INLINED | 14 of 17 ids inside (the 3 others are id+1 range ends); 48 PS4 callers. Its trophy-stat calls (`0x4c7388`) are DROPPED |
| 173ff4 | 9ef610 | set a sound parameter (`92ff7969`) | COVERED-INLINED | id inside; `0x9ef610` in the pointer table |
| 1741cc | 9ef610 | switch sound `a6effd08` on, or off with a fade | COVERED-INLINED | id inside |
| 174270 | 9ef610 | switch sound `ebbf45b6` on / off | COVERED-INLINED | id inside |
| 174998 | 94fa90 | walk table `74bfa7d0` and apply every unflagged entry | COVERED-INLINED | id inside; callers `0x6042ec`, `0x604396`, `0x607e71` |
| 435468 | 5e3980 | TaskManagerReplay static initializer | COVERED-INLINED | section 2 |
| 4533f8 | 5f48e0 | remote anim: sample the snapshot list at the current time | COVERED-INLINED | callers `0x222d0f`, `0x6050ad` (FUNCTIONS-MATCH-GAMEPLAY: the live remote-animation path) |
| 4539f8 | 246d10 | anim generator helper: find the two snapshots around now, collect the anim events between them, interpolate | COVERED-OURS | its only PS3 caller `0x453c10` is the generator slot whose PS4 copy `0x5f48d0` is `ret`; `netanim.cpp` slot 4 host (`netanim.md` 1.4.3, `parity-c` row 453c10) |
| 455128 | 62e090 | skin table `916e03b0`: id of row k | COVERED-INLINED | id inside |
| 4551b0 | 62e090 | skin table: row k | COVERED-INLINED | id inside |
| 45521c | 62e090 | skin table: row of an id | COVERED-INLINED | id inside |
| 4554c0 | 62e090 | skin table: list (max 64) of usable rows | COVERED-INLINED | id inside |
| 455cb8 | 5776c0 | tunable `b5aa25f5` x value, rounded | COVERED-INLINED | id inside; callers `0x57760b`, `0x57808c` |
| 455dc8 | 5776c0 | same rule, second user | COVERED-INLINED | id inside |
| 4561f4 | ae4d80 | two tunable multipliers (`898bc681`, `330539cb`) with rounding | COVERED-INLINED | both ids inside; four callers |
| 4564a4 | 7edd80 | tunable `7bb853bb` x value | COVERED-INLINED | id inside |
| 4564dc | 96bc80 | tunable `9a925faf` x value, rounded | COVERED-INLINED | id inside |
| 45684c | 659bd0 | skin table: picture of row k except the two always-free ids | COVERED-INLINED | all three ids inside |
| 456900 | 65e890 | local player's hero / villain skin -> row index | COVERED-INLINED | id inside; menus.cpp `query_profile` calls the original |
| 4570f0 | 6db600 | create a net object of class `7339d08c` | COVERED-INLINED | id inside |
| 457ec8 | 6bc0c0 | play an animation by name | COVERED-INLINED | in the pointer table; 28 PS3 callers |
| 457f30 | 5f9900 | start an anim layer (`7efa5a21`, blend `230015b3`) | COVERED-INLINED | both ids inside |
| 45ac00 | 605070 | show / hide the six attached props (`f57ba7ff` / `271af020`) | COVERED-INLINED | both ids inside |
| 45b338 | 712e60 | place the attach points (`a5c6681c`, `d5132d8f`) | COVERED-INLINED | both ids inside; caller `0x971b75` |
| 45b9a8 | 5fb7b0 | NetCharacter::Init part: look set `cc7f36e6`, hero-chloe model flags | COVERED-INLINED | ids inside; render.cpp / netinit.cpp only check bytes there |
| 45bc40 | 289650 | character setup: 14 attribute ids | COVERED-INLINED | all 14 inside |
| 45ed60 | 605070 | per-frame remote character pose / aim update | COVERED-INLINED | the three ids inside |
| 461238 | 603cf0 | remote character update (weapons, carried objects) | COVERED-INLINED | six ids inside, `9df97f47` at `0x603fa9` |
| 468c20 | 634970 | (second listing of the row above) | COVERED-INLINED | as 3.1 |
| 470af4 | 62f670 | SetJuggernaut helper for a `9df97f47` object (weapon hand-over) | COVERED-INLINED | `9df97f47` at `0x62f683`; callers in `0x62f4a9..0x62f582` |
| 4729f0 | aaaa40 | kill-info fill: flags, clock, killer = most damage | COVERED-INLINED | `0x60f480` (section 2) |
| 484cc8 | 60ded0 | player state snapshot for the state send | COVERED-INLINED | `5ec9a3e7` inside; session.cpp trace calls the original |
| 48c3d0 | 612770 | carry-object client start-move | COVERED-INLINED | both ids inside; caller `0x4633f5` |
| 48eb80 | 60f970 | send a player event with the carried object | COVERED-INLINED | four callers |
| 493d30 | 642d20 | respawn helper: which tracker, its spawn spot ids | COVERED-INLINED | all three ids inside; called by HostUpdate `0x63ea37` |
| 494608 | 63e6a0 | tracker visibility state machine (seen / hidden timers `c032f430`) | COVERED-INLINED | id inside; the probes it reads are kicked by visibility.cpp (`gameplay.md`) |
| 494930 | 63e6a0 | NetGameManager::ClientUpdate: visibility, plus debug-only pickup counters | COVERED-INLINED | visibility as above; the counters are printed only with a debug flag: no retail effect |
| 4958c8 | 64b640 | send `fb4207d2` to every net player's character | COVERED-INLINED | `0x2618e0` (section 2) |
| 497570 | 671cd0 | test over the NGM object list for a `62fd1e7e` object of the player's team that the player holds | COVERED-INLINED | id inside; hud.cpp only checks bytes in `0x671cd0` |
| 497c48 | 5b0070 | table `5a4731d2`: row of an id | COVERED-INLINED | id inside |
| 499858 | 5f1000 | net-games table: index of an id | COVERED-INLINED | inlined at the PS4 uses |
| 499908 | 5f1000 | current net game record | COVERED-INLINED | as above (33 PS3 callers) |
| 499988 | 5f1000 | net game record by index | COVERED-INLINED | as above |
| 499de0 | 559850 | `9353ed16` list: name text of an id | COVERED-INLINED | id inside |
| 499e90 | 645c50 | `a977dad3` table: text of an id | COVERED-INLINED | id inside; caller `0x632f51` |
| 499f60 | 5b0070 | `5a4731d2` row k: its id | COVERED-INLINED | id inside |
| 49b858 | 66c600 | union of the map flags over the playlist's net games (playlist from the player data, or the default `0b70ed28`) | COVERED-INLINED | inline at `0x640451`, `0x642478`, `0x64a0fd`, `0x65a94a`, `0x66c7dc`; PS3 caller `0x4b0a58` is ours (`parity-f`) |
| 49c458 | 63dc30 | SetNetGame (limits, flags, level packages) | COVERED-INLINED | ids inside; called by PS4 and by our code |
| 49c8dc | 5f1000 | score limit by player count | COVERED-INLINED | `0x63e707` (section 2) |
| 49cae8 | 7d6170 | co-op wave time left, 1 -> 0 | COVERED-OURS | its PS3 caller is the HUD draw `0x8aef68`; `hud.cpp wave_left` |
| 49cee8 | 5f2830 | level of the current net game's map | COVERED-INLINED | ids inside |
| 49d000 | 647a50 | StartNetGame: create the game's pickups and objects | COVERED-INLINED | five ids inside; spawn.cpp wraps the spawner calls at `0x647c62`, `0x647dc6`, `0x6484e8` and still spawns |
| 49dbf8 | 647a50 | StartNetGame: objects of game types 5 / 7 (`58609830`) | COVERED-INLINED | id inside |
| 49dfc0 | 5f1000 | "is the map's content present" | COVERED-INLINED | PS3 callers `0x4350ac` (`parity-l`, PS4 `0x5e9420`) and `0x440478` (LoadLevels, original runs) |
| 49e0e8 | 65b830 | time limit in clock units, 0 for co-op types | COVERED-OURS (NI1, implemented 2026-11-25: `online.cpp match_end_now`) | the PS4 callers keep it; our presence (PS3 caller `0x490338`) lacks the co-op rule; the HUD caller `0x8aef68` is ours and shows no score panel in co-op |
| 49e1c0 | 7d6170 | a float of the current task, else of the net game record (most likely the respawn delay) | COVERED-INLINED | PS3 caller `0x4bf4a8` = PS4 `0x65b830` (original runs, scoreboard.cpp only checks bytes) |
| 49e2b0 | 5f1000 | current co-op task's script id, default task fallback | COVERED-OURS | natives.cpp `net-coop-script-id` (`parity-c` row 4cd8ac) |
| 49e368 | 5f1000 | current co-op task's host script id, fallback | COVERED-OURS | natives.cpp `net-coop-host-script-id` (`parity-c` row 4ccab8) |
| 49e4a0 | 5f1000 | game type of the current net game | COVERED-INLINED | inlined at every use (40+ PS3 callers) |
| 49e518 | 5f1000 | minimum players per game type | COVERED-OURS | Start Match: session.cpp `type_min_players` (owner rule: 2, co-op 1); in-match check inline in `0x63e6a0` (section 2), co-op lowered by solocoop.cpp |
| 49e620 | 5f1000 | room size per type (10, co-op 3) | COVERED-INLINED | PS3 caller `0x444bb0` = PS4 `0x5eb830` |
| 49e6e0 | 5f2830 | map record of the current net game | COVERED-INLINED | ids inside |
| 49eb80 | 647a50 | StartNetGame: remember the flag object (`ad66fbf6`) | COVERED-INLINED | id inside |
| 49f058 | 5f1000 | current co-op task record | COVERED-INLINED | PS3 callers `0x4caa28` / `0x4cca70` / `0x4cd850` = PS4 `0x6640a0`, `0x669780`, `0x6697e0` |
| 49f114 | 63e6a0 | CheckRoundOver (5 s after round start; one team left; time) | COVERED-INLINED | second 1500-tick test at `0x63f761` |
| 49f450 | 5f1000 | maximum players per type (10, co-op 3) | COVERED-OURS | session.cpp `type_max_players` (`parity-d` 2.2) |
| 49f510 | 5f1000 | co-op task step: two calls on the co-op score object, the first only when the current task has its value field set | COVERED-INLINED | PS3 caller = CompleteTask Apply PS4 `0x62d820` |
| 49f958 | 5f1000 | type 11 flag | COVERED-INLINED | inlined |
| 49f9e0 | 5c8840 | round reset | COVERED-INLINED | `0x641b50` (section 2) |
| 4a0bcc | 7a2000 | "is a co-op type" | COVERED-INLINED | inlined at every use |
| 4a0c98 | 5f1000 | co-op type or a page-mode test | COVERED-INLINED | inlined |
| 4a1120 | 5f1000 | team test for the local player | COVERED-INLINED | inlined |
| 4a1790 | 640fc0 | end the net game: state 0, kill characters, clear lists, fade audio | COVERED-INLINED | callers `0x5e9b3a`, `0x5f2e7d`; netinit.cpp only checks bytes |
| 4a49e8 | 63e6a0 | CheckGameOver | COVERED-INLINED | audit's manual check (`0x63f31d..0x63f3e5`); co-op player tests at `0x63f529..0x63f637`, minimum lowered by solocoop.cpp |
| 4a5548 | 5f1000 | "is a competitive type" | COVERED-INLINED | inlined |
| 4a8af8 | 63e6a0 | NetGameManager::HostUpdate | COVERED-INLINED | audit's manual check; `0x63e6a0` called at `0x5e730e`, `0x5f226f` |
| 4c5268 | 661a50 | trophy stat: award the trophy when the counter reaches its target | DROPPED | trophies |
| 4c7388 | 661a50 | trophy stat: count +1 | DROPPED | trophies |
| 4c75e8 | 661a50 | trophy stat: set a bit | DROPPED | trophies |
| 4c8448 | 664bc0 | tracker fade value per frame | COVERED-INLINED | tracker update `0x664bc0` called at `0x63fa43` |
| 4c9e98 | 407d20 | death camera on a teammate | COVERED-INLINED | `0x664550` (section 2) |
| 4cab90 | 664bc0 | tracker helper: one branch for the local player, another for the rest | COVERED-INLINED | in the tracker update; session.cpp only traces the call at `0x664d6a` |
| 4cbc10 | 5a0620 | tracker overlay draw (scoreboard entries, dead-player hint) | COVERED-OURS | scoreboard.cpp (`parity-m` M1 / M2; their open parts stay there) |
| 4dd340 | 66d8c0 | medal check `f69e35a3` | COVERED-INLINED | ids inside; `0x66d8c0` called at `0x645c91` |
| 4dd888 | 66d8c0 | medal check `a8932480` (kills within a distance) | COVERED-INLINED | ids inside |
| 4de5d0 | 66d8c0 | medal check `4a9da8a5` | COVERED-INLINED | ids inside |
| 4de7d8 | 66d8c0 | medal checks `f2c76c63` / `4bae48b4` (kills within a time) | COVERED-INLINED | ids inside |
| 4df898 | 66cb70 | skill rating update after a match | COVERED-INLINED | caller `0x648c61` |
| 4e05e0 | 642370 | stats packet writer for the SQS upload | PLATFORM | section 2; `parity-e` row 43bc30, PORT-GAMEPLAY3.md |
| 4e2630 | 66d8c0 | NPC kill points (co-op / arena score) | COVERED-INLINED | all 18 ids inside |
| 4e3e10 | 671cd0 | type 7 carry test (`62fd1e7e`) | COVERED-INLINED | id inside |
| 4e3f60 | 671cd0 | an id looked up for the player's net slot (`b0f0e030` when the slot is unset) | COVERED-INLINED | id inside |

## 4. Behaviour items

### NI1. Presence: no match end time in co-op (PS3 `0x490338` -> `0x49e0e8`) - MISSING, implemented 2026-11-25 (`online.cpp match_end_now`)

What PS3 does: when it writes the presence block (`parity-g` 1.2), the match end time comes from a helper that
answers "time limit in net-clock units" for the current net game. The helper answers 0 when there is no current net
game record, and also when the game type is co-op (4, 8, 9 or 12) whatever the record's time limit; otherwise it is
the NGM time-limit seconds x 300, truncated to an integer. A 0 answer leaves the end time 0, so friends see no
"Match ends in [TIME]" line for a co-op game (`parity-b` 4: nothing when the end time is 0).

PS4 facts: the NGM time limit (float seconds) is at NGM `+0x21b8`; the current net game index (s16) at NGM `+0x1ca0`;
the net-games table through the lookup `0xa15e70` with id `0x3b9a067d` (count at `+0`, records at `+8`, `0x90` bytes
each, type at `+0xc`). The same co-op set is tested by the PS4 HostUpdate: `0x63f60b` `b9 02 00 00 00 ba 10 13 00 00`
(mask `0x1310` = bits 4, 8, 9, 12).

Ours: `online.cpp match_end_now()` returns 0 only when the limit is not above 0; a co-op game with a time limit in its
record gets an end time.

Rule to implement: `match_end_now()` returns 0 (and resets its kept value) when the current net game index is out of
range, or the record's game type is 4, 8, 9 or 12, or `int(limit x 300)` is 0 or less. Everything else stays as it is.
Test: host a co-op game (any variation) with a friend looking at the friends list or the friend card: no "Match ends
in" line; a competitive game still shows it.

## 5. Open points

1. In-match minimum (PS4 `0x63f529..0x63f637`, PS3 `0x4a49e8`): only co-op games end for too few players (fewer than
   at the start, or fewer than the co-op minimum, which solocoop.cpp lowers to 1). Competitive games never end for
   player count there. That matches PS3; the owner rule "minimum 2 in every mode" is applied at Start Match and in
   matchmaking only. Whether a competitive match should also end when it drops below 2 is an owner question (not a
   PS3 behaviour).
2. The PS3 role of `0x439df8` was read only as far as needed (it is intact in PS4 `0x5ec010` and runs); its two
   NetPlayerData fields are not named here.
3. `0x4729f0`'s PS4 site `0x60f480` was identified by the event's vtable and the "most damage" loop, not compared
   field by field (offsets differ between the builds).
