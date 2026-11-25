# Spec: PS3 parity, group N (the open items of groups M and J)

Clean-room follow-up of `parity-m-features.md` items M6, M7, M9, M10 (b) and `parity-j-voice-medals.md` J5, J7 and
open question 7 (J1 topology). It states what a PS3 1.10 player sees and hears, the PS4 1.02 facts an implementer
needs (addresses, offsets, bytes to verify before patching) and the PC rule. It does not prescribe code structure.
Conventions as in `parity-m-features.md`: addresses without "PS3" are PS4 1.02 image offsets (file offset of
`boot-linked.bin` minus `0x183ce8`); "NGM" is the NetGameManager `0x113b760`; trackers are `0x113b770 + i * 0x2c0`.

Sources: PS3 `EBOOT.elf` (headless Ghidra plus a raw PPC word dump for branch senses), PS3 `dc1.psarc:menu.bin`
(NPUA80698), PS4 `eboot` image, PS4 `dc1/ps4/menu.bin`, PS4 `actor27/ps4/*.pak` (animation names), the imported
English text banks. Everything below was read from those files; nothing was run in the game.

Owner rules that bound this group (unchanged by it): minimum 2 players in every mode, 1 in co-op; trophies out of
scope; dead online services (Twitter posting, Message of the Day) stay hidden. None of the items below touches them.

Status words as in group M: **COVERED**, **PLATFORM**, **MISSING**, **CHECK**.

## 1. Summary

| Item | Answer | Status | Work |
|---|---|---|---|
| N1 (M6) film spectating HUD | PS3 film HUD = the normal HUD (score panel, kill feed, names over heads, markers, centre message) plus the theater bar, plus the followed player's boosters **only while the follow camera is active (cinema mode 0)**. There is no separate "watched player's name" element. Ours draws the film boosters in the wrong camera modes (condition inverted). | COVERED (done 2026-11-25; was one inverted condition) | hud.cpp: one condition (done) |
| N2 (M7 + J5) Mute Players page | Rows are the scoreboard team lists (same list kind, same layout `dcae45f4`), already filled. Selecting a row must toggle that player's voice mute; PS4 finds the row and does nothing. J5's "scoreboard mute action `0xb04c828b`" is this page's list id, not a scoreboard action. | COVERED (done 2026-11-25; CHECK in a match) | remaining.cpp list callbacks; J4 icon (scoreboard.cpp) |
| N3 (M9) Multiplayer Medals | PS3: the button is on the no-save main menu PAGE_NEWPROFILE (and PAGE_NEWPROFILE_BETA) only, after Options, with a visibility condition; not on PAGE_MULTIPLAYER, not on PAGE_PROFILE, not on Bonuses (Bonuses has the single-player Medals). Our menu overlay turns exactly that button into Multiplayer, so the page is unreachable on PC. | COVERED in the import (overlay version 6); visibility and list content CHECK | overlay.cpp (done), page check (open) |
| N4 (M10 b) `cdf70794` | Not a setting. Each frame the PS3 loading-screen draw publishes its "shown" byte under the name id `cdf70794` into the game's short-lived named-state table (the same mechanism the player update uses for `player-on-narrow-edge`). Nothing in PS3 code or DC data reads that name; PS4 neither writes nor reads it. The 30 is an argument of that publish call, not a default. No visible effect. | COVERED (nothing to do) | none |
| N5 (J7) mouth follows the microphone | The local player's caller is **`0x714db6`** (inside `0x714a70`, the PS4 counterpart of PS3 `0x51b3e8`). Sid `0x94bb3775` is the facial talking animation layer (the setter plays `facial-talking-calm` on it). PS4 replaced the microphone term by 0: one 8-byte patch makes it PS3 again. | COVERED (done 2026-11-25; loudness scale a proposal) | voice.cpp + one patch (done) |
| N6 (J1 question 7) talking indicators of other clients | PS3 had a full mesh: every machine had a connection record per other player, so every reader found every talker. Proposed PC rule: point the four PS4 talking-level readers at a separate level table that lists every remote member; SendNetEvent and every other record user keep the real records. | COVERED (done 2026-11-25) | voice.cpp + 4 displacement patches (done) |

## 2. N1 (M6). Film spectating HUD

### 2.1 What PS3 shows while a film plays

PS3 RePlaying update (`0x4366e4..`) and PS4 RePlaying::Update (`0x5e6d39..0x5e6ddd`) do the same with the HUD flag word
(PS4 `[[0x1130008 + 0x60]]`):

