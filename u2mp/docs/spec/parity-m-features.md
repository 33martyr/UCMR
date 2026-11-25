# Spec: PS3 parity, group M (player-visible features: menus, loading screen, spectating, leaderboard tables, assets)

Clean-room gap survey of what a PS3 1.10 multiplayer player sees, measured against the UCMR U2 runtime
(`gow3_pc/src/u2mp`, branch `u2mp-ps3-parity-3`) and the import (`u2mp/patcher/importer`). It states what the
PS3 game shows and what game memory must hold; it does not prescribe code. Conventions as in `parity-f-party-mm.md`:
addresses without "PS3" are PS4 1.02 image offsets; "s" are real seconds; the net clock counts 1/300 s; "NGM" is the
NetGameManager (`0x113b760`, PS3 object read through the module data pointer); "the tracker overlay" is the object
at `0x104f544` (PS3: the object passed to `0x4cbf00` / `0x4cbc10` from the per-frame net display `0x447248`).

Status words: **COVERED** (same visible result already; spec / file named), **PLATFORM** (Sony plumbing replaced by
our design), **MISSING** (visible behaviour we lack or do differently; exact difference named), **CHECK** (code is
present and should match; only a run can tell).

Sources: PS3 `EBOOT.elf` (headless Ghidra, TOC `0xe183b8`, plus llvm-objdump for the parts Ghidra folded), PS4
`eboot.elf`, PS4 `dc1/ps4/menu.bin` and `net.bin`, the imported English text banks
(`recipe/work/u2data/build/main/text1/ps4/2.*`), our docs (`MENUS.md`, `PORT-FRONTEND4/5.md`, `PORT-HUD.md`,
`IMPORT.md`, `ASSET-INVENTORY.md`, `TEST-LOG.md`) and specs (`loading.md`, `hud.md`, `gameplay.md` 1, `menus.md`,
`progression.md`, `parity-a..i`). Third-party notes (`maintainer-tools/research/u2-parity/zip-survey.md`) were used
only as hints; every fact below was checked against the PS3 or PS4 binary or data.

## 1. Area status

| Area | PS3 | Ours | Status | Where |
|---|---|---|---|---|
| MP menu pages (every page of menu.bin) | 40 MP pages; same controls, conditions and labels on PS3 and PS4 (`MENUS.md` 1) | every page reachable on PS3 is reachable, except the four below | COVERED, with M7-M9 | `MENUS.md` 2-9, `parity-b/d/e/f/g` |
| Loading screen | black fill, card table in perspective, fort picture, coins, edges, rosters sliding in, band, map name, description, letter-by-letter tip | all of it, PS3 timings | COVERED (three PS3 details left out on purpose, M10) | `loading.md`, `PORT-FRONTEND5.md` |
| Death camera / watching others | native camera kinds (net death, follow teammate, map spot), D-pad cycles | the PS4 code still has them and runs them | COVERED (CHECK) | M5 |
| Dead-player hint line | "\|SELECT\| Scoreboard    \|D-PAD\| Camera" at the bottom while the death camera runs | nothing | **MISSING** | M1 |
| Scoreboard / result overlay rules | four overlay entries, priority, PS3 fade and slide, open / close sounds, auto-open rules | own approximation (timers, other slide direction, no sounds) | **MISSING** (parts) | M2, M3 |
| Respawn countdown / co-op tries | in the scoreboard header (query cond 44) | the query is intact; shown only when the scoreboard is open | CHECK, depends on M2 | M4 |
| Film spectating HUD | score panel and the watched player's name at the top, boosters on the left, compact camera bar | theater bar only | CHECK / possibly MISSING | M6 |
| Leaderboard tables | 4 boards, global / friends filter, "my page", 11 rows, Rank / badge / name / score columns, footer Filter / Previous / Next / Go to Top | local + RPCN boards, same list object | COVERED | `progression.md` 1.5.11-1.5.15, `parity-a` |
| MP assets (images, icons, sounds, texts, models) | GOTY net.pak, hud.pak, hud.bnk, music stingers, MP vox, DLC voices | net-u2mp.pak (96 images), stingers, DLC voices, all texts | COVERED (two known holes, M11) | `IMPORT.md` audit, `ASSET-INVENTORY.md` |

