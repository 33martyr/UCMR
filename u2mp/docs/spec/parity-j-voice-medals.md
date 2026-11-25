# Spec: PS3 parity, group J (net voice, bandwidth stats, EarnMedal events)

Clean-room parity check of the 21 PS3 1.10 functions of `maintainer-tools/research/u2-parity/groupJ.tsv` (14
net-voice rows `0x4e6cd0..0x4e7988`, 1 net-stats row `0x4dbe70`) and `groupJ-gameplay.tsv` (NetEventEarnMedalClient
type 60 and NetEventEarnMedal type 61, Read / Write / Apply) against the UCMR U2 runtime (`gow3_pc/src/u2mp`, branch
`u2mp-ps3-parity-3`). It says what a PS3 player saw and heard and what our port must do; it does not prescribe code,
structure or names. Addresses are PS3 1.10 (`NPUA80698` EBOOT) unless marked PS4 (PS4 1.02 image offset).
Background: `parity-e-flow2.md` 2.1 (in-match voice gating, done; not repeated here), `parity-f-party-mm.md` F8
(stranger mute in matchmaking), `parity-d-flow1.md` 2.4 and `parity-e-flow2.md` 2.7 / `parity-h-session.md` H7
(Mute / Mute All popups), `parity-b-menus.md` 1 / 2 (roster voice icons, volume slider), `hud.md` (overhead names),
`events.md` (type 61 layout), `TRANSPORT-INTERFACE.md` 4 (bit stream).

Status words (as in `parity-c-match.md`):

- **COVERED**: our code, or the intact PS4 code it relies on, gives the same visible / audible result.
- **PLATFORM**: PS3 plumbing (cellVoice ports, NP, file system, developer overlays) that the PC design replaces. Only
  the visible / audible result has to match; the replacement is named.
- **MISSING**: absent, or present with a different visible / audible result in the part named.

## 0. Facts every item uses

**Module data.** The voice functions address their data through the word at `0xe10b98` (= `0xdbb9ec`), not through
the Ghidra default TOC `0xe183b8`; the bandwidth overlay uses `0xe10b88` (= `0xdbb704`), the talking/mute queries of
the player tracker file `0xe10b64` (= `0xdbad18`), the volume slider `0xe10b54` (= `0xdba8bc`), flow.cpp `0xe10adc`
(= `0xdb82b8`). Every value below was read through the right base.

| PS3 | meaning | PS4 / ours |
|---|---|---|
| `0xf3a034` / `0xf3a03c` / `0xf3a048` | the three global voice ports: microphone in, speaker (headset) out, encoded voice out | ours: SDL capture / playback streams + Opus encoder (voice.cpp) |
| `0xf3a058` u8 | "voice initialised" | voice.cpp `audio_open` |
| `0xf3a030` u8 | "this machine's microphone is producing voice" (encoded port running); only reported in the SQS telemetry | none needed |
| `0xf3a060` | connection manager: 32 records of 460 bytes from `+8`; record `+4` "in use" | PS4 records `0x1152a18`, 0x220 bytes (gamenet.cpp `kRecords`); PS4 code reaches them through the pointer at `0x104ae30` (+8) |
| record `+0x17c` | voice record of that connection (below) | PS4 record: talking level at `+0x1c8` (the only voice field PS4 code still reads) |
| tracker `+0x204` | connection id of the player (selects the record) | PS4 tracker `+0x23c` |
| netInfo `+0x2b8` u8 | "voice off": the PSN account's chat restriction (ticket subject-status bit 8, read once in NetInit) | PS4 netInfo `+0x2c8`; no PS4 code reads or writes it |

**Voice record** (one per connection record, PS3 record `+0x17c`):