- every frame: flags |= `0xe0000`;
- the film task has two toggles, both 1 when the film starts (PS4 `0x5e8c6e`: task `+0x16c` = `+0x16d` = 1; PS3 task
  `+0x100` / `+0x101`): circle (PS4 pad bit `0x2000`) flips the HUD toggle `+0x16d`, square (`0x8000`) flips the theater
  toggle `+0x16c`;
- HUD toggle on: flags |= `0xe2400` (score panel `0x40000`, names and markers `0x20000`, message list `0x80000`, the
  multiplayer HUD element bit `0x2000`, element bit `0x400`); off: those bits cleared (the whole multiplayer HUD hides);
- theater toggle on: flags |= `0x100000`, off: cleared. (PS4 also clears `+0x16c` at `0x5e6a3e` when task `+0x162` is set.)

So with both toggles on (the default) PS3 HUD Draw `0x8aef68` draws, in its usual order and with its usual gates:
score panel (competitive, not game type 11), co-op panel (its own gates), the theater bar (flag `0x100000` and the
capture gate), the film boosters (below), the loadout boosters (flag `0x10`, competitive), the kill feed (competitive,
not type 11), names over heads and world markers (flag `0x20000`), the centre message. hud.cpp already follows this
order and these gates (`hud.md` 4.2); PS4 sets the same flags. In a film the "local" tracker (NGM `+0`) is the player
the film follows, so the score panel's "own team" and the boosters are the followed player's.

**No "watched player's name" element exists** in PS3 `0x8aef68`, in the theater draw `0x8ac3f0` (it prints only the
camera names, the m:ss position / length, a separator and the speed) or in the film task's camera handlers. The
followed player's name is visible as that player's overhead name tag (names list filled by the intact PS4 update
`0x55abe0`, drawn by hud.cpp 4.9). The third-party "name at the top" report is not backed by either binary: COVERED.

Cinema mode (`[0x113a990 + 0x678]` = PS4 `0x113b008`, PS3 `[cinema + 0x668]`) is set by the theater camera callbacks
in the film task's handler: `0xff502160` ("View Follow Camera", next followed player, `0x5e90fb`) sets **0**,
`0x806c9824` sets 1 (`0x5e8ed7`), `0x887c7d6e` sets 2 (`0x5e910f`). Mode 0 is therefore the follow camera.

### 2.2 The one difference: film boosters