Ranked list of the work (section 6 repeats it with files).

## 2. Spectating and the dead player's screen

### 2.0 Facts

- PS3 1.10 and PS4 1.02 both carry the camera kinds `CameraControlNetDeath`, `CameraControlFollow2`,
  `CameraControlFixed`, `CameraControlExplosionDeath`, the DC tunable block `death-cam` (PS4 `dc1/ps4/anim-net-player.bin`)
  and the script native `force-death-camera`. The PS3 net player tracker (net-player-tracker.cpp) starts a death
  camera for the local player: either a fixed camera at a spawn spot (PS3 `0x4c830c`) or a camera that follows a
  teammate (PS3 `0x4c9e98`), picked and advanced by its update (PS3 `0x4ca228`); the handle of the running camera is
  kept in the tracker (PS3 tracker `+0x15c`, the process check id at `+0x160`). The same logic is present in the PS4
  binary (camera update called from the tracker frame update `0x664bc0`; teammate camera `0x664550`; spot camera
  `0x664200`; D-pad advance from the update). No PS4 stub was found on that path.
- Loading tip `4fc75197` (net.bin tips): "Help your teammates after death by calling out enemy positions. Use
  |D-PAD| to cycle through the death cameras." The PS4 tips table still carries it.
- Text `e2186a0c`: "|SELECT| Scoreboard    |D-PAD| Camera". Present in every PS4 text bank (greek / czech / turkish
  get ours, `texts.cpp kMenu`), **referenced by no PS4 code or data**. On PS3 its only user is the tracker overlay
  draw `0x4cbc10`.
- PS3 per-frame order (`0x447248`): tracker overlay update `0x4cbf00`, tracker overlay draw `0x4cbc10`, then the
  loading screen (`0x4adaf8`, `0x4ae638`). The PS4 frame has neither tracker call; our `scoreboard.cpp` replaces them
  (`gameplay.md` 1). Correction to `functions-match-gameplay.tsv`: its pairings `0x4cbf00 -> 0x5e8e70` and
  `0x4cbc10 -> 0x5a0620` are wrong. `0x5e8e70` is the theater-camera callback handler (it compares the theater
  camera ids `0x3cf5deed`, `0xff502160`, `0x887c7d6e`, ...); `0x5a0620` is the menu system-message poller
  (`FUNCTIONS-FRONTEND-NET.md`). Both tracker functions are absent on PS4.

### M1. Dead-player hint line (PS3 `0x4cbc10` second half, fade in `0x4cbf00` end) - MISSING

