# Spec: PS3 parity, group C (match / gameplay net events, script natives, mode / player / weapon events)

Clean-room parity check of 60 PS3 1.10 functions (rows of `maintainer-tools/research/u2-parity/groupC.tsv`) against
the UCMR U2 runtime (`gow3_pc/src/u2mp`, branch `u2mp-ps3-parity`). It states what the PS3 game does, as seen
from outside, and whether our code does the same. It does not prescribe structure or names. Format and notation
follow `events.md` (addresses without "PS3" are PS4 1.02 image offsets; `ev+0xNN` is a byte offset into the
event object; "u32"/"f32" etc. are host little-endian values).

Status words:

- **COVERED**: our code (or the intact PS4 code it relies on) behaves like PS3.
- **PLATFORM**: PS3 used a facility the PC port replaced by its own design (transport, session layer). Only the
  observable result has to match.
- **MISSING**: absent, or present but different in the part named.
- **KEPT**: differs from PS3 on purpose (owner's decision); the difference is logged and documented.

## 1. Findings that apply to many rows

### 1.1 Slot labels in the TSV

The TSV (and `FUNCTIONS-MATCH-GAMEPLAY.md` section 7) names the PS3 serializers the wrong way round. Every PS3
function the TSV calls "Write (slot 2)" consumes bits into the event (a Read), and every "Read (slot 3)" produces
bits from the event (a Write). PS4 slot 2 (vtable + 0x10) is Read and slot 3 (vtable + 0x18) is Write, as
`events.md` 1.1 says. The status table below gives the real role. Our copies (`u2mp/docs/functions-match-gameplay.tsv`,
`FUNCTIONS-MATCH-GAMEPLAY.md` section 7) are corrected; the research group TSV still has the old labels.

### 1.2 Payload offsets PS3 to PS4

On PS3 an event's scalar payload starts at ev+0x08; on PS4 at ev+0x0c. Scalar fields therefore sit 4 bytes later
on PS4. Fields that are 16-byte aligned on PS3 (locators at +0x10 / +0x30, the hidden-treasure position at +0x10,
the start-npc-fight block +0x50..+0x79, the snapshot block +0x30..+0x38) keep the same offset on PS4. The NetEventEvent
memento was re-laid out on PS4 (8 inline argument slots, see `events.md` 4.4). All field lists in `events.md`
4.3..4.5 match the PS3 serializers field for field, width for width and in order; the checks are summarised in
the table.

### 1.3 Serializers: wire format verified

For types 37, 38, 44, 47, 49, 51, 52, 53, 54, 55, 57, 67, 76 the PS3 Read and Write produce and consume exactly the
`events.md` wire layout (same fields, same order, same widths: 32-bit ids, 12-bit player / attack indices, 6-bit
attack index, 4-bit counts, 1-bit flags, 16-bit zone indices, the 11-bit time, the 3 x 18-bit position, the locator
with 1-bit presence + 13-bit quaternion components). Details that were checked specially:

- PlayVox (53) and TerritorySetTimer (52): PS3 writes the flag as the low bit of its byte but chooses the following
  field (12-bit vs 32-bit width for 53, presence of the trailing bit for 52) from "byte is non-zero". Our writer
  copies that mismatch. The flag write on PS3 is a 1-bit write of the byte value, so the low bit is what travels.
- NetEventEvent (47): PS3 Read takes every argument on the wire (up to 15), decodes types 1 (boolean = raw value
  non-zero), 2, 3 and 6 (raw 32-bit value) and turns every other type into value 0 / type 0. The memento copy
  that stores them keeps at most 7 arguments (PS3 prints a warning when it truncates). PS3 Write sends 3 ids
  (target, message, sender), the 4-bit count and per argument 4-bit type plus 32-bit value; for booleans PS3 sends
  the raw 32-bit slot word (non-zero when true) where we send 0/1. Both receivers only test "non-zero", so the two
  are interchangeable on the wire.
- TerritoryOwn (49): PS3 Read has no upper bound on the 4-bit count (a count of 11..15 would run over the next
  fields); our Read rejects such a stream and our Write clamps the count to 10. PS3 never sends more than 10, so
  this only changes behaviour on malformed input. The f32 at the end exists only when the game mode is 12, on both
  sides, as on PS3.
- NpcDealDamage (44): only the PS3 Read is in this group; its third field is a full 32-bit read (signed damage),
  as ours.

### 1.4 Applies whose PS4 body is intact

The TSV calls nine Applies "partial" only because their Read/Write were stubs on PS4. With `events.cpp` restoring
the payload, the intact PS4 bodies do the PS3 work. Checked by the identifying ids and calls each PS4 body still
contains: TerritorySetTimer `0x61e610` (process lookup, message ids `0x63a87964` / `0x917a57a7`), NpcKill
`0x621f80` (tracker lookup, death call through the NPC's component slot 4), TurretSnapshot `0x61be10`
(`0x5313dbd1`), SimpleSnapshot `0x61e1c0` (`0x3b4cacd2`), PlayVox `0x61e2b0` (player or NPC lookup, 4 typed
arguments: two string ids, two floats), SpawnHiddenTreasure `0x61d280` (`0xf0c80b36`, `0x67a82b28`, owner id into
the spawned object), TerritorySpawn `0x61e710` (`0x58609830`, `0xfa1c0a7a`, `0x4bd2a224`), TerritoryOwn `0x61eb90`
(`0x242cbf2`, `0xfcfed5d0`, `0x7b4d4744`, `0x289d4d29`, the mode-7 branch), StartNpcFight `0x61d6d0` (all 14 ids of
the PS3 body, from `0x939b5514` to `0x6d7dd92d`) and NetEventEvent `0x61fa10` (target lookup, then the memento is
delivered to that process). Nothing to implement for them.

## 2. MISSING and PLATFORM items

### 2.1 NetEventNpcSpawn::Apply (type 37) - PS3 0x475578, PS4 0x62f220 (our `events.cpp`): KEPT (one case) + PLATFORM

Wire: unchanged (`events.md` 4.3: 32-bit npc unique id, 32-bit spawner id, 32-bit second spawner id, 32-bit spawn
tag; 128 bits).

PS3 behaviour on the receiving machine, in order:

1. Look up a live process with the npc unique id. If one exists, PS3 reports a soft error (its assert line 0x1875,
   "already exists") **and then carries on** with step 2. It does not stop.
2. Look up the spawner by spawner id, and, only when the second id is non-zero, the second spawner.
3. Spawner missing: soft error (line 0x187a, "can't find spawner"), stop.
4. Build the spawn info from the spawner, initialise the spawn parameters from it, set the parameters' spawner id
   to the second id and, when that id names a spawner, attach that spawner to the parameters; set the spawn tag.
5. Spawn the NPC: with the second spawner's locator when the second spawner exists, else at the spawner's own
   place.
6. Spawn failed: soft error (line 0x188b), stop.
7. Write the npc unique id into the new NPC's name/id field (PS4 npc+0x68) and register the NPC with the
   NetGameManager NPC tracker table (PS4 `0x644fa0`, third argument the NPC's own field at +0x3708). Registration
   failed: soft error (line 0x1890).

Our behaviour differs in two places:

- **KEPT (case "already exists")**: we log
  `NetEventNpcSpawn: npc %u already exists; not spawning a second one (deliberate difference from PS3)` and stop;
  PS3 logs and spawns a second NPC with the same unique id. Owner's decision (open question 1): keep the stop. It
  is safer and the case should not occur when the host is the only sender. (To follow PS3 one would keep the log
  line and continue at step 2 as for a new NPC.)
- **PLATFORM (client binding, `bind_remote_npc`, `local_npc_init`, `local_npc_despawn`)**: on PC a client may already
  have spawned the same NPC from its own copy of the spawner. Before step 1 our Apply returns at once on the match
  host, and on a client first tries to bind the event to such a local NPC (same spawner key, or for key 0 the same
  two copied spawn fields), writing the unique id and registering the tracker instead of spawning. Parity
  requirement for this design: the end state on every client must equal PS3's: exactly one NPC per host NPC,
  carrying the host's unique id, registered once in the NPC tracker table, standing where the second spawner's
  locator (or the spawner) puts it. When no local NPC matches, the full PS3 path (steps 1..7) runs. A local NPC that
  despawned or whose process id no longer matches must never be bound. On the host the event has no effect (on PS3
  the host is the sender and does not receive it).

Edge cases to keep: second id 0 (no second lookup, no locator, the parameters still get id 0 and the unconditional
re-lookup); spawner not found with a non-zero second id (the second lookup happens before the error); registration
result judged by its low byte only.

### 2.2 NetEventKickGriefer::Apply (type 69) - PS3 0x4711f4, PS4 stub 0x61b900 (our `remaining.cpp` B7): COVERED (block implemented, PLATFORM limits below)

Wire: one 32-bit net id (the player to remove) at the payload start (PS4 ev+0x0c). PS4 Read `0x61b910` and Write
`0x61b9b0` are intact; the sender `0x617690` (the "kick griefer" vote path) is intact.

PS3 behaviour:

1. Only the match host acts (the NetGameManager host test). On any other machine the Apply does nothing.
2. The host removes the member with that net id from the match room **with the block option set** (PS3 Matching2
   kickout, block flag 1): the kicked player cannot join that same room again while the room exists.
3. No game state is changed directly by the Apply; the removal reaches the game through the normal "member left"
   handling.

Our behaviour (implemented; `session.md` 5.1 "Block list" and 5.15 `kick_griefer`): on the host the Apply
calls `session::kick_griefer(net id)`.

- Used member: the kick message is sent (the kicked PC closes its transport and starts fresh) and the member's
  online id (NpId, read from its transport address before the kick) goes into the host's block list. The key is
  the online id, not the net id, because a rejoin gets a new net id.
- The block list lives exactly as long as the hosted transport: every transport close (teardown, kicked / host
  lost, online transport switch, match migration) and every transport start empties it. A new hosted session
  therefore starts with no blocks.
- A Hello from a blocked online id gets no slot, no Welcome and no peer event, exactly as a full party turns a
  joiner away; the host logs `party: refused a join from %s (kicked as a griefer; %u tries)` on the first try and
  then once per 20 tries. Online the joiner then gives up through its normal "room owner not reached" path.
- Only the griefer kick blocks. The party-menu "Kick From Party" (`0x5ec4a0`) keeps its behaviour (no block):
  whether PS3 sets the block flag there is not checked (open question 2), and the owner's rule is not to block
  while that is open.
- Net id of the host itself: PS3's kickout wrapper treats kicking your own member as leaving the room
  (`PORT-MATCHMAKING.md`), so the host leaves. We do the same: the host logs
  `party: this PC (the host) was named as the griefer: leaving the session`, sets the network error byte and
  closes its transport on the next frame as a kicked client does (online: it leaves every room, so the room's
  new owner takes over; LAN: the clients lose the host).
- Net id unused or out of range: nothing happens apart from the existing `NetEventKickGriefer: can't kick net id
  %u` log (PS3: the room server rejects the request, no game effect).

PLATFORM limits that remain (no player-visible difference in a normal online match):

- The block is enforced by our host at the transport, not by a room server. A blocked player can still enter the
  RPCN room for a few seconds but never reaches the host, so it never appears in the game.
- A player without an online id (LAN / direct IP play) is kicked but not blocked (logged
  `party: net id %u has no online id: kicked, not blocked`); PS3 had no such play.
- The block list belongs to the PC that hosts. After a match migration the new host starts with an empty list;
  PS3's room-server block would survive an owner change.

### 2.3 NetEventStartConnection::Apply (type 0) - PS3 0x474e50, PS4 stub 0x63c0e0 (our `gameplay2.cpp`): PLATFORM

PS3: the event has no payload (Read and Write are empty on PS3 too). The host sends it to a joining peer; the
peer's Apply raises a "connect to the others" request, the peer's network update then connects to every member and
answers the host with ConnectionDone (can-talk-to-all bit and two averages that are only printed).

Ours (`events.md` 4.9): a client answers at once with ConnectionDone(host net id, can-talk-to-all 1, 0, 0) through
the surviving PS4 sender, because on PC every client reaches the others through our transport. Nothing on PS4 or
in our session layer sends StartConnection today. Parity requirement: by the time the match starts each remote
player's tracker is marked connected (+0x275 = 1) and has its can-talk-to-all flag (+0x27c) set; `session.cpp`
does this when it registers a player. Nothing to implement while that holds.

### 2.4 NetEventConnectionDone::Apply (type 1) - PS3 0x474d90, PS4 stub 0x63bda0 (our `gameplay2.cpp`): PLATFORM

PS3: on the host, the tracker of the peer being connected (held in a PS3-only pending id) gets can-talk-to-all from
the payload and is marked connected; the pending id is cleared; the two averages are only printed. Ours
(`events.md` 4.10) finds the sending player from the connection record the event came in on (PS4 has no pending
id) and sets the same two tracker bytes. Same parity requirement as 2.3; nothing to implement.

## 3. Cleanup notes (not parity gaps)

- Done: the second NpcDealDamage Apply in `events.cpp` (`apply_npc_deal_damage` / `apply_npc_damage` and its
  helpers) is removed; the type 44 port entry has `apply = nullptr` again (`events.md` 2.2). It was never installed
  (apply stub 0) and differed from PS3 (46 attack-type indices instead of 45, a different time conversion, trackers
  with an empty name skipped). The only Apply is `remaining.cpp` B6.
- `events.cpp` NetEventEvent Read/Write print up to 400 diagnostic lines (`net event 47 in/out ...`). PS3 printed
  such lines only with its net debug flag. Kept on purpose: the team still uses them for testing; drop or gate
  before a release.
- Done: the slot labels in `u2mp/docs/functions-match-gameplay.tsv` and `FUNCTIONS-MATCH-GAMEPLAY.md` section 7
  now say Read = slot 2, Write = slot 3 (1.1), and the TSV rows for `0x45212c` / `0x453c10` say they are ported
  (`netanim.cpp`, `PORT-NETANIM.md`, `netanim.md` 1.4.3). The research copy of the group TSV is not ours to edit
  here; it still has the old labels.

## 4. Open questions

1. Decided (owner): NpcSpawn "already exists" keeps our stop (KEPT, 2.1); the log line names it as a deliberate
   difference.
2. Decided (owner): KickGriefer blocks by online id for the lifetime of the hosted session, cleared when hosting
   ends; the host named as griefer leaves (2.2). Still open: whether the party "Kick From Party" path uses block 0
   on PS3 (read PS3 `0x439cf8` -> `0x4d8a98` arguments). Until checked, Kick From Party does not block.
3. net-apply-npc-fight-damage: the PS3 decompile passes an uninitialised local as the kill-info time; we pass the
   current frame counter. Check the PS3 assembly of `0x4cdc90` around the `0x48a3c0` call to see what is stored
   there (affects only the kill-feed time stamp).
4. `0x4862b4` (and its siblings): PS3 writes a global into the caller's result; every PS4 counterpart of this slot
   family is a bare `ret`, including those of working events. Treated as a systematic PS4 build difference with no
   reader; confirm no PS4 caller reads the result if a related bug shows up.

## 5. Test scenarios for the items of section 2

1. NpcSpawn with an existing unique id: one log line (naming the deliberate difference), no spawner lookup, no
   spawn.
2. NpcSpawn on a client with a matching local NPC: bound, no spawn call; with a stale local NPC: not bound, full
   spawn; on the host: no effect.
3. KickGriefer on a non-host: nothing. On the host with a used member that has an online id: kick message sent,
   online id blocked; the same player's Hellos are refused (no slot, log on the 1st and every 20th); after the
   host closes the session and hosts again, the player can join. A member without an online id: kicked, not
   blocked. The host's own net id: the host leaves (network error byte, transport closed next frame). Unused id /
   id 12: `can't kick` log, nothing blocked. Kick From Party: never blocks. (`tests/test_u2mp_net.cpp`
   `refused_join` covers the transport side: a refused Hello gets no slot and the client joins once admitted.)

## 6. Status table

Role = what the PS3 function really does (1.1). "PS4 intact" = the PS4 function at that address does the work.

| PS3 | Role | Status | Where (our code / PS4) |
|---|---|---|---|
| 0x45212c | NetAnimList::AddAnim (16-entry list, append ignored when full) | COVERED | `netanim.cpp` slot 4 list (netanim.md 1.4.3) |
| 0x453c10 | NetAnimCmdGenerator slot (fire anim events between snapshots) -> PS4 0x5f48d0 | COVERED | `netanim.cpp` slot 4 host |
| 0x456a40 | empty vtable slot | COVERED | PS4 0x5f8240 empty too |
| 0x459ec8 | vtable slot returning id 0xcde4b742 | COVERED | PS4 0x5fb7a0 same value |
| 0x459f10 | vtable slot returning 0x20 | COVERED | PS4 0x604e80 same value |
| 0x45e728 | NetCharacter ground-pattern probe ("(no pat found)" debug text) | COVERED | PS4 inlined in 0x5fe4b0; only debug text gone |
| 0x468c18 | StartConnection Read (type 0; empty) | COVERED | empty on both |
| 0x468c1c | StartConnection Write (empty) | COVERED | empty on both |
| 0x468dcc | CompleteTask Read (type 65; empty) | COVERED | empty on both |
| 0x468dd0 | CompleteTask Write (empty) | COVERED | empty on both |
| 0x469b50 | KillEntity Write (76) | COVERED | `events.cpp` port 76 write (0x61afc0) |
| 0x469b80 | TerritorySpawn Write (51) | COVERED | `events.cpp` port 51 write (0x61e880) |
| 0x469e6c | KillEntity Read (76) | COVERED | `events.cpp` port 76 read (0x61afd0) |
| 0x469e9c | TerritorySpawn Read (51) | COVERED | `events.cpp` port 51 read (0x61e890) |
| 0x46a408 | PlayVox Write (53) | COVERED | `events.cpp` port 53 write (0x61e5f0) |
| 0x46a4b8 | TerritorySetTimer Write (52) | COVERED | `events.cpp` port 52 write (0x61e6f0) |
| 0x46a6fc | NpcKill Write (38) | COVERED | `events.cpp` port 38 write (0x622070) |
| 0x46a730 | NpcSpawn Write (37) | COVERED | `events.cpp` port 37 write (0x62f230) |
| 0x46ae78 | PlayVox Read (53) | COVERED | `events.cpp` port 53 read (0x61e600) |
| 0x46af48 | TerritorySetTimer Read (52) | COVERED | `events.cpp` port 52 read (0x61e700) |
| 0x46b1cc | NpcKill Read (38) | COVERED | `events.cpp` port 38 read (0x622080) |
| 0x46b200 | NpcSpawn Read (37) | COVERED | `events.cpp` port 37 read (0x62f240) |
| 0x46bd8c | StartNpcFight Write (55) | COVERED | `events.cpp` port 55 write (0x61e1a0) |
| 0x46c198 | StartNpcFight Read (55) | COVERED | `events.cpp` port 55 read (0x61e1b0) |
| 0x46c650 | SpawnHiddenTreasure Write (57) | COVERED | `events.cpp` port 57 write (0x61d390) |
| 0x46cb60 | SpawnHiddenTreasure Read (57) | COVERED | `events.cpp` port 57 read (0x61d3a0) |
| 0x46d124 | TurretSnapshot Write (67) | COVERED | `events.cpp` port 67 write (0x61bed0) |
| 0x46d218 | SimpleSnapshot Write (54) | COVERED | `events.cpp` port 54 write (0x61e290) |
| 0x46dc98 | TurretSnapshot Read (67) | COVERED | `events.cpp` port 67 read (0x61bee0) |
| 0x46dda8 | SimpleSnapshot Read (54) | COVERED | `events.cpp` port 54 read (0x61e2a0) |
| 0x46de30 | NpcDealDamage Read (44) | COVERED | `events.cpp` port 44 read (0x61fe00) |
| 0x46f4e8 | TerritoryOwn Write (49) | COVERED (count clamp, 1.3) | `events.cpp` port 49 write (0x61f020) |
| 0x46fc10 | TerritoryOwn Read (49) | COVERED (count > 10 rejected, 1.3) | `events.cpp` port 49 read (0x61f030) |
| 0x470fc8 | NetEventEvent Apply (47) | COVERED | PS4 intact 0x61fa10 + `events.cpp` port 47 |
| 0x4711f4 | KickGriefer Apply (69) | COVERED (block + host leaves, 2.2) | `remaining.cpp` B7 (0x61b900) + `session.cpp` `kick_griefer` |
| 0x474d90 | ConnectionDone Apply (1) | PLATFORM (2.4) | `gameplay2.cpp` (vtable 0x10414d0 slot 4) |
| 0x474e50 | StartConnection Apply (0) | PLATFORM (2.3) | `gameplay2.cpp` (vtable 0x1041520 slot 4) |
| 0x475578 | NpcSpawn Apply (37) | KEPT (already-exists case) + PLATFORM (client binding), 2.1 | `events.cpp` `apply_npc_spawn` (0x62f220) |
| 0x475d38 | TerritorySetTimer Apply (52) | COVERED | PS4 intact 0x61e610 + port 52 |
| 0x4767e4 | NpcKill Apply (38) | COVERED | PS4 intact 0x621f80 + port 38 |
| 0x476e70 | TurretSnapshot Apply (67) | COVERED | PS4 intact 0x61be10 + port 67 |
| 0x476fac | SimpleSnapshot Apply (54) | COVERED | PS4 intact 0x61e1c0 + port 54 |
| 0x4785d0 | PlayVox Apply (53) | COVERED | PS4 intact 0x61e2b0 + port 53 |
| 0x4789c8 | NetEventEvent Read (47) | COVERED (7-argument cap as PS3) | `events.cpp` port 47 read (0x61fa60) |
| 0x4792b0 | NetEventEvent Write (47) | COVERED | `events.cpp` port 47 write (0x61fa50) |
| 0x47ae08 | SpawnHiddenTreasure Apply (57) | COVERED | PS4 intact 0x61d280 + port 57 |
| 0x47af00 | TerritorySpawn Apply (51) | COVERED | PS4 intact 0x61e710 + port 51 |
| 0x47b468 | TerritoryOwn Apply (49) | COVERED | PS4 intact 0x61eb90 + port 49 |
| 0x482718 | StartNpcFight Apply (55) | COVERED | PS4 intact 0x61d6d0 + port 55 |
| 0x4862b4 | event-class getter (stores a global into the result) | COVERED (systematic, question 4) | PS4 0x63c740 ret, like all siblings |
| 0x48a620 | NpcDealDamage Apply (44) | COVERED | `remaining.cpp` B6 (0x61fa70), the only copy (3) |
| 0x494ee0 | HashTable init, 0x30-byte nodes (NGM table 256) | COVERED | `netinit.cpp` (ngm+0x1c10; NETINIT.md) |
| 0x4950f0 | HashTable init, 0x4c-byte nodes (NGM NPC table 16) | COVERED | `netinit.cpp` (ngm+0x1ba0; NETINIT.md) |
| 0x4ccab8 | native net-coop-host-script-id (current task's host script, list default fallback) | COVERED | `natives.cpp` (0x669c40) |
| 0x4cd66c | native net-get-capture-zone (spawner 0 data key 0x7af5d23b) | COVERED | `natives.cpp` (0x6687d0) |
| 0x4cd6e8 | native net-player-coop-id (3 spawner ids by local coop index) | COVERED | `natives.cpp` (0x669240) |
| 0x4cd8ac | native net-coop-script-id (current task's script, default fallback) | COVERED | `natives.cpp` (0x669c30) |
| 0x4cdc90 | native net-apply-npc-fight-damage | COVERED (question 3) | `natives.cpp` (0x668db0) |
| 0x4ce030 | native net-get-net-id-from-player | COVERED | PS4 intact 0x668fe0 (same loop, not a cut) |
| 0x4ce6f0 | native net-get-player-by-net-id | COVERED | `natives.cpp` (0x6690b0) |

Counts: 57 COVERED, 2 PLATFORM, 1 KEPT, 0 MISSING.