PS3 (`0x8af18c..0x8af198`: `lwz r0,0x668(cinema)`, `cmpwi 0`, `beq` to the booster draw `0x8af5c4`) draws the film
booster pair (followed player's booster indices, PS4 tracker `+0x2a0` / `+0x2a4`) when a film plays, flag `0x40000` is
set, **cinema mode == 0** and the local tracker exists. hud.cpp `boosters()` (line ~813) and `hud.md` 4.7 step 1 test
`mode != 0`: ours shows them in the free / orbit cameras and hides them in the follow camera.

To do: in the film site condition use `[0x113b008] == 0`. Correct `hud.md` 4.7 step 1 ("s32 [0x113b008] == 0") and
`PORT-HUD.md` 5.4 ("cinema mode 0 = follow camera"). Nothing else in the film HUD changes.

Test: play a film; follow camera: the two booster icons of the followed player at the left (x = HUD x + 90, y 150 /
180.72); switch to the free or orbit camera: they disappear; circle: the whole HUD hides and returns; square: the
theater bar hides and returns.

Not a parity item (left as documented in `PORT-HUD.md` 7.3): PS3 prints the name of every camera icon that is near the
selection (weight `1 - |smooth - i|` above a threshold), ours prints only the selected camera's name.

## 3. N2 (M7 + J5). Mute Players page

### 3.1 Page facts (same on PS3 and PS4 menu.bin)

PAGE_PAUSE_MULTIPLAYER: Options, Mute Players (BUTTON_MP_MUTE_PLAYERS -> PAGE_MULTIPLAYER_MUTE_PLAYERS), spacer, Leave
Game. PAGE_MULTIPLAYER_MUTE_PLAYERS (title MENU_TITLE_MUTE_PLAYERS) holds two list controls:

| control | id | kind | action (PS3 / PS4) | condition | layout | column |
|---|---|---|---|---|---|---|
| LIST_MUTEPLAYERS0 | `0xb04c828b` | list (`0xa`) | 22 / 23 (team list) | 0 | `dcae45f4` | 0, width 350 |
| LIST_MUTEPLAYERS1 | `0xb48d9f3c` | list (`0xa`) | 22 / 23 | 1 | `dcae45f4` | 1, width 350 |

These are exactly the two team lists of PAGE_MULTIPLAYER_SCOREBOARD (same kind, action, condition, layout, columns);
the only difference is that the scoreboard's lists have no name / id, so they send no callback when a row is
selected. Rows, names, rank badges, the voice column (talking icon, J1 / N6) come from the same NetStatGame-based team
list (vtable `0x100a280`, row build `0x5b0f70`, cell `0x5b1e90`, voice cell `0x5b2ac0`) the scoreboard uses, which
is already populated on PC (`gameplay.md` 2, `rosternames`). Rows: COVERED (CHECK in a match).

### 3.2 What PS3 does when a row is selected (PS3 `0x443248`, ids `0xb04c828b` / `0xb48d9f3c`)

1. No local tracker: nothing.
2. Team: own team (local tracker team) for LIST_MUTEPLAYERS0, the other team (1 - own team) for LIST_MUTEPLAYERS1.
3. Table: the current-match NetStatGame table while the NGM match state is 1 or 2, else the previous-match copy.
4. Find the record whose team byte equals that team and whose list position equals the selected row + 1.
5. No record, no player behind it, or it is the local player: nothing.
6. The player's connection record; none: nothing. Otherwise flip that connection's voice mute (silently: no sound,
   no message box, no log). The list's voice column shows the new state on the next draw.

### 3.3 PS4 facts

- The menu callback registry (`0x1136640 + 0x18ec` count, `+0x18f0` entries of 0x18 bytes {u32 id, u64 fn, u64 arg};
  dispatch `0x5cb8d4..0x5cb926` calls every entry whose id is 0 or equals the selected control's id, with a 0x10-byte
  event {u32 control id, u32 value (= the selected row), u64 arg}).
- `0x5ecb80` is registered with id 0 (catch-all, registration at `0x5ed964`: `48 8d 0d 15 f2 ff ff`); prologue
  `55 48 89 e5 41 57 41 56 41 54 53 48 81 ec 90 00 00 00`. It reads the event id at `[rdi]`, row at `[rdi + 4]`.
- `0x5ed076` (`81 fe 3c 9f 8d b4`, id `0xb48d9f3c`) and `0x5ed0ca` (`81 fe 8b 82 4c b0`, id `0xb04c828b`) run steps 1-4
  of 3.2 exactly (team from local tracker `+0x134`, table `0x1147bd8` while `ngm + 0x1e80` is 1 or 2, else
  `0x114d2ec`; team byte at table `+0x38eb + 0x80 * i`, position u16 at table `+0x3e4c + 0x34 * i`), then jump to the
  common exit `0x5ed388` whether found or not (`0x5ed152` je on found). Step 5-6 are gone.
- Record i of a table: `table + 0x38e4 + 0x80 * i` (table = the address of its u32 count); its u32 at `+0` is the
  player's net id (PS4's own talk-time code `0x66c4f7` maps record -> tracker by comparing it with tracker `+0x120`),
  `+7` team, `+0xa` non-zero for the local player.

### 3.4 PC rule

Register a host callback for each of the two ids (remaining.cpp `register_callback`, ids `0xb04c828b`,
`0xb48d9f3c`; they have no entry yet, so it is appended). On the event:

1. No local tracker (`[0x113b760]` NULL): return.
2. team = local tracker `+0x134` for `0xb04c828b`, `1 - team` for `0xb48d9f3c`.
3. table = `0x1147bd8` when u32 `ngm + 0x1e80` is 1 or 2, else `0x114d2ec`; count = u32 at table, at most 10.
4. Find the first i < count with u8 `table + 0x38eb + 0x80 * i` == team and u16 `table + 0x3e4c + 0x34 * i` ==
   row + 1. None: return.
5. id = u32 `table + 0x38e4 + 0x80 * i`. If no tracker has `+0x120` == id (player gone), or id is the local net id
   (`session::local_id()`), return.
6. `voice::toggle_mute(id)` (it already refuses the local id). Log once per process
   `mute players page: net id %u muted|unmuted (PS3 0x443248)`; later toggles silent.

PS3 needs a connection record (step 6); on a PC client the other clients have no record (N6), but the voice layer
knows every member, so the PC rule toggles any remote member: the same visible result as the PS3 mesh. The PS4
catch-all keeps running (it finds the row and does nothing), so no patch of `0x5ecb80` is needed.