- playback volume of that player, 1.0 at creation (in-match gating writes 0.0 / 1.0, `parity-e` 2.1);
- a 2000-byte outgoing queue of this machine's encoded voice for that player;
- its own cellVoice input port, connected to the speaker port; mute flag of that port = "this player is muted";
- talking level (record `+0x18c`): set to **10.0** whenever voice data from that player arrives, lowered by **1.0**
  once per game frame while above 0; "talking" means **level >= 1.0** (constant 1.0 at `0xdb2d1c`). At 30 frames per
  second the icon stays on for about a third of a second after the last voice packet;
- the time (net clock seconds) the last voice data arrived.

**Codec facts (interop only, not used by our design):** cellVoice at 3850 bit/s, 20-byte frames. Voice rides at the
end of the ordinary game-data packet of a connection (packet kind 0, after the events): one presence bit; when set,
3 bits = frames - 1 and then that many 20-byte frames. PS3 always sends 6 frames (120 bytes, about 0.25 s of speech)
and only when at least 120 bytes are queued for that connection.

## 1. What a PS3 player heard and saw (the reference behaviour)

1. **Open microphone, no button.** Voice starts at NetInit (entering online multiplayer) and ends at the net
   teardown. The microphone feeds the encoder all the time; the system's voice activity detection decides when data
   flows. There is no push-to-talk on PS3.
2. **Full mesh.** Every connection carries the voice of both ends. Each frame the newest encoded voice (at most 200
   bytes) is appended to the queue of every connection in use; a queue that is full keeps its old data and drops the
   new. Every game packet to a connection takes one 120-byte chunk from its queue; the chunk is sent only when this
   machine's playback volume for that player is not 0, otherwise it is thrown away. So a player I do not hear (gated
   to 0 in a match, or "voice off") does not get my voice either.
3. **Receiving.** With "voice off" set the voice part of incoming packets is ignored entirely (no sound, no talking
   level). Otherwise the frames go to that player's port and the talking level is set to 10, **whether or not the
   player is muted** (mute only silences the port).
4. **Who hears whom.** Lobby, party, matchmaking room and results: everyone connected hears everyone at full volume,
   except muted players (item 5). In a match: `parity-e` 2.1 (team / 12 m / Elimination rules; results reset to 1.0).
5. **Mute.** One flag per connection, the port mute flag:
   - a connection record taken for a new peer is reset, and the reset **mutes** it (and empties its queue);
   - a party member is unmuted when they join the party (`0x4c40f0`);
   - every player of the matchmaking slot list who is not in our party is unmuted when the vote opens (`0x4b0700`
     client, `0x4b0868` host) - also one the player had muted by hand while gathering;
   - the player's own Mute / Unmute / Mute All / Unmute All (party rows, match player list, in-match scoreboard)
     toggle the same flag, silently;
   - the flag lives as long as the connection record; nothing else changes it (results, new rounds and leaving the
     match do not).
   So a stranger is muted from the moment the connection exists until the next vote opens; one who arrives after the
   vote (join in progress) stays muted until the next vote, unless unmuted by hand. Every mute-state reader (roster
   and matchmaking rows, popups, scoreboard) shows this one flag.
6. **Volume.** The Audio Volumes page slider (cond 104, only while online) sets the speaker port volume: 21 steps,
   0..20, volume = step x 0.05; reading it back gives volume x 20 truncated. Voice init creates the speaker port at
   1.0, so **the voice volume is back at full every time online multiplayer is entered**.
7. **Talking indicators** (all read the talking level of the player's connection record, never of the local player:
   the local player has no connection record):
   - party roster rows and matchmaking rows: muted icon `0x7a5f5bf1` if muted, else talking icon `0xffa98ec5` while
     talking (covered, `parity-b`);
   - in-match scoreboard, voice column (column id `0x4c217b`, PS3 `0x412d14`): muted icon if that player is muted,
     else talking icon while talking; nothing for the local player;
   - overhead name tags (PS3 HUD `0x8aa448`): talking icon while that player is talking (mute is not consulted);
   - the scoreboard's per-player "time spent talking" counter (PS3 `0x4df460`, entry `+0x14` of the 0x80-byte stats
     entries): grows by the frame time while that player is talking.
