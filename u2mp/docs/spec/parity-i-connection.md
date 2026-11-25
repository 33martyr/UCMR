# Spec: PS3 parity, group I (packet layer, message layer, sockets, NP connection manager, NP utilities)

Clean-room parity check of the 65 PS3 1.10 functions of `maintainer-tools/research/u2-parity/groupI.tsv`
(net-connection.cpp / net-message 37, np-connection-manager 15, net-socket 7, np-util 6) against the UCMR U2 runtime
(`gow3_pc/src/u2mp`, branch `u2mp-ps3-parity-2`). It says what PS3 does on the wire and what a player sees, and
whether our port gives the same result. It does not prescribe code, structure or names. Addresses are PS3 1.10
(`NPUA80698` EBOOT) unless marked PS4 (PS4 1.02 image offset). Background: `TRANSPORT-INTERFACE.md` (field maps of the
connection records and the message layer, wire format), `FUNCTIONS-FRONTEND-NET.md`, `PORT-ONLINE.md`,
`FUNCTIONS-MATCH-GAMEPLAY.md` 7 (event types), specs `parity-b-menus.md` 2 (connection bars), `parity-e-flow2.md`
2.2 / 2.3 / 3.1 (timeouts, ping, per-frame update), `parity-f-party-mm.md` F5 / F11 (bad rooms, party owner check).

Status words (as in `parity-e-flow2.md`):

- **COVERED**: our code, or the intact PS4 code it relies on, gives the same visible result.
- **PLATFORM**: PS3 plumbing (sockets, signaling, NP, NetCtl, debug output) that the PC design replaces. The
  visible result must still match; the replacement is named and any delta stated.
- **MISSING**: absent, or present with a different visible (or on-the-wire timing) result in the part named.

Reading notes. All 65 OPD descriptors carry TOC `0xe183b8`. These modules do not read their constants straight off
r2: each function first loads a module data pointer from the TOC area (e.g. TOC slots `0xe10b14`, `0xe10b20`,
`0xe10b88`, `0xe10ba0`, `0xe10ba4`) and addresses its strings and floats at negative offsets from it; every string and
number below was resolved that way. The decompile `groupI-ps3.c` was checked against the disassembly where it mattered:
the connection-status function (`0x4e8f1c`) really returns the signaling state (Ghidra shows only 0 / -1), the lag test
(`0x464b18` / `0x4e954c`), the send scheduling in `0x43cdb8`, the event vtables (to find which event types override
the expiry method) and the party / matchmaking callers of `0x4e8978`.

## 1. Facts every item uses

### 1.1 Two layers on PS3, and ours

PS3 stacks three pieces per remote peer:

1. **Socket layer** (net-socket): one UDP-P2P socket (port 3658, virtual port 1, encryption and signature options on,
   non-blocking) for game traffic and one plain UDP socket for other traffic. A send hook can simulate loss, delay
   and duplication (developer tunables, all 0 in retail, see 1.5).
2. **Packet layer** (NetConnection, `0x464cb8..0x4654c0`): sequence numbers, a 32-bit ack field, a send window, the
   last send / last receive times (1.2).
3. **Message layer** (`0x467378..0x468ed8`): queues of game events packed into each packet, three delivery modes,
   in-order delivery of reliable events, relay lists, expiry, per-type statistics (1.3).

Connection records (np-connection-manager, 32 records, 1.4) bind a peer's online id to a signaling connection and own
one packet+message layer object each.

Ours: `net.cpp` is our own datagram transport (UDP on LAN, the online program's link online; hello / welcome / bye,
its own sequence and ack, 0.1 s keepalive, 10 s timeout). Inside it, `gamenet.cpp` re-creates the PS3 packet layer
(header, window, ack processing) around the PS4 build's message layer, which survived intact (PS4 `0x60a110` unpack,
`0x60ac70` pack, `0x60b780` delivered, `0x60b8f0` lost, `0x60ba10` reset, `0x609fc0` queue; TRANSPORT-INTERFACE 1, 7).
The PS3 connection manager is replaced by the online program's peers (`online.cpp`, `u2_online_abi.h`: peer states
inactive / pending / active / dead, RTT) and by `gamenet.cpp attach/detach` filling the PS4 records `0x1152a10`.

### 1.2 PS3 packet layer facts (wire format in TRANSPORT-INTERFACE 2)