The muted icon in the voice column is J4 (`parity-j-voice-medals.md`), at the voice cell `0x5b2ac0`: the row's tracker
pointer is at `[rbp - 0x40]` at `0x5b2cb0` (`48 8b 45 c0`), its key (`+0x23c`, -1 for the local player, else the net id)
is loaded at `0x5b2cb4`; the PS4 talking branch builds a type-3 image cell whose index it finds by sid `0x63969b5b`
in the HUD image table `[0x1130008 + 0x70]` (entries of 0x28, sid at `+0`, list from `+8`, count u16 at `+2`),
`0x5b2e57..0x5b2e94`. J4 = before the talking test, when `voice::muted(key)`: the same kind of cell for the PS3 muted
icon (PS3 `0x7a5f5bf1`; find its PS4 counterpart in that table by name the way menuimages.cpp pairs PS3 sids, else
the net-u2mp copy). Without J4 the page toggles mute but shows no muted mark (PS3 shows it).

Test: match on two PCs, A pauses, Mute Players: both team lists with all players; select B: B silent, B's row shows
the muted icon; again: heard, icon gone (talking icon while B talks); select own row: nothing.

## 4. N3 (M9). Multiplayer Medals

### 4.1 PS3 menu facts (`dc1.psarc:menu.bin`, control records 0x48 bytes)

| page | controls in order |
|---|---|
| PAGE_NEWPROFILE (no save yet) | New Game; Load Game (go to page, condition 7); **Multiplayer**; Options; **Multiplayer Medals** (BUTTON_MULTIMEDALS `0x12b796c6`, label MENU_MEDALS, go to page (PS3 action 19) PAGE_MULTIMEDALS, visibility condition 8) |
| PAGE_NEWPROFILE_BETA | Multiplayer; Beta Trailer; Options; Multiplayer Medals (same record) |
| PAGE_PROFILE (with saves) | Continue; spacer; Multiplayer; Chapters; spacer; Options; Bonuses; New Game; Load Game (no medals) |
| PAGE_BONUSES | Gallery; Store; Treasures; **Medals** (single-player PAGE_MEDALS); Stats |
| PAGE_MULTIMEDALS | one list LIST_MEDALS (kind 7, PS3 action 11, condition **1**); PAGE_MEDALS has the same list with condition 0 |
| PAGE_MULTIPLAYER | no medals control |

So on PS3 the Multiplayer Medals page is reached only from the no-save main menu, when its visibility condition holds.

### 4.2 PS4 menu facts (control records 0x70 bytes)

PS4 PAGE_NEWPROFILE: New Game; Load Game (action 20, condition 8); Options; BUTTON_MULTIMEDALS (action 20 ->
PAGE_MULTIMEDALS, condition **9**) at `0x53a8`; PAGE_NEWPROFILE_BETA: Beta Trailer; Options; BUTTON_MULTIMEDALS at
`0x5658`. PAGE_MULTIMEDALS: LIST_MEDALS (action 12, condition 1). PS4 numbers these button conditions one higher
than PS3, as it does actions (checked on four buttons: Load Game 7 -> 8, Chapters 1 -> 2, Game Movies 6 -> 7, Multiplayer
Medals 8 -> 9), so PS4 condition 9 is PS3 condition 8.

Our import overlay (`u2mp/patcher/importer/overlay.cpp`, the first item of its header) rewrites the BUTTON_MULTIMEDALS
record of both pages into the Multiplayer button. Result on PC: Multiplayer sits where Medals sat (after Options), and
the medals page has no route at all.

### 4.3 PC rule (import overlay; no runtime change expected)

PAGE_NEWPROFILE and PAGE_NEWPROFILE_BETA get the PS3 control order:

- PAGE_NEWPROFILE: New Game, Load Game, **Multiplayer** (new control: a copy of an existing button record of this page
  with id / name BUTTON_MULTIPLAYER, label MENU_MULTIPLAYER, code button as the overlay makes it today), Options,
  **Multiplayer Medals** (the PS4 BUTTON_MULTIMEDALS record left exactly as the game ships it: action 20, condition 9,
  target PAGE_MULTIMEDALS);
- PAGE_NEWPROFILE_BETA: **Multiplayer** first (new control as above), Beta Trailer, Options, Multiplayer Medals (as
  shipped).

The overlay already appends a new control list for PAGE_PROFILE; the same technique (new list after the data, new
pointer slots in the bitmap) gives these two pages five / four controls. Bump the overlay VERSION. PAGE_PROFILE stays
as the overlay makes it (no medals there, as PS3).

What remains a CHECK (one look in the game, then decide):

1. Visibility: the PS4 menu evaluates condition 9 with its own intact code. If the button never shows on a fresh
   profile although PS3 would show it, PS4 stubbed that condition; then map PS3 condition 8 (the PS3 control-condition
   evaluator was not located in this pass) and add it as a menu query wrap.