What PS3 shows: while the local player is watching through a death camera, the line
"|SELECT| Scoreboard    |D-PAD| Camera" (text `e2186a0c`, the button glyphs drawn by the game's text printer) fades in
at the bottom of the screen, and fades out when the player respawns, the round ends or a menu takes over.

Behaviour to implement:

1. A hint fade value h in 0..1, process-wide, starts at 0; it lives with the scoreboard overlay (PS3 tracker overlay
   `+0x4`).
2. Every frame (the scoreboard frame hook, after the scoreboard work), with dt = elapsed seconds (the same dt the
   scoreboard uses), h rises by `1.0 x dt` when ALL of the following hold, else it falls by `2.0 x dt`; then it is
   clamped to 0..1:
   - no menu page is blocking the HUD (PS3: the menu manager's word at `+0x5b4` is 0; the same word gates the
     loading-screen rosters, `loading.md` 1 "not in scope"; its PS4 field is open question 1);
   - there is a local player tracker (NGM `+0`, the local tracker pointer) and it has a running death camera (the
     tracker's death-camera handle resolves to a live process);
   - the NGM match state is "playing" (PS3 NGM `+0x1a8c` == 2; PS4 `ngm+0x1e80` == 2, as `scoreboard.cpp` reads);
   - NOT "every active player is out": for each of the 10 trackers that is active, at least one of its three
     out-of-play conditions holds (it died after its last spawn, its character carries the "killed" state flag, or
     its character carries the second down state flag; PS3 `0x494178` called with a threshold of 0, conditions
     `0x4c892c`, `0x4c8aa4`, `0x4c89f8`). When every active player is out, the hint fades.
3. Drawing, every frame while h > 0, except when the current net game's game type is 11 (the Machinima / cinema
   type: nothing drawn):
   - overlay alpha A = 1.0 when the NGM match state is 3, 4 or 5, or the game is co-op (game types 4, 8, 9), or the
     game type is 3 or 8; otherwise A = the HUD fade value (PS3 `0xe1a660`; most likely the PS4 value
     `scoreboard.cpp` already reads at `0x104ed74`: open question 2);
   - the text is drawn white with alpha `A x h`, scale 0.8, colour-transform offset slot [5] = 3.0 (as the loading
     screen's map name, `loading.md` 3.5), centred in the rectangle x 0, y 500, w 1280, h 240 of the 1280 x 720
     menu canvas, with the same text-in-rect call and mode values (1, 1, flags 0, empty clip) the loading screen
     uses (`loading.md`, PS3 `0x4153d0` = PS4 `0x5b5450`). PS3 measures the text first (`0x4157a8`); that has no
     visible effect.
   - When TextDb has no text for `e2186a0c`, nothing is drawn.
4. PC buttons: the line shows the game's own glyphs for SELECT and D-PAD. Our scoreboard button is the touchpad by
   default (`UNCHARTED_U2MP_SCOREBOARD_BUTTON`); the glyph the text printer draws for `|SELECT|` must be the one of
   the button that really opens the scoreboard (open question 3).
5. Log once per process when the hint first becomes visible: `scoreboard: death camera hint shown`.

### M2. Scoreboard and result overlay rules (PS3 `0x4cbf00` / `0x4cbc10` first half) - MISSING (parts)

The overlay has four entries (PS4 data at `0x104f544 + 8 + 0x14 x k`: u8 shown, f32 fade, f32 speed, u8 dim flag,
u32 page id). PS4 data: entry 0 = PAGE_MULTIPLAYER_SCOREBOARD (`f650325d`, speed 1.0, no dim), entry 1 =
PAGE_MULTIPLAYER_ROUND_RESULTS (`5dad5a68`, dim), entry 2 = PAGE_MULTIPLAYER_RESULTS (`3d437466`, dim), entry 3 =
PAGE_MULTIPLAYER_ROUND_RESULTS again (dim). Our `scoreboard.cpp` draws one page with its own rules.

PS3 rules that differ from ours (each a visible difference):

1. **Fade speed.** Every entry fades toward its shown flag at `speed` per second (fade += speed x dt when shown,
   -= when hidden, clamped 0..1). Speed is 1.0, and entry 0 gets 2.0 from the moment the player toggles it by
   button. Ours: in at 4.0/s, out at 2.0/s.
2. **Which page draws.** Only the highest entry with fade > 0 draws (3 before 2 before 1 before 0), with alpha
   `A x fade` (A as in M1 3).
3. **Slide.** While its entry is shown (fading in): ease = 1 - (1 - fade)^2 and the page's y offset = -20 x (1 - ease)
   (it slides down into place from 20 units above). While hidden (fading out): ease = fade^2, y = +20 x (1 - ease)
   (it slides on downward). x is 0. Ours slides in from below and out upward.
4. **Background dim.** When the drawn entry has its dim flag (entries 1..3), the game's menu background dim is set
   to `ease` for that frame (PS3 `0x421b00`: menu manager `+0x5d0` = value, `+0x5d8` = 1, once per frame). Ours does
   not dim. PS4 field: open question 4.
5. **Sounds.** Toggling entry 0 by button plays a menu sound: `d9b465e1` when it opens, `2f118f31` when it closes.
   An automatic open / close plays nothing. Ours plays none.
6. **Automatic opening (entry 0) while playing (NGM state 2), not in a film:** the scoreboard opens by itself (speed
   1.0, no sound) when the "auto" condition becomes true, and closes again when it becomes false, unless the player
   opened it by button. The auto condition (PS3 `0x6b6b3c`) is read from the local player's object: true when the
   player object gives no answer to the state query `1a9688ee`, or the answer's state flags carry bit 15. Our reading:
   it is true while the player is dead and waiting to respawn, which is when PS3 shows the "Respawn in [A]..."
   header (M4). Open question 5 (one RPCS3 check settles it). It is forced false while the HUD is in the state PS3
   tests with `0x8a0348` (unidentified) and the board was not opened by button. Also, while every active player is
   out (M1 2, last point), entry 0 is shown.
7. **Button toggle.** A press of the scoreboard button toggles entry 0 (speed 2.0, sound), except while the
   blocking conditions hold (PS3: a second pad bit held, or an unidentified global word non-zero); when not toggled
   in state 2, entries 2 and 3 are hidden.
8. **Match states.** NGM state 3: entries 0, 1, 3 cleared, entry 2 (RESULTS) shown. State 4: entry 3 (ROUND_RESULTS)
   shown, the others cleared. State 5: entry 3 shown when the NGM round sub-state (PS3 `+0x1a94`) is 3, else entry 0.
   Three request bytes of a net object (PS3 object at `0xf2264c`, `+0x54` -> entry 2, `+0x55` -> entry 3, `+0x56` ->
   entry 1) also show one entry and clear the rest. A DC switch (`e68ae001`) set true clears every shown flag.
   In a cinema game (type 11) only states 3 / 4 show entries 2 / 3. Ours uses the round-end word `ngm+0x1e84` and
   fixed timers (12 s, 60 s) instead; keep that until the PS4 fields of these states and request bytes are mapped
   (open question 6), then replace the timers by the state rules.

Not a difference: the page draw itself (`gameplay.md` 1.4 "Draw the page") matches PS3 `0x423410`.

### M3. Sounds and dim are part of M2 (no separate work).

### M4. Respawn countdown and co-op tries in the scoreboard header (PS3 cond 44 query `0x4bf4a8`, PS4 `0x65b830`) - CHECK

PS3 title text of the scoreboard page header (op 1 only), for the local player:
- competitive, NGM state 2, the net game's respawn delay above 0: "Respawn in [A]..." (`a251dfd5`) with [A] = whole
  seconds left (delay from the net game, counted from the player's death time), "Respawning" (`61f6f20d`) at 0;
- co-op with the tries counter running (NGM states 2 or 5): "[A] Tries Remaining" (`b9ce5fc4`) when 2 or more,
  "Last Try" (`0a7e5b3a`) at 1, nothing at 0;
- shown only while the local player is killed / down, or (co-op) the tries flag is set.

The PS4 query is intact (`MENUS.md` 2, cond 44) and all four texts are in the banks. So the player sees the countdown
exactly when the scoreboard page is up while dead. With our button-only scoreboard it is never seen unless the player
opens the board. M2 rule 6 fixes that. Test: die in Team Deathmatch, the board opens with the countdown and closes
on respawn; co-op: the tries line.

### M5. Death camera and cycling (native) - COVERED (CHECK)

Nothing to implement; the PS4 code is intact. To check in a run on two PCs (and in co-op): after a death in
Deathmatch the net death camera, then respawn; in Elimination and co-op, after death the camera follows a living
teammate, D-pad moves to the next teammate, the camera moves on by itself when the watched teammate dies, and falls
back to a spawn-spot camera when no teammate lives. Names over heads (`hud.md` 4.9) and markers keep working from the
death camera's position (`visibility.cpp` uses the camera position, `gameplay.md` 3). Known risk (not PS3 parity): the
camera process creation can fail when the default process heap is exhausted after long sessions; if a run shows an
assert in CreateControlFromName with the follow / spot camera, treat it as a robustness item, not parity.

### M6. Film spectating HUD (theater) - CHECK / possibly MISSING

PS3 in a film shows the normal match score panel and the watched player's name at the top and the booster icons on
the left, besides the compact theater bar. Ours draws the theater bar (`hud.md` 4.11) and the HUD parts whose PS3
gates allow film mode. To do: in a film, compare with a PS3 / RPCS3 capture whether the score panel, the watched
player's name and the boosters appear; if not, read the PS3 HUD Draw `0x8aef68` gates for flag `0x100000` (film)
and give the PS4 port the same gates. A third-party tester report names the same three parts (hint only, verify). Open question 7.

## 3. Menus: pages without a PC route and small items

Every MP page of PS4 `menu.bin` was listed and checked against our docs and code. All pages a PS3 player reaches are
covered by `MENUS.md`, `menus.md`, `parity-b/d/e/f/g` and `CINEMA.md`, except:

### M7. Mute Players (PAGE_MULTIPLAYER_MUTE_PLAYERS, from the in-match pause page) - CHECK

PAGE_PAUSE_MULTIPLAYER: Options, Mute Players (BUTTON_MP_MUTE_PLAYERS), Leave Game (-> LEAVEGAME_CONFIRM); in a film
also Render Settings, Camera, Fog, Lighting. The Mute Players page has two team lists LIST_MUTEPLAYERS0 / 1 (title
MENU_TITLE_MUTE_PLAYERS); their rows come from PS4 code that is still present (ids at `0x5ed078`, `0x5ed0cc`,
`0x5ed0fa`, in the page handler). What must be true: each list shows the players of that team (the NGM trackers,
names as on the scoreboard, `gameplay.md` 2), selecting a row toggles that player's mute in our voice layer
(`voice.cpp`, same state as the lobby popup Mute, `parity-e-flow2.md` 2.7 and `parity-h-session.md` H7). Check in a
match: rows filled, toggle heard. If rows are empty, the cause is the source list the handler reads (map it, then
spec it here).

### M8. Pages that stay unreachable on PC - owner decisions, nothing to do

- PAGE_FRIENDACTIONS (PSN friend card actions): replaced by View Profile and the popups (`MENUS.md` 3.5).
- PAGE_MULTIPLAYER_EULA, PAGE_MULTIPLAYER_MOTD (server pages; MOTD decided "none").
- PAGE_FRIENDSTATS ("COMPARE FRIENDS"): no control of PS4 menu.bin opens it and its id is in neither binary as code;
  dead on PS3 1.10 as well. The visible friend stats are the ticker (`parity-d-flow1.md` 2.10).
- PAGE_TWITTER_MESSAGES: PLATFORM (`remaining.md`).

### M9. PAGE_MULTIMEDALS (BUTTON_MULTIMEDALS, label MENU_MEDALS, title "MULTIPLAYER MEDALS") - CHECK

Two controls of PS4 menu.bin open it (records at menu.bin `0x53b0`, `0x5660`, beside the PS4 collection's bonus
pages), neither binary refers to its id in code. Decide after one look in the game: if the button appears on a
multiplayer page (PS3) the medals list must read the MP medals of NetPlayerData; if it is only on the PS4
single-player bonus pages, nothing to do. Open question 8.

## 4. Loading screen and leaderboards

### M10. Loading screen leftovers - COVERED (two details may be added)

`loading.md` covers every element. Its "not in scope" list: (a) PS3 letterbox bars: only drawn when the display has
fewer than 719 lines: never on PC, nothing to do; (b) the tunable read at the start of the PS3 draw (`0x158ff8`, id
`cdf70794`, default 30): identify its use before deciding; (c) "rosters only while a menu field is 0": the field is
the menu manager word PS3 `+0x5b4` that also gates M1. Once open question 1 maps it on PS4, apply it to both.

### Leaderboard tables - COVERED

Columns, pages, filter and footer are the game's own list (`0x113d960` object, `progression.md` 1.5.11-1.5.15 and
`parity-a` 2.2-2.4). The rank badge column is image type 0x27 (`menus.md` 2.9). Nothing more is visible on PS3.

## 5. Assets

### M11. Asset holes - COVERED except two known

The import audit (`IMPORT.md` "Audit of what MP / co-op still lacks") checked HUD, menu and net images, particles,
hud.bnk sounds, MP banks, music cues, MP vox and DLC voices, per-mode actors, dc modules and texts against GOTY. Left:
- `sniper-towers-multi-ingame` and `train-wreck-multi-tdm-ingame`: named by the registry, in no copy: nothing to
  import.
- `rank-badges/default2` (ranks above 63): absent in PS3 and PS4, so PS3 drew nothing either.
- New with M1 / M2: no asset needed. Text `e2186a0c` is in every bank; the two sounds of M2 (5) must be found in the
  PS4 sound banks by id (open question 9).

## 6. Ranked missing features

1. M2 rule 6 + M4: scoreboard opens by itself while dead (with the respawn countdown / co-op tries header).
2. M1: dead-player hint line "|SELECT| Scoreboard |D-PAD| Camera".
3. M2 rules 1-3: PS3 fade speeds, entry priority, slide direction.
4. M2 rule 5: open / close sounds on the scoreboard toggle.
5. M2 rule 4: background dim behind the result pages.
6. M2 rule 8: result pages from the NGM states instead of timers (after open question 6).
7. M6: film spectating HUD parts (check first).
8. M7: Mute Players lists (check).
9. M9: Multiplayer Medals page (check).
10. M10 (b): loading-screen tunable (identify).

Files an implementation touches: `gow3_pc/src/u2mp/scoreboard.cpp` (M1, M2, M4), possibly a small helper shared with
`loading.cpp` for the text-in-rect call and the menu-blocking word (M1, M10); `hud.cpp` (M6, only if the check fails);
`remaining.cpp` / `voice.cpp` (M7, only if the check fails); `gow3_pc/src/u2mp/u2mp_internal.h` if shared helpers are
added; docs: `u2mp/docs/spec/gameplay.md` 1 (scoreboard behaviour updated), `functions-match-gameplay.tsv` rows of
`0x4cbf00` / `0x4cbc10` (pairing correction), `MENUS.md` 3.7 / 5.

## 7. Test scenarios

1. Team Deathmatch, two PCs: A kills B. On B: death camera; the board opens by itself (fade 1/s, slides down from
   above), header "Respawn in 5..." counting down, then "Respawning"; the hint line fades in at y 500 (1/s); on
   respawn both go (hint 2/s). No sound on the automatic open.
2. Same, B presses the scoreboard button while alive: board opens at 2/s with sound `d9b465e1`; press again: closes
   with `2f118f31`.
3. Elimination: B dies, watches A; D-pad cycles to the next living teammate. The hint stays while any active player
   is still in play and fades only when every active player is out.
4. Round end (Elimination / Plunder): result page drawn over the board (priority), background dimmed.
5. Co-op Arena: down / die: "[A] Tries Remaining" / "Last Try" in the header.
6. Machinima / film: no hint line; M6 comparison.
7. Pause during a match: Mute Players lists both teams; toggling mutes voice.
8. A pause / menu page up while dead: hint fades out (menu-blocking word).

## 8. Open questions

1. PS4 field of the PS3 menu manager word `+0x5b4` (non-zero while a menu page blocks the HUD); shared with
   `loading.md`'s roster gate.
2. Is PS4 `0x104ed74` (used by `scoreboard.cpp` as the fade scale) the PS3 HUD fade value `0xe1a660`?
3. Which glyph the text printer draws for `|SELECT|` on PC, and whether it should name our scoreboard button
   (touchpad by default).
4. PS4 fields of the menu background dim (PS3 menu manager `+0x5d0` value, `+0x5d8` set-this-frame flag).
5. The auto-open condition (M2 rule 6): confirm on RPCS3 that the PS3 board opens by itself on every death in
   Team Deathmatch (and closes on respawn). If not, it means something narrower and rule 6 must be re-read.
6. PS4 fields of the NGM round sub-state (PS3 `+0x1a94`), of the three result request bytes (PS3 object `0xf2264c`
   `+0x54..+0x56`), and the meaning of DC switch `e68ae001`.
7. M6: what exactly PS3 shows in a film (capture needed).
8. M9: where BUTTON_MULTIMEDALS sits (MP page or PS4 bonus page only).
9. Names / banks of sound ids `d9b465e1` and `2f118f31` on PS4 (expected in the menu or HUD bank).