- Header: a "has messages" bit, the 11-bit packet sequence, a 10-bit ack (the highest sequence received from the peer)
  and a 32-bit field saying which of the 32 packets up to the ack arrived *with messages*.
- Window: before a packet is built, if **more than 30** packets are unacknowledged the packet goes out as an ack-only
  header ("has messages" 0, sequence not advanced, no message body) and a debug line `<name> window full` is printed.
  Otherwise the sequence advances, a packet record is appended to the in-flight list and the message layer packs the
  body. So at most **31** packets are ever in flight.
- Receive: the last-receive time is stamped **first**, for every datagram handed to the connection, before any check.
  Then: a sequence that is not 0..31 ahead of the last one (after extending the 11 bits upward) makes the whole packet
  ignored silently; an ack beyond our own newest sequence makes it ignored with `<name> Ignoring Packet <ack> > <sent>`.
  An ack that advances by 32 or more prints a separator line with the name and still resolves every record (those
  beyond the 32-bit field count as lost). For each resolved record: bit set -> delivered, else lost. Only a new packet
  with messages is unpacked. Records must resolve in send order (assertion in the PS3 build).
- Silent test (`0x4651b4`): "nothing received for more than T seconds", false while nothing was ever received. Its
  only caller (NpNetUpdate) uses T = 20 s (`parity-e-flow2.md` 2.2).
- Lag test (`0x464b18`): "more packets in flight than L", L clamped to 0..30; `0x4e954c` answers "every live record
  (in use, signaling connection set, port set) is over L" (also true when there is none). Its caller is the
  TaskManagerOnline update, whose PS4 copy is intact (PS4 `0x5f14d0`, TRANSPORT-INTERFACE 1.4) and reads the PS4
  record counters that `gamenet.cpp` keeps.

### 1.3 PS3 message layer facts (all present in PS4; listed for the items below)

- Modes per event: 0 reliable ordered (gets a 7-bit-on-the-wire reliable sequence; resent on loss), 1 unreliable but
  re-queued at the front on loss, 2 fire-and-forget (released on loss). Mode 0 waits in the reliable queue; modes 1
  and 2 share the unreliable queue.
- Pack: unreliable section first, then reliable messages whose sequence is at most 64 past the highest contiguously
  acked one; stop at 9600 bits ("Exceeded Packet size 1200 bytes"). Each message may carry a relay list (up to 15
  16-bit net ids, 4-bit count) and a "relay only" flag.
- Unpack: reliable messages are delivered strictly in order (out-of-order ones parked); an unknown type aborts the
  rest of the packet with a debug line. An event flagged "in game only" is applied only while the game manager's state
  is "in game".
- **Expiry**: every event type has an "expired?" method. The base version says never. **Only type 12**
  (NetEventCharacterMove, the player state snapshot, mode 2) overrides it: it is expired when the net clock is more than
  **1.0 s** past the time stamp the snapshot was filled with (the first payload field, the net clock at fill time).
  Expired unreliable messages are removed from the queue by a per-frame pass (`0x467d20`, debug line
  `Message type <type> has expired`) and skipped by the pack.
- Receive hook (`0x4674e0`): a function + context called for every received message. PS3 installs a recorder that
  does nothing unless a developer flag is set; PS4 installs a no-op.
- Statistics: per type (80 types) count, total bits, max, min (`0x468dd4`, inline in PS4 pack) and a debug table
  printer (`0x468ed8`).

### 1.4 PS3 connection manager facts

- 32 records; record ids are handed out from a counter that starts at `0x1234` and only grows. A record holds the
  peer's online id, IPv4 address, signaling connection id, a reference count and the packet+message layer object; the
  online id doubles as the connection's name in log lines.
- Connect by online id (`0x4e9e98`): refused (-1) if the id is on the unreachable list (1.4.1); an in-use record with
  the same id gets one more reference and its id is returned; otherwise the first free record is taken (reset, new id,
  reference 1, layers reset), and an existing signaling connection to that id is adopted. 32 in use -> -1.
