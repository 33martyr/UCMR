# Player spawn (spawn.cpp, `install_spawn`)

## How a player spawn finds its spawner (PS3 1.10 as the map)

| PS4 | PS3 | what |
|---|---|---|
| 0x662fa0 | 0x4c8568 | GetSpawnLocator(Locator *out, u32 spawner id, int mode) -> bool (net-player-tracker.cpp). Spawner from the package of 0x644130, locator from 0x4bdfe0 (spawner pos/quat, parent +0x60) |
| 0x644130 | 0x49cee8 | package of the net game's level record: *net-games* [NGM+0x1ca0] +4 -> *levels* record; PS4 looks up the id at +0x20 in the package table 0x1130390, PS3 looks up the name at +0x18 in the level manager (0xb31e0) |
| (inlined) | 0xb6e10 | spawner by id: package +0x9a0 (PS3 level +0x4e4) spawner group, sorted {u32 sid, ptr} at +0x18, count at +0x20 |
| 0x644220 | 0x4a1eb0 | initial spawn: returns the spawner id (spawner +0x28). Walks the group's list (+8, count +0x10) of the same "<map>-pak" package (the lookup is inlined, 0x6442b8..0x644392) and keeps spawners tagged `online-spawner` (5bfae217) = 1, `team` (d50bc34b) = tracker +0x134 or -1, `start-spawner` (33364361), 09b2fb53. Returns **0** when the package is not loaded |
| 0x642d20 | 0x4a6008 | respawn, same tags and the same inlined lookup |
| 0x6636b0 / 0x663280 | 0x4c9880 / 0x4c9b58 | spawn remote / local character; call 0x662fa0 at 0x663840 / 0x6632d1; when it returns false the player is not spawned |