8. **Own character's mouth** (PS3 `0x51b3ec`, the local player's character update): the microphone loudness while
   voice activity is detected (0 otherwise) is fed, as a floor under the value the character's own voice-line
   playback gives, into a facial animation channel (sid `0x94bb3775`, most likely the jaw / mouth). Talking into the
   headset moves your own character's mouth.

## 2. Rows with nothing to do (summary)

- **Bandwidth stats `0x4dbe70`** (caller `0x43b698` from the per-frame update): a developer overlay that prints the
  send / receive rates and draws a 126-sample history graph. It runs only when the devkit DIP switch 0 is set (a
  `sys_gpio_get` of the DIP device); a retail PS3 never shows it. PLATFORM, nothing to port.
- **Voice init `0x4e71d8` / voice end `0x4e7038`** (NetInit `0x447a5c`, teardown `0x4386d0`): cellVoice start/stop,
  the three global ports, the mic -> encoder link, error logs. PLATFORM: voice.cpp opens its SDL streams when the
  first other member is present and `voice::stop` closes them from the session teardown. The audible results (voice
  on while online, off after leaving) match. The volume reset at init is item J6.
- **Per-peer port `0x4e77e0`** (connection manager init `0x4e9d28`, 32 records) and **record reset `0x4e6f04`**
  (from `0x4e8a50`: unmute, empty the queue, then mute): PLATFORM; voice.cpp creates a jitter buffer and decoder per
  speaker on the first packet and `forget()` drops them when the member leaves. The mute-on-new-connection rule is J3.
- **Send `0x4e7464`** (NetConnectionUpdate `0x43cdb8`): PLATFORM; voice.cpp `frame` sends Opus frames to the host,
  the host relays them (star). Audible result matches: mesh voice and star voice reach the same people. The "do not
  send to a player I do not hear" part only changes who sees a talking icon: J2.
- **Read `0x4e7560`** (per-frame update `0x449e88`): mic data into the queues (PLATFORM: SDL capture callback +
  `frame`), the telemetry-only "mic running" byte (PLATFORM), the talking-level decay (J1).
- **Master volume get / set `0x4e6e24` / `0x4e6e94`** (slider `0x4b77ac`): COVERED by menus.cpp `query_volume` +
  `voice::volume` / `voice::set_volume` (steps 0..20, x 0.05). Ours rounds the read-back (+0.5) where PS3 truncates;
  with values the slider itself wrote both give the same step.
- **Per-player volume `0x4e6e64`** (gating `0x43bdd0`, ShowResults `0x43ec08`, voice-off send path `0x43cdb8`):
  COVERED by voice.cpp `update_gating` / `all_audible` (`parity-e` 2.1).