- Activate (`0x4e9060`): starts signaling to the record's peer if not started (activating our own id fails quietly).
- Release (`0x4e8c8c`): one reference less; at 0 the layers are reset, the record is freed and signaling is ended.
- Signaling events (`0x4e9618`, `0x4ea438`): "dead" frees the record and ends signaling (see 1.4.1); "established"
  stores the peer address; an incoming activation from a peer with no record creates a record **without** keeping a
  reference and activates back; "peer deactivated" frees the record (`*************** <id> has left :
  SCE_NP_SIGNALING_EVENT_EXT_PEER_DEACTIVATED`); "mutual activation" records the connection; events for unknown
  connections print `CONNECTION NOT FOUND` and end that connection.
- Status (`0x4e8f1c`): the signaling state of the record's connection: **0 inactive, 1 pending, 2 active**, -1 when
  the record is missing, not in use, has no signaling connection or the query fails.
- Quality (`0x4e8e90`): 1..5 bars from the record's round-trip time in ms (the 10 s ping of `parity-e-flow2.md` 2.3):
  up to 49 -> 5, up to 79 -> 4, up to 119 -> 3, up to 199 -> 2, above -> 1; 0 for a negative or unknown id.

#### 1.4.1 The unreachable list (`0x4e8978`)

When signaling reports a peer's connection dead **with the reason "peer unreachable"** (SCE signaling error
`0x8002a80f`; SDK name from memory, verify) and the peer has a record, its online id is appended to a list of 32 slots
(the 33rd entry overwrites the oldest; duplicates are not merged). The list is emptied only when the connection manager
is initialised (NetInit, i.e. each time the player enters online multiplayer). Membership is used three times:

1. Connect by online id refuses the peer at once (no signaling attempt).
2. Matchmaking room choice (`0x4b0a58` state 6, call at `0x4b14d4`) skips a room whose owner is on the list.
3. Party room machine state 6 (`0x4c41f8`, call at `0x4c49fc`): while the link to the party owner is not established,
   a listed owner makes the member leave the party room **at once**; an unlisted owner only after the 5 s of F11 (b).

### 1.5 Sockets and the network simulator

The send hook reads three developer tunables (matchmaking tunables block `0xf2264c` +0 / +4 / +8: drop percent, added
delay in ms with random jitter, duplicate percent). The tunables initialiser `0x4b3f40` sets all three to 0, so a retail
PS3 never drops, delays or duplicates on purpose. Byte counters add 52 bytes of overhead per datagram and keep
per-second history for a debug line (`sent / recv %3.2f KBps %i PPS min: %i max: %i`). Nothing here is player-visible.

### 1.6 Clocks

The packet layer times (last send / receive, 0.05 s / 2 s send gates) use the frame clock word `+0xec` of the clock
object `0xe60a00`; the snapshot time stamp and its expiry test use the net clock word `+0x1cc` of the same object.
Both count 1/300 s.

## 2. What our port does today

- `gamenet.cpp send_peer` (rules in `gamenet_rules.h`, tested by `tests/test_u2mp_gamenet.cpp`): header as PS3;
  header-only when **more than 30** are unacknowledged (31 in flight at most; `slot <n> window full` logged once per
  stall); a fixed 50 ms tick that advances whether or not a packet goes out; on a tick it sends if a queue is
  non-empty, an in-flight packet holds a mode 0 / 1 message, or the 2 s keepalive (moved by each packet) is due (I1,
  I2 done).
- `gamenet.cpp receive_peer`: stamps the last-receive time first, for every datagram (PS3 order); PS3 acceptance rules
  (refuses jumps of 32+ and acks beyond our sequence), delivered / lost per record, unpack for new packets with
  messages.
- `gamenet.cpp expire_peer`, every frame for every peer before the send decision: unlinks the type-12 (mode 2)
  messages of the unreliable queue whose time stamp is more than 1.0 s behind the net clock, keeping the others in
  order, logs `Message type 12 has expired (<count>, slot <n>)` once per pass, and releases them through the PS4
  packet-lost method (mode 2 = released, as in the reset) (I3 done, run-time checks below).
- `session.cpp mm_roster_query`: bars from `online::peer_rtt` (`bars_of`, PS3 thresholds) and colour `0x800000FF`
  when the peer is active, `0x80808080` otherwise.
- `online.cpp`: `mm_bad_rooms` (rooms that failed this session, by room id) stands in for the unreachable list;
  party owner check `kPartyHostLost` 5 s (F11 b).