2. Content: LIST_MEDALS with condition 1 must list the multiplayer medals (names, counts from NetPlayerData, loaded
   by progression.cpp). If the list is empty, the PS4 list handler for condition 1 is stubbed or reads an unloaded
   table: map it then (PS4 action 12 list handler).

Test: fresh profile (no save): main menu New Game, Load Game (if shown), Multiplayer, Options, Multiplayer Medals;
select it: "MULTIPLAYER MEDALS" page listing MP medals with the counts the career has; with a save: no medals entry in
the main menu (PS3 parity), single-player Medals under Bonuses.

## 5. N4 (M10 b). The loading-screen id `cdf70794`

PS3 loading draw `0x4ae638`, at `0x4ae6a0..0x4ae6ec`, calls `0x158ff8` with the name id `cdf70794`, a pointer to the
loading object's "shown" byte (object `+4`, the byte `0x4adaf8` sets on the first shown frame and the draw tests right
after the call), a float, and the values 150 and 30. `0x158ff8` / `0x1581b8` insert or refresh an entry keyed by the name
id in a table of short-lived named states (copy of the given bytes, a float, an expiry frame count). The player update
`0x51b3e8` uses the same call for `player-on-narrow-edge` (`0x3385fa01`, a hit in the PS3 EBOOT strings); many static
initialisers register names through it too. Findings:

- PS3 code builds `cdf70794` only at `0x4ae6a0` (no other `oris/ori` pair of that value in the whole text);
- no PS3 or PS4 DC file (`dc1`), no PS4 level pak and no actor pak contains the id;
- PS4 code never uses `cdf70794` (PS4 still publishes `player-on-narrow-edge` at `0x714c3e` via `0xa1cd50`).

So `cdf70794` is a "loading screen is up" state the PS3 published for scripts or debug views that do not exist in
either shipped build; it draws nothing and changes nothing a player can see. COVERED; remove "(b)" from
`loading.md`'s not-in-scope list or mark it "identified, no visible effect". (The name string itself was not
recovered; common guesses do not hash to it.)

## 6. N5 (J7). Own character's mouth follows the microphone

### 6.1 PS3 behaviour (`0x51b3e8`, setter `0x437c0`)

Inside the character update `0x51b3e8` (the base update every player state calls), after the animation-state checks
`e1939f43` / `12d842c0` / `7861af68` and after publishing `player-on-narrow-edge`:

1. mic = microphone loudness (`0x4e6f78`): 0 when the voice encoder port does not exist; else it turns the port's
   energy metering on, asks the voice-activity flag, and answers the port's energy level while voice activity is
   detected, 0 otherwise.
2. line = the character's own voice-line sound amplitude x **100.0** (PS3 constant `0x42c80000`).
3. v = the larger of mic and line.
4. Setter (`0x437c0`, channel list of the character): on the animation layer **`0x94bb3775`** (found, or created, in
   the character's animation control) it plays **`facial-talking-calm`** (`0x55c118a9`, found in every PS4 character
   animation pak) and fades the layer toward weight = min(v / **5.0**, **1.0**); the anim is started whenever that
   weight is above 0 and the layer has no animation running; the fade speed depends on the change of v since the
   last frame, with one rate when v is at least **2.0** and another below it (constants 0.5 and 5.0). So sid
   `0x94bb3775` is the facial talking layer: talking into the headset makes the own character talk.

PS3 setter callers: `0x1a20a8`, `0x45f328`, `0x51b3e8`; only `0x51b3e8` reads the microphone.

### 6.2 PS4 facts

- `0x473010` is the same setter (same thresholds: x `0.2` at `0xe6087c`, cap `1.0` at `0xe60880`, `2.0` at `0xe60884`,
  rates `0.5` / `5.0` at `0xe60888` / `0xe6088c`, plays `0x55c118a9` at `0x473226`). Its three callers pair with the
  PS3 three: `0x714db6` (in `0x714a70`) = PS3 `0x51b3e8`; `0x21d3a6` and `0x607763` are the other two (other actor
  kinds, no microphone on PS3 either).
- `0x714a70` is the counterpart of PS3 `0x51b3e8`: same order of work (publish `0x3385fa01` at `0x714c3e`, the three
  animation-state tests at `0x714cca` / `0x714cf0` / `0x714d0d`, then the mouth).
- The mouth at `0x714d7a..0x714dbb`:

```
714d96: c5 fa 59 05 f2 ec 74 00   vmulss xmm0,xmm0,[0xe63a90]   line amplitude x 100.0
714d9e: c5 f0 57 c9               vxorps xmm1,xmm1,xmm1          the microphone term, now 0
714da2: c5 f2 5f c0               vmaxss xmm0,xmm1,xmm0          v = max(0, line)
714da6: 49 8d bc 24 c0 2e 00 00   lea rdi,[r12+0x2ec0]           setter state
714dae: 49 8b b4 24 90 01 00 00   mov rsi,[r12+0x190]            channel list
714db6: e8 55 e2 d5 ff            call 0x473010
```

PS4 removed `0x4e6f78` (no voice library) and put 0 in its place; everything else is PS3.

### 6.3 PC rule

1. A 4-byte float M, process-wide, starts 0, in memory reachable rip-relative from `0x714da6` (a `near_stub`
   allocation).
2. Patch the 8 bytes at `0x714d9e` (`c5 f0 57 c9 c5 f2 5f c0`) to `c5 fa 5f 05 <rel32>` = `vmaxss xmm0,xmm0,[rip+M]`
   (rel32 = M - `0x714da6`). v = max(line, M); M is never negative, so the PS4 "not below 0" rule still holds. Verify
   before patching: the 8 bytes above, `0x714d96` `c5 fa 59 05 f2 ec 74 00`, `0x714db6` `e8 55 e2 d5 ff`, and the setter
   prologue `0x473010` (voice.cpp `log_mouth_state` already checks it). Any mismatch: no patch, log
   `voice: mouth patch skipped (bytes at 0x%llx differ)`.
3. M is written by the game thread once per frame (voice::frame): M = L when voice is on, the capture stream is open
   and we are transmitting this frame (our gate open; in push-to-talk mode also the key held), else 0. PS3 had no
   push to talk; "transmitting" is the PC form of "voice activity detected".
4. L, the loudness: the PS3 unit is the voice library's port energy level, which neither binary defines. Proposed
   PC scale, chosen to land on the setter's PS3 range (weight 0 below the gate, full at normal speech):
   L = 100 x rms / 32768, with rms the RMS of the last 20 ms capture frame in 16-bit sample units (the value
   `Gate::update` already computes; hand it from the capture callback to the game thread through an atomic). Gate
   threshold rms 300 gives L about 0.9 (weight 0.18), rms 3000 (ordinary speech) gives L about 9 (full weight).
   Mark L's factor as a named constant; adjust only after an in-game look (owner's call, open question N-1).
5. Every character that runs `0x714a70` gets the same floor, exactly as PS3 `0x51b3e8` did (no local-player test in
   either binary). Log once per process `voice: mouth follows the microphone (PS3 0x51b3e8; patch at 0x714d9e)` and
   once the first time M > 0 with the character object (`r12` is not reachable from the patch; log from voice::frame
   when M first becomes non-zero).
6. Replace the current "J7 off" log line by the line of point 5 when the patch is in.

Test: third person, lobby or match, speak: the own character's face plays the talking animation while speaking and
stops within a fraction of a second; silence: only voice lines move it; voice off (`UNCHARTED_U2MP_VOICE=off`):
never (PLATFORM: PS3's chat restriction kept the microphone metered; ours opens no capture device).

## 7. N6 (J1 question 7). Talking indicators of other clients

### 7.1 PS3

PS3 ran a full mesh: every machine had one connection record per other player (32 records), and every player's
voice arrived directly at every machine that hears them (`parity-j-voice-medals.md` 1 items 2, 3, 7). The talking
level lived in the record of the speaker's connection, so every reader (overhead name tags, scoreboard voice column,
the Mute Players page column, the talk-time counter) found every remote talker on every machine.

### 7.2 PS4 / PC facts

- Our transport is a star: a client attaches only the host's record (`session.cpp on_peer`, gamenet.cpp `attach`), the
  host attaches every client. So on a client the readers find only the host.
- SendNetEvent `0x644ab0` finds a player's record by key (record `+0` == tracker `+0x23c`, `0x644b80..0x644ba5`) and
  queues to it directly only when the record's relayed byte `+6` is 0; otherwise, and when no record matches, the
  player goes on the relay list. Other PS4 code also acts on a record found by key without testing "in use"
  (`0x60bdeb`, `0x66a51b`, `0x66a67b`); others look up by the `+0x34` copy (`0x6149ab`, the `0x609xxx` connection
  manager). So any extra record with a matching key would change event handling: not done.