Callers of 0x6636b0: 0x646ffb (StartNetGame helper 0x646bf0: ids from the net game's coop spawn list), 0x663aae /
0x663b6b (the tracker spawn request, local tracker -> 0x663280 later), 0x623db3 (NetEventRespawnPlayer::Apply).

## Why "Missing spawner" (test run 31)

The level record of every net game is `<map>-pak` (gompa-multi-pak, village-multi-pak ...). Its package
`<map>-pak-ingame` (file `<map>-pak-ingame10.pak`) holds every multiplayer gameplay spawner of the map:
`online-spawner-*` (26-48 per map, 10-21 of them start spawners, tagged by team), `net-flag*`, `net-territory-*`,
`plundvival-*`, `ss-battle-manager-*`. The map's own packages (`<map>`, `<map>-ingame`, `-npc-ingame`) hold
**no** online spawner (gompa-multi-ingame10: 352 spawners, all fx/lights/nav blockers/props, PS3 and PS4 identical).

These packages are neither on the PS3 disc nor on PS4. PS3 downloaded them from the server: `net-patch9.bin`
(in the PS3 game data folder BCES00757DATA2/USRDIR) maps level `<map>-pak` to package `1-<map>-pak-ingame`,
and the files are `BCES00757DATA2/USRDIR/build/main/pak22/1-<map>-pak-ingame10.pak` (12 maps: facility,
gompa, highrise-multi-2, ice-cave, museum, ruins, shambhala, the-fort, train-wreck, treasure, village, warzone)
plus `stage-multi-pak-ingame10.pak`. PS4 ships only `stage-multi-pak-ingame10.pak` (PS4 format, with its 37
online spawners) and no `-pak` level in `pak22_ps4.txt`. The PS3 code that registered the downloaded levels
(net-patch) is gone on PS4.

So with no `<map>-pak` package, 0x644220 returns id 0, GetSpawnLocator is asked for 0 and our
level_record_package (remaining.cpp) hands it the map package `gompa-multi`, which has no such spawner.
(The table 0x1130390 is keyed by package/level id; `gompa-multi-ingame` is merged into level `gompa-multi`,
which is why it is never found by its own sid.)

## What spawn.cpp does

0x662fd0 (call 0x644130, its only caller) -> `spawn_package(ngm, id, mode)` (esi/edx still hold the arguments):
1. the package whose spawner group holds the id: `<map>-pak` (by record +0x20), then the map, then any package
   in the table;
2. otherwise a package of our own (only +0x9a0 is read) whose group has one entry {id, fallback}: an
   online spawner (start spawners first when mode == 0) of `<map>-pak`, of any loaded package, a `pickup-` /
   `npc-` / `continue-player` spawner of the map, any map spawner, and last a spawner of our own at the origin.
   Rotates through the candidates per call. Logs the level state once per map and each missing id once.

The assert at 0x6630dc is never reached. Without the import, players spawn at map pickups (wrong spots, not
by team) - only a safety net.

## Diagnostics (squash 934a6ca; exact rules in spec/gameplay.md 6.4 "Diagnostics" and 6.5 steps 0 and 3)

Added for the Siege (type 12) and Gold Rush hunts, on top of the search above (which is unchanged). Every
diagnostic only observes: it never changes the game's memory or registers.
- two image probes, installed before the GetSpawnLocator check: 0x4bdfad (does nothing) and 0x4bf925 (the NPC
  batch gate: logs each candidate's flags, the territory spots and the spawners near them). Both stubs resume with
  `jmp [rip+0]`, so rax is what the game left (before, it held the resume address; dead at both sites);
- 31 call-site probes after a successful install: NetNpc name (0x23bb22, only when its call bytes match), 16
  SpawnNpc sites, 12 spawner update sites, the 2 get-spawner-locator name lookups; each forwards to the game and
  logs its first calls;
- in the GetSpawnLocator host function: a game type read (*net-games* record +0x0c) at each return, before the pak
  search and after the level lines; in type 12 the spawner, candidate, level spawner list and wave spawner lines;
  once per process the net-game dump (each net game's type, level name, task list).
- `spawner_name` treats an unreadable name pointer as `?` (c5a5d89, Gold Rush).

## Runtime workaround: Siege npc-soldier spawners (spec/gameplay.md 6.5 step 0a)

The converted Siege levels ship their `npc-soldier*` spawners disabled (u16 flags +0x50 bit 1, rejected by the
batch gate 0x4bdb20) and nothing enables them, so Siege would have no attackers. `install_siege_spawner_fix`
(only when the 0x4bf925 gate stub is installed; log `spawn: siege npc-soldier spawners enabled at the NPC batch
gate 0x4bf925 ...`) makes the gate enable 4 of them per change of the zone's handle count (0x113d530), each moved
next to the roof / vault entry point nearest the active zone (the last spawned `net-territory*` spawner); logs
`spawn: zone wave: handles %d, releasing %d attackers` and `spawn: released npc spawner ...`. Origin: inside the
batch probe since 934a6ca; its own named step since 2026-11-25. **To do:** the owner rule puts map fixes into the
PS3 -> PS4 conversion: the level converter should write these spawners enabled (and placed), then this runtime
workaround goes.

Off: the per-type update slot probes, the factory probe (boot crash) and the type-vtable scan (session.cpp, PR 21).
The rewrite test (`tests/u2mp_rewrite/test_gameplay.cpp`, `sp_install_expected` / `sp_call_expected`) puts these
into the old record from the spec and compares the rest unchanged.

## What is left (the real fix, for the lead)

1. **Import** (u2import, new rows): convert `1-<map>-pak-ingame10.pak` from the PS3 game data with the
   existing `-ingame10` converter (u2ingame; item types are the same as the data paks: SPAWNER_GROUP,
   REGION_TREE, SPLINE_TABLE, FEATURE_DB_2, LEVEL_INFO_1, TAG_INT) to `pak22/ps4/<map>-pak-ingame10.pak`
   (the PS4 name, without "1-"), and add to `pak22_ps4.txt` for each map (stage too, its PS4 file exists):
   ```
   level-name <map>-pak 
   level-id <map>-pak
   package <map>-pak-ingame
   ```
   (PS3 pak22.txt's stage-multi-pak block lists the actors anim-proto-carry-objects, anim-proto-shield ...;
   copy them from there.) The import needs a `--ps3-data` source (BCES00757DATA2 / NPUA80698DATA2 folder).
2. Then 0x644130's original lookup, 0x644220 and 0x642d20 find the package, choose team start spawners, and the
   `<map>-pak` skip in session.cpp (skip_missing_level_pak) stops applying by itself. CTF flags, territories
   and the plunder chest (net-flag, net-territory, plundvival spawners) live there too.
3. Quick test without converting: The Stage only needs the registry block (PS4 retail stage-multi-pak-ingame10.pak).
4. remaining.cpp level_record_package (0x644130): its only caller is now retargeted by spawn.cpp, so it is unused.
   Leave it or drop its install; if kept it should return package_by_id(record +0x20) only (the `-ingame10` /
   `-ingame` candidates can never match: those packages are merged into the map's level).
5. Unknown: the meaning of `mode` (1..4 move the locator, 0x663064); tag 09b2fb53 (seen on gompa
   online-spawner-986, a neutral start spawner); highrise-multi-2-pak uses `spawner-start-*` names instead of
   online-spawner (tags not checked).