- `net.cpp`: 10 s silence timeout for every peer (`parity-e-flow2.md` 2.2 asks for 20 s).

## 3. Items to implement

### I1. Send window of 31 packets (PS3 `0x4652cc`) - COVERED (gamenet.cpp, 2026-11-25)

PS3 lets 31 packets be unacknowledged; the 32nd send attempt (and every later one while 31 are unacked) becomes an
ack-only header. Ours switches to ack-only at 30. Change the rule to "ack-only when more than 30 are in flight". Effect
beyond throughput: the intact PS4 lag test (1.2) compares the in-flight count with a limit clamped to 30 and fires only
when the count is *above* it; with a 30-packet window that state can never be reached at the maximum limit, with 31 it
can, as on PS3. Optional while there: print `<peer name> window full` on the debug channel once per stall, and stamp
the record's last-receive time before the acceptance checks (PS3 order; no visible effect because liveness lives in
net.cpp).

### I2. When a connection sends (PS3 `0x43cdb8` with `0x467438`) - COVERED (gamenet.cpp, 2026-11-25)

PS3, every frame, per connection (after the expiry pass I3):

1. A 50 ms tick: when the tick time has come, the next tick is set to now + 0.05 s **whether or not a packet goes
   out**; between ticks nothing is sent.
2. On a tick, a packet is sent if: a message is queued (reliable or unreliable queue non-empty), **or** an in-flight
   packet still holds a message of mode 0 or 1, **or** the 2 s keepalive deadline has passed. Each packet sent moves
   the keepalive deadline to now + 2 s.

Ours differs in two ways: (a) any in-flight packet counts, also one holding only mode-2 snapshots, so a peer keeps
sending empty 20 Hz packets until its snapshots are acked where PS3 falls back to the 2 s keepalive; (b) our 50 ms gate
restarts at each send instead of running on a fixed tick, so after an idle period a new message leaves at once where
PS3 waits for the next tick (up to 50 ms). Target: the PS3 rule exactly (scan the in-flight records' message lists for
a mode 0 / 1 event; message node and event layouts in TRANSPORT-INTERFACE 1.1 / 1.3).

### I3. Stale player snapshots expire after 1 s (PS3 `0x467d20`, type-12 expiry `0x474a68`) - COVERED (gamenet.cpp, 2026-11-25; run-time checks to confirm in a match)

PS3 behaviour, every frame, for every connection, before the send decision:

- Walk the unreliable queue in order; every message whose type says "expired" is unlinked (the queue tail is fixed up
  when the last node goes), its event reference dropped (deleted at zero), its relay list and node freed, and the debug
  line `Message type <type> has expired` printed. Messages that are not expired keep their order.
- Only type 12 can expire: when the net clock (1.6) is more than 1.0 s past the snapshot's own time stamp.
- The pack skips expired messages too; when the pass runs right before the pack in the same frame with the same clock,
  that second check finds nothing more, so the pass alone gives the PS3 result.

What the player sees: while a link stalls (packet loss burst, window full so only ack-only packets go out), PS3
throws away movement snapshots older than 1 s; when the link recovers the other players jump to their current state.
Ours keeps every queued snapshot and sends the backlog afterwards: remote players replay seconds of old movement and
lag behind until the backlog drains, and the queue grows without bound during a long stall (until the 10 / 20 s
timeout).

Implementation facts for PS4 (to verify at run time, not assumed): the PS4 type-12 event is made by
`CreateNetEventByType 0x6180f0(12)`; its time stamp is the first payload word (PS3 +8 -> PS4 +0xc expected), written
by the fill function (PS4 `0x634970`) from the same net clock the expiry must compare with; the unreliable queue is the
record's message layer +0x120 head / +0x128 tail with 0x28-byte nodes (TRANSPORT-INTERFACE 1.3); free nodes the way
the PS4 reset `0x60ba10` does. Leave every other type alone (they never expire on PS3).

As implemented (2026-11-25). The time stamp is read as the f32 at event `+0xc` (net clock seconds, the encoding the
other snapshot types use, `events.md` 4.1 `Time`); the net clock is the game clock `0x1116180+0x204` / 300, which
session.cpp also writes into the net clock object. The expired nodes are handed to the PS4 packet-lost method
(`0x60b8f0`) as the message list of a scratch packet record: for mode 2 it releases event reference, relay list and
node, which is the reset's way. Two run-time checks guard what static reading could not settle; each logs its
outcome once:

- Record message list layout (event at node +0, next at +0x20, as the in-flight scan of I2 also needs): checked on the
  first packed record that holds messages (`packet record message list layout confirmed` / `layout differs`). Until
  confirmed, I2 counts any in-flight packet as pending (the old rule) and I3 does not run; if it differs, both stay so.
- Time stamp: expiry starts only after 8 queued type-12 events carried a stamp within 0.5 s of the net clock
  (`snapshot expiry on`); a stamp outside [now - 60 s, now + 0.1 s] or not a number turns it off (`snapshot expiry
  off: ... no net clock time`). The same check logs whether the type-12 vtable's expiry slot (+0x30) is the shared
  `0x619da0` (question 4).

Only mode-2 type-12 messages are removed (a type-12 event with another mode would be re-queued by the lost method,
so it is left alone). The PS3 line is printed once per pass with a count, not once per message (a 20 s stall would
otherwise print hundreds of lines per peer).

### I4. Unreachable peers are not tried again (PS3 `0x4e8978`, fed by `0x4e9618`, used by `0x4e9e98`, `0x4b0a58`, `0x4c41f8`) - MISSING (partly covered)

Behaviour as in 1.4.1. Ours marks *rooms* bad (by room id) when a matchmaking join fails for any reason. Missing:

1. A per-online-session list of up to 32 online ids of peers whose link ended as "unreachable" (oldest overwritten),
   emptied when online multiplayer is entered again (our `online::start` / NetInit point).
2. Connecting to a listed id fails immediately (a join whose owner is listed fails as "owner unreachable" without
   waiting for the 20 s link timeout).
3. Matchmaking skips every room whose **owner** is listed (not only the room that failed): a player whose link was
   unreachable is not matched with us again in this session, whichever room they host.
4. In a party, a member whose link to the party owner is not up leaves the party room at once when the owner is
   listed (otherwise after 5 s as F11 b).

Source on PC: the online program's peer state "dead". If it cannot tell "unreachable" from other ends (peer quit,
our own disconnect), use "dead without ever having been active" as the unreachable case and say so in the log
(question 2).

### I5. Connection state colours in the matchmaking rows (PS3 `0x4e8f1c`, used by `0x4bfac8`) - MISSING (correction to `parity-b-menus.md` 2 / 11)

`parity-b-menus.md` assumed the status function returns only 0 or -1. It returns the signaling state (1.4), and the row
code maps all four outcomes, so PS3 shows four colour words for the bars image (type 0x1d) of a player row:

| Peer state (our `u2_online_abi.h`) | PS3 state | Colour word as PS3 writes it |
|---|---|---|
| inactive | 0 | `0x800000FF` |
| pending | 1 | `0x80FF0000` |
| active | 2 | `0x8000FF00` |
| dead, unknown peer, own row, no record | -1 | `0x80808080` |