- The four talking-level readers search the records by `+0` == tracker `+0x23c` and test `+0x1c8` >= 1.0. Each takes
  the record base with one `add reg, [rip+d32]` of the pointer slot `0x104ae30` (which holds the address of the
  connection manager `0x1152a10`; records from `+8`, stride 0x220, 32 of them) and uses that base for nothing else:

| reader | instruction | bytes | base used afterwards |
|---|---|---|---|
| overhead name tags (writes tag `+0x98`) | `0x55b4d8` | `48 03 05 51 f9 ae 00` | stored at `[rbp-0x178]`, read only at `0x55c0a6` for this search |
| scoreboard / Mute Players voice cell | `0x5b2cbf` | `48 03 05 6a 81 a9 00` | the search at `0x5b2cc8` and the level test at `0x5b2df8` |
| "is talking" helper | `0x66644b` | `48 03 05 de 49 9e 00` | the search at `0x666460` and the test at `0x666480` |
| talk-time counter | `0x66c4d9` | `4c 03 05 50 e9 9d 00` | copied to rbx at `0x66c53c` for the search |

### 7.3 PC rule

1. A level table owned by the runtime: 8 bytes header, then 32 slots of 0x220 bytes (only slot `+0` key and
   `+0x1c8` f32 level are used; zero everything else), plus an 8-byte pointer slot P (in `near_stub` memory, rip
   reachable from the four instructions) holding the table's absolute address.
2. Patch each reader's disp32 so it reads P instead of `0x104ae30` (new disp = P - address of the next instruction:
   `0x55b4df`, `0x5b2cc6`, `0x666452`, `0x66c4e0`). Verify the 7 bytes of each first; patch all four or none; log
   `voice: talking indicators read the PC level table (4 readers)` or the mismatch.
3. Every frame (game thread, where `gamenet::set_talk_levels` runs today): for each present remote member with net
   id k (k >= 1), one slot with key k and level = voice.cpp's J1 level of k; every other slot key 0, level 0. Never a
   slot for the local player (its tracker key is -1). On the host this lists the same players its real records hold,
   on a client it adds the other clients, both with the J1 / J2 rules already in voice.cpp.
4. The real records' `+0x1c8` are no longer read by anyone; `set_talk_levels` may keep writing them or stop (no
   visible effect either way). SendNetEvent and every other record user are untouched.
5. If the patch is not applied (bytes differ), keep today's behaviour (host-only on clients).

Visible result: on every machine the talking icon appears over every remote talker's head and in their scoreboard
and Mute Players rows, and their talk time grows, exactly as on the PS3 mesh. Muted players still light (J2: mute only
silences; the rows show the muted icon first, J4).

Test: three PCs (host A, clients B, C), match: C talks; on B the icon over C's head and in C's scoreboard row for
about a third of a second after C stops; C's talk time grows on B; nothing changes for events (kills, pickups, chat
of the match) compared with before (log the relay counters before / after).

## 8. Ready-to-implement list

| # | Item | Files | Size |
|---|---|---|---|
| 1 | N1: film booster site condition `== 0` | `gow3_pc/src/u2mp/hud.cpp`; `hud.md` 4.7, `PORT-HUD.md` 5.4 | 1 line |
| 2 | N2: register callbacks `0xb04c828b` / `0xb48d9f3c`, rule 3.4 | `remaining.cpp` (callback registry), `voice.cpp` (`toggle_mute`) | small |
| 3 | N2 / J4: muted icon in the voice cell `0x5b2ac0` (also serves the scoreboard) | `scoreboard.cpp` or `menus.cpp`, `menuimages.cpp` (icon pairing) | medium |
| 4 | N6: level table + 4 reader displacements, rule 7.3 | `voice.cpp`, `gamenet.cpp` (or a new small file) | small / medium |
| 5 | N5: mouth patch at `0x714d9e` + loudness M, rule 6.3 | `voice.cpp` | small |
| 6 | N3: PS3 main-menu order with Multiplayer and Multiplayer Medals | `u2mp/patcher/importer/overlay.cpp` (VERSION bump), `MENUS.md` 5 | medium |
| 7 | N3 checks: condition 9 visible on a fresh profile, MP medals list filled | game look, then a follow-up spec if not | check |
| 8 | Docs: `parity-m-features.md` M6/M7/M9/M10 and `parity-j-voice-medals.md` J5/J7/Q7 point here | specs | done with this file |

Suggested order: 1, 2, 4, 5 (independent, small), then 3 (needs the icon pairing), then 6 and 7 (import change and a
fresh-profile run).

## 9. Open questions