- **Voice off.** PS3's flag is the PSN account chat restriction. Ours: `UNCHARTED_U2MP_VOICE=off` gives the same
  result (nothing sent, nothing played, no talking icons, the host still relays the others' voice). Nothing reads
  PS4 `+0x2c8`; leaving it 0 is fine. Open question 4 (an RPCN chat-restriction source): skipped, the online
  program's interface (`u2_online_abi.h`) gives the runtime no ticket subject status to read.
- **Push to talk.** Not on PS3. Our default (`open`, voice-activity gate) is the PS3 behaviour; `ptt` is a PC option
  and stays.
- **File read helper `0x4e7988`** (filed under voice by the matcher, but it is not voice): read a named file into a
  buffer, returning the byte count or -1 when it does not fit. Callers: player data load `0x4c699c` (0x228 bytes,
  version 10) and the net script patch load `0x4e83c8` (256 KB, netInfo `+0x2b0` load state 1/2/3). PLATFORM:
  progression_store.cpp / netpatch.cpp read their own files.
- **NetEventEarnMedalClient (type 60) Read `0x468dc4`, Write `0x468dc0`, Apply `0x468dc8`**: empty on PS3 (4-byte
  bodies) and on PS4 (`0x61d260`, `0x61d270`, `0x61d250`). COVERED (also `parity-i`).
- **NetEventEarnMedal (type 61)**, checked field by field:
  - Wire (PS3 Read slot 2 `0x469ef8`, Write slot 3 `0x469bd4`; the TSV's Read/Write labels are swapped): three full
    32-bit words in this order: the earner's net id, the medal trigger id, the amount. 96 bits, nothing else. PS3
    event fields `+8 / +0xc / +0x10`, PS4 `+0xc / +0x10 / +0x14`. events.cpp `read_earn_medal` / `write_earn_medal`
    read and write exactly these three words in this order. COVERED. (A short packet: PS3 asserts; ours resets
    the payload. Deliberate, safer.)
  - Apply (PS3 `0x46b760`, PS4 `0x61d1e0` intact): find the player tracker with that net id; if there is one, award
    the medal trigger with the amount to that player (PS3 `0xc2030`, PS4 `0x5779a0`: medal table match, pop-up,
    money with booster, stats); none: nothing. PS4 searches the ten trackers by net id, as PS3's lookup does.
    COVERED (events.cpp leaves the PS4 Apply in place).

## 3. MISSING items

Implementation state (2026-11-25, branch `u2mp-ps3-parity-3`): J1, J2, J3, J6 and J8 done; J7 off (hook point not
confirmed, see J7); J4 and J5 pending (they need scoreboard.cpp, which another branch is editing). Each item ends
with a **Done** / **Pending** paragraph. Rules with no game in them (level decay, which packets count, mute
transitions) are `voice::decay_talk_level` / `is_talking` / `use_voice` / `next_mute`, tested by `u2mp-voice-test`.

### J1. Talking level in the game's connection records (PS3 `0x4e6cd0`, `0x4e7560`; readers `0x4c7c70`, `0x412d14`, `0x8aa448`, `0x4df460`)

PS3 keeps a talking level per connection (section 0). PS4 kept every reader of it but nothing writes it, so on PC:
the scoreboard voice column never shows the talking icon (PS4 `0x5b2ac0`, branch at `0x5b2cb0`), the overhead name
tags never show it (PS4 `0x55c0e1`, byte at name entry `+0x98` that hud.cpp draws), the scoreboard talk-time counter
never grows (PS4 `0x66c4a0`), and one more PS4 reader (`0x66644b`, not traced) stays dark. Our own rows use
`voice::talking` (last 300 ms) and are fine.

To do (voice.cpp, with gamenet.cpp's record mapping): for every remote player, keep the PS3 level in that player's
PS4 connection record (`+0x1c8`, float; the record is the one whose id equals the player's tracker `+0x23c`):

- set it to 10.0 whenever a voice packet from that player is accepted for "talking" (J2 says which);
- lower it by 1.0 once per game frame while it is above 0 (game thread, from `voice::frame`);
- set it to 0 when the player leaves, when voice stops, and never write it for the local player.

PS4 readers compare against a constant expected to be 1.0 (`0xe61210`, `0xe61ca0`, `0xe62708`, `0xe626b0`: check at
install; if one is not 1.0 the level scale above still holds because the readers only test "at least the threshold").
`voice::talking` should then be "level >= 1.0" too, so every indicator agrees (same third of a second at 30 fps).

**Done.** voice.cpp keeps one level per net id (`receive` sets 10, `frame` lowers it by 1.0 per call, not below 0;
`forget` and `stop` zero it) and `gamenet::set_talk_levels` writes, every frame, `level(key)` to `+0x1c8` of each
record in use (0 to every other record, so a reused record starts dark; the local id always gets 0).
`voice::talking` = level >= 1.0. Checked in the PS4 1.02 image: all four threshold constants are 1.0; the readers
(scoreboard `0x5b2cb0`, name tags `0x55c0ad..0x55c0e1`, talk time `0x66c4a0`) find the
record by comparing record `+0` with tracker `+0x23c` over the 32 records reached through `0x104ae30` (+8), which is
the key `gamenet::attach` writes (= the net id, `session.cpp` tracker `+0x23c`).
Open part (topology): a client attaches only the host's record (star, `session.cpp on_peer`); the other clients have
no record on it, so on a client the game's own readers (name tags, scoreboard column, talk time) light only for the
host; our roster / matchmaking rows (`voice::talking`) light for everyone. Records for relayed players would change
how PS4 `SendNetEvent 0x644ab0` routes events (relay list), so they are not added here.

### J2. Which voice lights a talking indicator (PS3 `0x4e6cd0`, `0x4e7464`, flow `0x43cfb0` / `0x43d0e0`)

PS3: a player's level rises whenever their voice data arrives - also when I muted them (mute only silences), also for
a stranger while the matchmaking room gathers. Their voice does not arrive when they gate me out in a match (they do
not send to players they do not hear) or when I have voice off.

Ours: `receive` returns before marking "talking" for muted players and strangers, and marks it for players silenced
by in-match gating. Parity:

- mark talking (J1 level) for muted players and for strangers too; the rows keep showing the muted icon
  for them (mute wins there), the overhead name tag shows the talking icon (PS3 does not look at mute there);
- do not mark it for a speaker the in-match gating currently silences (the PS3 sender would not have sent; the rules
  are symmetric, so the receiver's own gating result is the same answer);
- with voice off, mark nothing (already so: `enabled()` false).

**Done.** `receive` asks `use_voice(voice on, gated silent, muted, playback stream)`: the level rises when voice is on
and the in-match gating does not silence the speaker (mute and stranger status do not matter); the frames are played
only when it rose, the speaker is not muted and there is a playback stream.

### J3. One mute flag, muted on arrival (PS3 `0x4e8a50` reset, `0x4e9e98`, `0x4c40f0`, `0x4b0700`, `0x4b0868`; resolves `parity-f` open question 2)

PS3 behaviour: section 1 item 5. Ours: a separate, hidden "strangers muted" state that is phase-based (on from the
start of the gathering phase of each search until the vote opens, `frontend3.cpp strangers_muted`), plus the
player's `mutes[]`. Differences:

- PS3 shows a muted stranger as muted (muted icon in the matchmaking rows, "Unmute" in the popup) and lets the player
  unmute one by hand before the vote; ours shows nothing and the hand toggle does not make them audible;
- PS3 keeps a stranger unmuted once the vote unmuted them, for as long as the connection lives (postgame, next
  gathering with the same players); ours re-silences every stranger whenever a new gathering phase begins;
- PS3 mutes a stranger who arrives after the vote until the next vote; ours plays them at once.

Parity (voice.cpp + frontend3.cpp + session.cpp): drop the separate state and use the one per-player mute:

- when a new remote member appears on the transport (voice.cpp `present` false -> true) and is not a member of our
  party (`session::party_stranger`), set them muted; party members appear unmuted;
- when a member joins our party, unmute them;
- when the vote opens (host and client paths), unmute every present stranger, including ones the player muted by hand;
- forget the flag when the member leaves (already so: `forget`);
- `voice::muted` reports this flag, so rows, popups (Mute / Unmute label, Mute All / Unmute All visibility) and the
  scoreboard (J4) all follow it.

**Done** (owner: PS3 exactly, late joiners stay muted until the next vote). frontend3.cpp's `strangers_muted` state
is gone. voice.cpp `frame` tracks every remote member: on arrival it is muted unless `session::party_member` (new:
off the online link everyone is; online only once its NpId is known and in our party room, so a member whose NpId
is not known yet starts muted, as a PS3 connection does, and is unmuted when it shows up in our party); the first
frame it is a party member it is unmuted ("joined our party", once per connection, so a later NpId flicker cannot
undo a hand mute); `open_vote` (host and client) calls `voice::unmute_strangers`, which unmutes every present player
that is not a party member, also hand mutes; leaving forgets the flag. `muted()` / `toggle_mute()` never apply to
the local player. Logs: `voice: net id N connected outside our party: muted until the vote opens`, `voice: net id N
joined our party: unmuted`, `voice: the vote opens: players outside our party are heard now (K unmuted)`.

### J4. Scoreboard voice column: muted icon (PS3 `0x412d14` via `0x4c9de8`; PS4 `0x5b2ac0`)

PS3, voice column `0x4c217b` of the in-match scoreboard, for a row that holds a player other than the local one: the
muted icon `0x7a5f5bf1` when that player is muted; otherwise the talking icon `0xffa98ec5` while talking; otherwise
nothing. The local player's row shows nothing. PS4 removed the muted branch and kept only the talking test (fed by
J1). To do: answer the muted icon first for muted players (same image record as PS3: type 3, image id, table 1 as the
talking icon already uses), then the PS4 talking branch.

**Pending** (scoreboard.cpp belongs to another branch right now). The muted state it needs is ready: `voice::muted`.

### J5. Scoreboard "mute this player" action (PS3 flow `0x443248`, branch at `0x44387c`; PS4 `0x5ed0ca`)

PS3: the in-match scoreboard sends action `0xb04c828b` for the row under the cursor; if the row holds a player who
is not the local player and has a connection, that player's mute is toggled. No message box, no log; the voice column
(J4) shows the new state. PS4 `0x5ed0ca..0x5ed166` still finds the row's scoreboard entry (team list and row index
as PS4 computes them) and then does nothing. To do: when that search finds the entry, toggle the mute of the entry's
player through `voice::toggle_mute`, never for the local player.

**Pending** (scoreboard.cpp, as J4). `voice::toggle_mute` already refuses the local player.

### J6. Voice volume back to full on each online start (PS3 `0x4e71d8`)

PS3 creates the speaker port at volume 1.0 in every NetInit, so the slider shows 20 again each time online
multiplayer is entered, whatever was set before. Ours keeps the last slider value for the process lifetime (start
value from `UNCHARTED_U2MP_VOICE_VOLUME`, default 1). Parity: put the volume back to its start value when the session
is torn down (or at the next online start). Open question 3 (the owner may prefer remembering it).

**Done** (owner: back to full). `session.cpp button_multiplayer`, when it starts a new session (the NetInit
counterpart), calls `voice::reset_volume()`: the volume goes back to its start value, 1.0 (slider 20) unless
`UNCHARTED_U2MP_VOICE_VOLUME` sets another; log `voice: volume back to X (online multiplayer entered)` when it
changed.

### J7. Own character's mouth follows the microphone (PS3 `0x51b3ec` -> `0x437c0` with `0x4e6f78`)

PS3 behaviour: section 1 item 8. `0x4e6f78` turns the encoded port's energy metering on and answers the current
loudness while voice activity is detected, else 0. PS4 has no voice library, so its character update only feeds the
voice-line value. To do: find the PS4 counterpart of the channel setter (PS4 code testing sid `0x94bb3775` at
`0x23c98b` / `0x47305b`) and its caller in the local player's update, and raise the fed value to at least our
capture loudness while our gate is open (`Gate` / capture callback already measure it; the scale must match the
voice-line values, open question 5). Remote characters are not affected on PS3 either.

**Off (not confirmed).** PS4 1.02: `0x23c98b` and `0x47305b` both lie in channel lookups for sid `0x94bb3775`;
`0x473010` is a setter (value in xmm0, channel list in rsi; it scales the value by the constant at `0xe6087c`, caps
it at `0xe60880` and acts on the channel only when the result is above 0). It has three callers, each feeding a sound object's amplitude
(vtable `+0xf8`) times a constant: `0x21d3a6` (object `+0x253c`), `0x607763` (tail jump, object `+0xc80`, sound
`+0x2c98`) and `0x714db6` (object `+0x2ec0`, clamped at 0). Which one is the local player's character update, and
whether the sid is the mouth (it is not in our name dictionary), cannot be told from the image alone, so nothing is
hooked. voice.cpp `log_mouth_state` checks the setter's bytes when the audio opens and logs `voice: own character's
mouth follows voice lines only, not the microphone (PS3 parity J7 off: channel setter 0x473010 present|differs, ...)`.
To finish: confirm the caller and the channel in game (for example by forcing a value through one call site), then
match the scale (question 5).

### J8. No talking icon for the local player (PS3 rows `0x4bcb38`, scoreboard `0x4c9de8` / `0x4c7c70`)

PS3 looks the voice state up through the member's connection, and the local player has none, so the own row never
shows the talking (or muted) icon anywhere. Ours: `voice::talking(local id)` is true while we send, so the own roster
row lights up. Parity: no icon for the local player. Open question 1 (a "you are transmitting" hint is useful with
push to talk).

**Done** (owner: PS3, no icon). `voice::talking` and `voice::muted` answer false for the local id, `frame` no longer
marks our own sending, and `toggle_mute` ignores the local id.

## 4. Test scenarios

1. J1/J2: two PCs in a match, B talks: A sees the talking icon over B's head and in B's scoreboard voice column for
   about a third of a second after B stops; A mutes B: name tag still shows talking, scoreboard shows the muted icon.
   B is an enemy beyond 12 m: no sound and no icons; within 12 m: both.
2. J1: B talks for 10 s during a match; B's scoreboard talk-time counter (entry `+0x14`) grows by about 10 s on A.
3. J3: online matchmaking, A's party of 1 meets stranger C: during gathering C's row shows the muted icon, C is not
   heard, A's popup on C says "Unmute"; A unmutes C by hand: C heard. Vote opens: C heard (also if A muted C again).
   After the match, next gathering with C still there: C still heard. D joins after the vote: muted until the next
   vote.
4. J3: party member E joins A's party: heard at once, no muted icon.
5. J4/J5: in a match, open the scoreboard, select C, press the mute action: C silent, voice column muted icon; again:
   heard, icon back to talking / nothing. Selecting your own row: nothing happens.
6. J6: set the slider to 5, leave online, enter again: slider at 20, voice at full volume.
7. J7: third-person in a lobby or match, speak: your character's mouth moves; silence: it follows only voice lines.
8. J8: talk in the party lobby: your own row shows no icon; the others see your talking icon.
9. EarnMedal: a medal earned on the host appears on the earner's PC with the PS3 pop-up and money; a forged event for
   an unknown net id changes nothing.

## 5. Open questions

1. ANSWERED (owner, 2026-11-25): J8: follow PS3, no icon on the own row.
2. ANSWERED (owner, 2026-11-25): J3: PS3 exactly, late joiners stay muted until the next vote.
3. ANSWERED (owner, 2026-11-25): J6: back to full on each online multiplayer entry.
4. SKIPPED (2026-11-25): Voice off from an RPCN chat restriction: the online program's interface gives the runtime no
   ticket subject status, so `UNCHARTED_U2MP_VOICE=off` stays the only switch.
5. J7: the channel's value range (voice-line amplitude scale at PS3 data `0xe10458` base, divisor `-0x7fc4`) and the
   loudness scale of cellVoice's energy level (0..1 assumed) must be matched to our RMS gate; confirm the channel is
   the mouth on PS4 and which of the setter's three callers is the local player's update (J7 Off paragraph) before
   shipping.
6. J1: the purpose of PS4 reader `0x66644b` (inside `0x666310`..) was not traced; it only reads the level.
7. J1: on a client the game's readers light only for the host (star topology: no connection record for the other
   clients). Relayed records would fix it but change PS4 event routing; owner's call.

## 6. Status table

| PS3 | Role (what it does) | Status | Where (our code / PS4) / item |
|---|---|---|---|
| 0x4dbe70 | bandwidth stats overlay (devkit DIP switch only) | PLATFORM | developer only; nothing |
| 0x4e6cd0 | voice receive: frames to the player's port, talking level 10, last-heard time | COVERED | voice.cpp `receive` / `use_voice`, `gamenet::set_talk_levels`; J1, J2 (client view of other clients: J1 open part) |
| 0x4e6e24 | master (speaker) volume read | COVERED | menus.cpp `query_volume`, `voice::volume` |
| 0x4e6e64 | one player's playback volume | COVERED | voice.cpp `update_gating` / `all_audible` (`parity-e` 2.1) |
| 0x4e6e94 | master volume set | COVERED | menus.cpp `query_volume`, `voice::set_volume` |
| 0x4e6ecc | is this player muted | MISSING | `voice::muted` (one flag, J3 done); J4 (scoreboard muted icon) pending |
| 0x4e6f04 | unmute and empty the queue (record reset) | PLATFORM | voice.cpp `forget` |
| 0x4e6f50 | set a player's mute | MISSING | `voice::toggle_mute` / `set_muted` / `unmute_strangers`, `frame` arrival and party join (J3 done); J5 (scoreboard action) pending |
| 0x4e6f78 | microphone loudness while voice is detected | MISSING | voice.cpp `Gate` (PLATFORM part); J7 (mouth) off, hook point not confirmed |
| 0x4e7038 | voice end: ports, stop, end | PLATFORM | `voice::stop` from the session teardown |
| 0x4e71d8 | voice init: ports at volume 1.0, mic -> encoder | COVERED | voice.cpp `open_audio` (PLATFORM part); `voice::reset_volume` from `button_multiplayer` (J6) |
| 0x4e7464 | voice send: one 120-byte chunk per packet, only to players heard | PLATFORM | voice.cpp `frame` (star relay); visible part J2 |
| 0x4e7560 | voice read: mic data to every connection queue, telemetry byte, level decay | COVERED | voice.cpp capture + `frame` (PLATFORM part); `decay_talk_level` in `frame` (J1) |
| 0x4e77e0 | per-connection voice port, queue, volume 1.0 | PLATFORM | voice.cpp `Speaker` on first packet |
| 0x4e7988 | read a named file into a buffer (not voice) | PLATFORM | progression_store.cpp / netpatch.cpp |
| 0x468dc0 | EarnMedalClient (60) Write: empty | COVERED | PS4 `0x61d270` |
| 0x468dc4 | EarnMedalClient (60) Read: empty | COVERED | PS4 `0x61d260` |
| 0x468dc8 | EarnMedalClient (60) Apply: empty | COVERED | PS4 `0x61d250` |
| 0x469bd4 | EarnMedal (61) Write (slot 3): net id, trigger, amount as 3 x 32 bits | COVERED | events.cpp `write_earn_medal` |
| 0x469ef8 | EarnMedal (61) Read (slot 2): the same three words | COVERED | events.cpp `read_earn_medal` |
| 0x46b760 | EarnMedal (61) Apply: award to the tracker with that net id | COVERED | PS4 `0x61d1e0` -> `0x5779a0` intact |

Counts: 21 rows. COVERED 12, PLATFORM 6, MISSING 3 (0x4e6ecc: J4, 0x4e6f50: J5, 0x4e6f78: J7). Items: J1, J2, J3, J6,
J8 done; J4, J5 pending (scoreboard.cpp); J7 off. J4, J5 and J8 come from tracing the callers of the mute and
talking queries.