Ours writes `0x800000FF` for an active peer (PS3's *inactive* word) and grey otherwise, so an established player's
bars have the wrong colour and pending ones look like dead ones. Fix in `session.cpp mm_roster_query` with the peer
state from `online.cpp` (a getter next to `peer_rtt`). The byte order of the word on PS4 is the open point of
`parity-b-menus.md` 2 (same for all four values). LAN rows: no peer state -> as today.

### I6. Silent-peer timeout (PS3 `0x4651b4`) - MISSING, specified in `parity-e-flow2.md` 2.2

20 s without any datagram (ours 10 s), never for a connection that has received nothing yet; one log line
`*************** <name> has left : timed out`. Not repeated here.

## 4. PLATFORM items and what replaces them

- Sockets (`0x4db1cc`, `0x4db1d0`, `0x4db600`, `0x4db630`, `0x4db7d4`, `0x4dba48`, `0x4dbc98`): net.cpp's UDP socket
  (LAN) and `net::Link` over the online program (online). No simulator, no bandwidth history: retail PS3 had the
  simulator off. Delta, not visible in normal play: PS3's P2P socket encrypted and signed every datagram, so forged or
  damaged datagrams never reached the game; ours relies on the online link online and on the magic / version check on
  LAN (TRANSPORT-INTERFACE 4 warns that a malformed body can crash the PS4 unpack; `gamenet.cpp` checks size before
  unpacking). Question 3.
- Connection manager (`0x4e8a50`, `0x4e8afc`, `0x4e8c8c`, `0x4e9060`, `0x4e92d0`, `0x4e94c8`, `0x4e9618`, `0x4e9d28`,
  `0x4e9e98`, `0x4ea438`): online program peers (signaling over RPCN, one link per peer kept while any of our rooms
  needs it = the reference count), `online.cpp` peer events, `gamenet.cpp attach/detach` (record keys instead of the
  `0x1234` counter), `net.cpp find` (peer by address). The unreachable part is I4. Signaling log lines are debug only.
- Module statics (`0x465484`, `0x4654c0`, `0x4674ec`, `0x467528`: set a module word to -1; `0x467378`, `0x4673c0`:
  tear down / set up a fixed-key 64-bit block-cipher state of 5120 bytes used by the module's 8-byte-block packet
  pass `0x466ee0`, already PLATFORM in `parity-f-party-mm.md`): no visible effect; our transport has its own design.
- Statistics printer `0x468ed8`: debug output only.
- NP utilities: error-code-to-name table `0x4eae30` (log text only; our online program logs its own errors), NetCtl
  init + system network dialog `0x4ebf7c` and the "signed in" test `0x4ec068` (the bootstrap sign-in prompt of
  `parity-d-flow1.md` 2 step 3; on PC the launcher / online program own the account), NP init / term `0x4ec184` /
  `0x4ec0f0` (two 128 KB pools; the online program).

## 5. Test scenarios

1. I1: block one client's acks (drop its outgoing datagrams in a test link) during a match: the host's record reaches
   31 packets in flight, the 32nd datagram is header-only; with the lag limit at 30 the PS4 lag state is entered.
2. I2: lobby with one silent client: the client's packets to the host come every 2 s once its own reliable messages are
   acked, not at 20 Hz; a message queued right after a tick leaves on the next 50 ms tick.
3. I3: in a match, stall a client's link for 3 s, then restore: the other PCs show the client at its current position
   at once (no replay of 3 s of movement); the client's log shows `Message type 12 has expired` lines during the stall;
   the unreliable queue length stays bounded (about 1 s of snapshots).
4. I4: make the owner of a matchmaking room unreachable (block its UDP): the join fails, the owner's id is listed; the
   same player then hosts a new room: our next search skips it; re-entering online multiplayer clears the list.
5. I5: matchmaking rows: an established player's bars use `0x8000FF00`, a player still connecting `0x80FF0000`, an
   inactive link `0x800000FF`, a dropped one grey.
6. Regression: two PCs on LAN and online, full match: no change in reliable delivery (all reliable events applied in
   order, none twice), packet size never over 1200 bytes of messages.

## 6. Open questions

1. The lag limit the PS4 TaskManagerOnline update passes (PS3 netInfo `+0x50`, PS4 `0x104f1c8+0x58` per
   TRANSPORT-INTERFACE 1.4) and what the update does in each branch were not traced in this group (row `0x442558`
   belongs to group E). I1 only restores the reachable state.
2. Can the online program report *why* a peer went dead (unreachable vs terminated by the peer vs timeout)? If not,
   I4 uses "dead before ever active" as the unreachable case.
3. Integrity on LAN: PS3 signed and encrypted its P2P datagrams. Should the LAN transport get a per-session message
   authentication tag so a stray or forged datagram cannot reach the PS4 unpack? (Not a visible PS3 behaviour; owner
   decision.)
4. I3: confirm on PS4 that the type-12 vtable's expiry slot is the shared "never" method and that the time stamp is the
   first payload word; if the PS4 slot already expires snapshots, only the per-frame pass is missing. gamenet.cpp now
   answers both in the log of the first match (`snapshot expiry: type 12 uses ...`, `snapshot expiry on` / `off`).
5. I5: the colour byte order on PS4 (same open point as `parity-b-menus.md` 2).

## 7. Status table

| PS3 | Role (what it does) | Status | Where (our code / PS4) / item |
|---|---|---|---|
| 0x464cb8 | packet header read: sequence / ack acceptance, ack field, delivered / lost per record | COVERED | gamenet.cpp `receive_peer` (same rules; 1.2) |
| 0x465138 | packet header write (has-messages bit, 11-bit seq, 10-bit ack, 32-bit field) | COVERED | gamenet.cpp `send_peer` |
| 0x4651b4 | "silent for more than T s" test (T = 20 s at its caller) | MISSING | I6 = parity-e 2.2 (net.cpp 10 s today) |
| 0x465218 | read packet: stamp last receive, header, unpack if new with messages | COVERED | net.cpp `last_heard` + gamenet.cpp `receive_peer` (stamps first, PS3 order) |
| 0x4652cc | write packet: window (ack-only above 30 in flight), new record, pack | COVERED | I1: gamenet.cpp `send_peer` + `gamenet_rules.h window_full` |
| 0x465484 | module static: a word set to -1 | PLATFORM | no visible effect |
| 0x4654c0 | static-init caller of 0x465484 | PLATFORM | no visible effect |
| 0x467378 | module static teardown: clear the block-cipher state | PLATFORM | 4 (transport own design) |
| 0x4673c0 | module static set-up: key the block-cipher state | PLATFORM | 4 (transport own design) |
| 0x467438 | "something to send": queues non-empty or in-flight mode 0/1 message | COVERED | I2: gamenet.cpp `send_peer` + `gamenet_rules.h` tick / send / inflight_needs_ack |
| 0x4674e0 | set the per-message receive hook + context | COVERED | gamenet.cpp `attach` installs the PS4 no-op hook (PS3 hook is debug-gated) |
| 0x4674ec | module static: a word set to -1 | PLATFORM | no visible effect |
| 0x467528 | static-init caller of 0x4674ec | PLATFORM | no visible effect |
| 0x467538 | debug dump of parked out-of-order reliable sequences ("Wait Seq") | COVERED | PS4 `0x60bcc0` (slot 5); debug only |
| 0x467600 | pack: unreliable then reliable (64 window), relay lists, 1200-byte budget | COVERED | PS4 `0x60ac70` via gamenet.cpp `send_peer` |
| 0x467ae8 | message layer constructor (empty queues, counters) | COVERED | PS4 static constructor `0x675570` |
| 0x467b5c | allocate a packet record | COVERED | gamenet.cpp own 0x18-byte record |
| 0x467ba0 | queue an event (reliable gets a sequence, others unsequenced; relay list) | COVERED | PS4 `0x609fc0` and inline in `0x644ab0` |
| 0x467d20 | per-frame removal of expired unreliable messages ("has expired") | COVERED | I3: gamenet.cpp `expire_peer` + `gamenet_rules.h unlink_expired` (run-time checks, I3) |
| 0x467e88 | unpack: factory, read, relay, hook, apply; in-order reliable delivery | COVERED | PS4 `0x60a110` via gamenet.cpp `receive_peer` |
| 0x468658 | reset: release all queued / parked messages, counters back | COVERED | PS4 `0x60ba10` (gamenet.cpp `attach` / `detach`) |
| 0x4688a8 | packet lost: mode 0 back in order, mode 1 to the front, mode 2 released | COVERED | PS4 `0x60b8f0` |
| 0x468a18 | packet delivered: release, advance the contiguous reliable ack | COVERED | PS4 `0x60b780` |
| 0x468c10 | event base "expired?" = never (79 of 80 types use it) | COVERED | PS4 shared method; type 12 override: gamenet_rules.h `expired` (I3) |
| 0x468c18 | StartConnection (type 0) Read: empty | COVERED | PS4 `0x63c100` (same empty body) |
| 0x468c1c | StartConnection (type 0) Write: empty | COVERED | PS4 `0x63c0f0` |
| 0x468d70 | copy an 80-byte block (BreakableCollapse sender helper) | COVERED | PS4 `0x614010` (inline) |
| 0x468dc0 | EarnMedalClient (type 60) Read: empty | COVERED | PS4 `0x61d270` |
| 0x468dc4 | EarnMedalClient (type 60) Write: empty | COVERED | PS4 `0x61d260` |
| 0x468dc8 | EarnMedalClient (type 60) Apply: empty | COVERED | PS4 `0x61d250` |
| 0x468dcc | CompleteTask (type 65) Read: empty | COVERED | PS4 `0x62d900` |
| 0x468dd0 | CompleteTask (type 65) Write: empty | COVERED | PS4 stub (same empty body) |
| 0x468dd4 | per-type statistics: count, bits, max, min | COVERED | inline in PS4 `0x60ac70` |
| 0x468e34 | event base deleting destructor | COVERED | PS4 event base |
| 0x468e70 | event base destructor | COVERED | PS4 event base |
| 0x468e88 | NetEventMessage (type 46) destructor: free the payload buffer | COVERED | PS4 type 46 intact |
| 0x468ed8 | print the per-type statistics table | PLATFORM | debug output only |
| 0x4db1cc | reset socket byte / packet statistics | PLATFORM | net.cpp (no statistics) |
| 0x4db1d0 | sendto with loss / delay / duplicate simulator (off in retail), byte counters | PLATFORM | net.cpp `send_raw` / `net::Link` |
| 0x4db600 | close a socket | PLATFORM | net.cpp `close` |
| 0x4db630 | plain UDP socket: non-blocking, bind | PLATFORM | net.cpp `open` |
| 0x4db7d4 | P2P UDP socket: encryption, signature, non-blocking, bind port 3658 / vport 1 | PLATFORM | online program link; integrity delta 4, question 3 |
| 0x4dba48 | recvfrom (P2P port fixed, receive time stamped, byte counters) | PLATFORM | net.cpp `pump` |
| 0x4dbc98 | per-frame socket update: due delayed sends, per-second bandwidth history | PLATFORM | none needed (simulator off) |
| 0x4e8978 | is an online id on the unreachable list (32 slots) | MISSING | I4 (ours: bad rooms by room id) |
| 0x4e8a50 | clear one connection record | PLATFORM | gamenet.cpp `detach` |
| 0x4e8afc | connection manager shutdown: reset all, end signaling, destroy context | PLATFORM | online program / `gamenet.cpp detach` |
| 0x4e8c8c | release a record (reference count; at 0 reset + end signaling) | PLATFORM | online program keeps a peer link while any room needs it |
| 0x4e8e90 | connection quality 1..5 from round-trip ms | COVERED | session.cpp `bars_of` + online.cpp `peer_rtt` |
| 0x4e8f1c | signaling state of a record (0 / 1 / 2, -1) | MISSING | I5 (four colours; ours two) |
| 0x4e9060 | activate signaling to a record's peer | PLATFORM | online program `peer_connect` |
| 0x4e92c4 | constant "no" answer used by the net page update | COVERED | always false on PS3 too; no behaviour |
| 0x4e92d0 | find a record by peer address and port | PLATFORM | net.cpp `find` |
| 0x4e94c8 | peer address and port of a record | PLATFORM | net.cpp `peer_address` |
| 0x4e954c | "every live record is over the lag limit" | COVERED | PS4 `0x5f14d0` inline + gamenet.cpp counters (reachability: I1) |
| 0x4e9618 | signaling callback: dead / established / net-info events | PLATFORM | online.cpp peer events; unreachable feed is I4 |
| 0x4e9d28 | connection manager init: own id, signaling context, 32 records, empty unreachable list | PLATFORM | online program open; list reset is I4 |
| 0x4e9e98 | connect by online id (unreachable refusal, shared record, new record) | PLATFORM | online.cpp `connect_member`; refusal part I4 |
| 0x4ea438 | extended signaling events: peer activated / deactivated / mutual | PLATFORM | online program accepts incoming peers; online.cpp peer events |
| 0x4eae30 | NP error code to name (log text) | PLATFORM | online program's own error logs |
| 0x4ebf7c | NetCtl init + system network dialog | PLATFORM | parity-d bootstrap sign-in prompt; launcher / online program |
| 0x4ec068 | "NP status is online" test | PLATFORM | online link state (online.cpp) |
| 0x4ec0f0 | NP / NP2 shutdown | PLATFORM | online program |
| 0x4ec120 | pick the NP communication id (retail NPWR00462, beta NPWR00795) | COVERED | online.cpp `kCommunicationId` "NPWR00462_00" |
| 0x4ec184 | NP / NP2 init with two 128 KB pools | PLATFORM | online program |

Counts: COVERED 33, PLATFORM 29, MISSING 3 (I4 `0x4e8978`, I5 `0x4e8f1c`, I6 `0x4651b4` = parity-e 2.2). I1
`0x4652cc`, I2 `0x467438` and I3 `0x467d20` were done on 2026-11-25.