1. N5: the factor of L (100 x rms / 32768) is a proposal; PS3's unit (the voice library's energy level) is not defined
   by either binary. Confirm by eye after the patch.
2. N3: what PS3 condition 8 tests (the visibility of Multiplayer Medals on the no-save menu). Only needed if the PS4
   condition 9 turns out to be stubbed.
3. N2 / J4: the PS4 name of the muted icon in the HUD image table `[0x1130008 + 0x70]` (PS3 `0x7a5f5bf1`).

## 10. Implementation state (2026-11-25, branch `u2mp-p4-mspec`)

Nothing below was run in the game yet; the unit tests and the static byte checks pass.

- **N1 done.** hud.cpp `boosters()`: film site when `[0x113b008] == 0`; `hud.md` 4.7 and `PORT-HUD.md` 5.4 corrected.
- **N2 done, one mechanism.** The earlier J5 code (a93b2a8: the `lea` at `0x5ed964` pointed at a wrapper of
  `0x5ecb80`) is removed; `0x5ecb80` runs unpatched. remaining.cpp `mute_players_row` is registered for
  `0xb04c828b` and `0xb48d9f3c` (F-register, appended) by `mute_players_frame`, which session.cpp `frame_work` calls
  every session frame (the game never registers the two ids and fills its table at run time; the call finds the
  entries after the first time). Rule 3.4 exactly: no local tracker / unknown list / no entry / no tracker with that
  net id (`+0x120`) / the local net id: nothing; else `voice::toggle_mute`, log once per process. Install checks the
  PS4 row search bytes (`0x5ed076`, `0x5ed0ca`, own team, NGM state, the two copies, positions, team bytes, the
  NetStatGame `lea` target); any difference: no callbacks, logged. Rules `overlay::mute_action_team`,
  `mute_action_previous_game`, `team_row_entry`, `mute_list_target` (u2mp-overlay-test).
- **N2 / J4 muted icon** (a93b2a8, checked): the vtable slot 15 of `0x100a280` (the cell query `0x5b2ac0`) is taken
  whole, so the muted icon is answered before the talking branch (`0x5b2cb0`); the install check includes the key load
  at `0x5b2cb4` (`8b 88 3c 02 00 00`). Every list of that vtable asks it: the scoreboard's team lists and the Mute
  Players lists. The icon record carries id `0x7a5f5bf1` with its index in the net image table, or -1, which makes
  the image lookup (menuimages.cpp) pair it by PS3 name or take the net-u2mp copy (question 3 still open: the PS4
  name is not known).
- **N6 done.** voice.cpp `install_talk_table`: the four 7-byte readers checked (bytes and that each d32 reaches
  `0x104ae30`), then all four d32 point at an 8-byte slot from `near_stub` holding the address of a static table (8 +
  32 x 0x220 bytes). `fill_talk_table` (game thread, every `voice::frame`, and zeroed in `voice::stop`) writes one
  slot per present remote member (rule `voice::talk_table_slots`, u2mp-voice-test). `gamenet::set_talk_levels` keeps
  writing the real records (rule 4; no reader left). Any difference: no patch, today's behaviour, logged.
- **N5 done.** voice.cpp `install_mouth`: the four sites checked, M from `near_stub`, the 8 bytes at `0x714d9e`
  become `c5 fa 5f 05 <rel32>`. The capture callback stores `voice::mouth_loudness(Gate::rms(), transmitting)` (L =
  `kMouthLoudnessScale` 100 x rms / 32768, a named constant marked as the proposal of question 1) in an atomic;
  `voice::frame` copies it to M while the capture stream is open, else M stays 0 (also set to 0 in `stop`). The old
  "J7 off" log line is gone.
- **N3 done in the import.** overlay.cpp (VERSION 6): PAGE_NEWPROFILE and PAGE_NEWPROFILE_BETA get new control lists
  in the PS3 order with a new Multiplayer control (a copy of the page's BUTTON_MULTIMEDALS record made the code button
  the overlay made before, with appended name strings `BUTTON_MULTIPLAYER` / `MENU_MULTIPLAYER`); BUTTON_MULTIMEDALS
  is left as shipped. PAGE_PROFILE's list is unchanged apart from the Multiplayer control's two name pointers.
  Checked on a copy of the PS4 game's menu.bin (`u2import --overlay` into a scratch folder): the three pages read back as specified, the pointer bitmap covers the new
  data, and against overlay version 5 the Multiplayer record differs only in its two name pointers. The CHECKs of
  4.3 (condition 9 visibility, LIST_MEDALS content) stay open until a game look.
