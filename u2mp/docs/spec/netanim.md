# Spec: netanim (netanim.cpp, natives.cpp)

Clean-room behaviour description of two files in `gow3_pc/src/u2mp/`. It states what the code must do, as seen
from the game image, the log and the install counter. It does not prescribe code structure, helper names or loop
shapes. All addresses are image offsets of Uncharted 2 PS4 1.02 (`image.base + offset`). "u32 at X+o" means a
4-byte little-endian read at that address; "pointer at X+o" means an 8-byte read. Reads may be unaligned.

Shared facilities from `u2mp_internal.h` (already provided by the project):

- `image.matches(offset, bytes, length)`: true only when the range lies inside the image and the bytes are equal.
- `near_stub(bytes)`: executable memory within rel32 range of the image; NULL when none is left.
- `replace_function(offset, length, host, name)`: writes a jump at `offset` to a U2MP_GUEST host function and
  logs on its own. Returns true on success.
- `is_ret_stub(offset)`: the 5 bytes at `offset` are `c3 66 66 66 66`.
- `log(format, ...)`: one line, printf style. The project prefixes `u2mp: `; the strings below are the format
  strings passed to `log`, without that prefix.
- `U2MP_GUEST`: System V x86-64 calling convention. Every guest function called and every function the guest
  calls uses it.

Floating point: all float arithmetic below is IEEE single precision with each operation rounded on its own
(no fused multiply-add, no double intermediates, no fast-math). The build has no `-march`, so SSE2 scalar
float is the reference.

Parts 1 and 2 describe the clean-room version (4c88d7a, 2026-11-14). Part 3 lists what the current code does
differently (first in the squash 934a6ca, 2026-11-24, unchanged since): it replaces 1.4.1 step 2-3, 1.4.2 and 1.5
for the current code, adds four call-outs, and adds three log lines to Part 2. Generator slot 4 (1.4.3) is no
longer installed; whether it should be is open (3.10).

---

## Part 1: netanim.cpp

### 1.1 Purpose and hooks

On PS4 1.02 remote characters in multiplayer lose two things:

1. The snapshot stream no longer carries the phase of each animation clip. The writer writes a clip command's
   layer type and anim index only; the reader resolves the anim and counts it, but never reads a phase and never
   calls visitor slot 3. As a result the visitor that builds a remote character's clip command is never reached:
   remote characters get blends but no clip. The fix: a call-out right after the writer's index write appends
   the phase bits, and a call-out in the reader's anim case reads them and calls visitor slot 3. This changes the
   wire format: both PCs must run this build.
2. NetAnimCmdGenerator vtable slot 4 (`0x5f48d0`) is a bare `ret`. The engine anim layer calls it every frame
   (after generator slot 6) with an events list, or NULL. It should fire the frame-keyed events (sounds, effects)
   of the animations a remote character plays between two snapshots. The fix: replace slot 4 with a host
   function that keeps a 16-entry animation list per generator and calls the game's event firing function
   `0x4bcfd0` (present on PS4, but nothing calls it).

The NetAnimList visitor the slot needs does not exist on PS4; the host provides one with the PS4 visitor layout.

**Current state (since `e05a7d9`, PR #110, 2026-11-22): slot 4 is not installed.** That commit replaced the reader
call-out of 1.4.2: the new one (`read_leaf`) reads the phase, resolves the clip through an anim index table (index
call-out `0x66b637`) and calls slot 3 (clip) or slot 5 (bind pose) of the visitor only when that visitor is the pose
generator (vtable `0x100ac80`); it also added a bind-pose prefill (`0x5f3d63`), a tree depth guard (`0x5f3dd1`) and
the bit count padding (`0x60e296` / `0x60e9f3`, `UNCHARTED_U2MP_NETANIM_PAD=0` off). The same commit dropped the
slot 4 checks and `replace_function` (install steps 6 and 7 below). No commit message, comment or crash note gives a
reason, but with the new reader the slot's own visitor would never receive a clip, so the slot would never fire an
event. The slot 4 host function (1.4.3) stays in netanim.cpp, unused. Installing it again needs the reader call-out
to hand the resolved clip to every visitor's slot 3 (as 1.4.2 describes), tested on two PCs. Sections 1.4.2 and
1.5 below still describe the version before `e05a7d9`; the new reader, index, prefill, depth and padding call-outs are
not described here yet.

Install log of the padding (current code): `netanim: snapshot bit counts padded to whole words` when installed;
`netanim: bit count padding off (UNCHARTED_U2MP_NETANIM_PAD=0)` only when the variable is exactly `"0"`;
`netanim: snapshot bit count sites differ from 1.02, no bit count padding` when its bytes differ;
`netanim: no stub memory near the image, no bit count padding` when a stub request fails.

Switch: environment variable `UNCHARTED_U2MP_NETANIM`. When it is exactly `"0"`, nothing in this file is
installed.

### 1.2 Interface

Used by other files (declared in `u2mp_internal.h`, namespace `u2mp`):

- `void install_netanim(int &installed);` called once by `u2mp_install` (install.cpp), after
  `install_visibility` and before `install_netpatch`.

Nothing else is exported. No other source file calls into netanim.cpp (gameplay2.cpp only mentions it in a
comment).

Entry points the game reaches after install:

| Entry | Address | How | Convention / arguments |
|---|---|---|---|
| Writer call-out | `0x6364b5` | 11 bytes replaced by `e9 rel32` + 6 x `cc`, jumping to a stub in `near_stub` memory | register contract, see 1.4.1 |
| Reader call-out | `0x66c035` | 28 bytes replaced by `e9 rel32` + 23 x `cc`, jumping to a stub | register contract, see 1.4.2 |
| Generator slot 4 (not installed since `e05a7d9`, see 1.1) | `0x5f48d0` | `replace_function(0x5f48d0, 16, host, "NetAnimCmdGenerator slot 4 (PS3 0x453c10)")` | `void (const uint8_t *generator /*rdi*/, void *events /*rsi*/)`, no return value |
| Visitor slots 0..6 | host vtable | pointers in a host-owned 7-entry table, passed to the guest inside the visitor object | see 1.4.4 |

### 1.3 Game facts

Functions and code sites:

| Address | Meaning |
|---|---|
| `0x6364b5` | Writer site, reached right after a clip command's index was written (also reached from the index-skip branch at `0x6363fb`). Original: `inc r10d; inc r14d; jmp 0x638000`. At this point r12 = clip command, r15 = bit stream. |
| `0x638000` | Writer resume address (original jump target). |
| `0x6363f6` | Checked: `cmp r11d,[rsp+0x54]; je ...` leading to the writer site. |
| `0x63646c` | Checked: `jae` to the writer site. |
| `0x66c035` | Reader site in the anim case of snapshot reader `0x66b430`, reached only with a non-null resolved anim in rax. Original: store r14 to `[rbp-0x188]`, increment the 32-bit anim counter at `[rbp-0x180]` (64-bit load, `inc eax`, 64-bit store), `jmp 0x66c250`. |
| `0x66c250` | Reader resume address. |
| `0x66bffb` | Checked: `test rax,rax; jne 0x66c035`. |
| `0x66b430` | Snapshot reader: `void (visitor, snapshot, generator+0x28, u32 a, u32 b, u32 word)`; walks one snapshot and calls the visitor's slots. |
| `0x66b270` | Base visitor reset: `void (visitor)`. Zeroes the vector at +0x10, stores a constant vector at +0x20, clears the byte at +0x30. |
| `0x4bcfd0` | Fire animation events: `void (const void *event_table, u32 forward, u32 looping, u32 zero, u32 one, void *events, float from, float to, float duration)`; integer args in esi, edx, ecx, r8d, events in r9, floats in xmm0..xmm2. |
| `0x5f48d0` | NetAnimCmdGenerator vtable (`0x103fab0`) slot 4: `ret` plus padding to `0x5f48e0`. |
| `0x391590` | Anim layer code that loads and calls generator slot 4 (`mov rdi,[rbx+0x40]; mov rax,[rdi]; mov rax,[rax+0x20]`). |

Generator (NetAnimCmdGenerator) fields:

| Offset | Type | Meaning |
|---|---|---|
| +0x24 | u32 | Word passed as the 6th argument of `0x66b430`. |
| +0x28 | (address) | Anim set; its address is the 3rd argument of `0x66b430`. |
| +0x68 | u32 | Snapshot count. |
| +0x70 | array | Snapshots, stride 0x90, newest first. Snapshot +4 = time (float). |
| +0x960 | float | Generator time. |

Anim fields:

| Offset | Type | Meaning |
|---|---|---|
| +0x24 | u32 | Flags; bit 0 = looping. |
| +0x28 | pointer | Anim info. Info +0x22 = frame count (u16); info +0x2c = rate (float). |
| +0x48 | pointer | Event table. Table +0 = id (u32), +4 = event count (u32). |

Clip command (writer side, r12): +4 anim pointer (8 bytes, unaligned), +0x10 frame (float).

Bit stream (both sides): +0 pointer to u32 words, +8 u32 bit position, +0xc u32 bit limit. Bit n lives in
word n/32 at bit (31 - n%32): MSB first in native little-endian u32 words.

Reader frame (rbp of `0x66b430` at the reader site): bit stream at rbp-0x150 (so position at rbp-0x148, limit at
rbp-0x144), visitor pointer at rbp-0x190, anim counter (low 32 bits used) at rbp-0x180, r14 save slot at
rbp-0x188.

PS4 visitor object layout (what `0x66b430` expects and what the host must provide):

| Offset | Content |
|---|---|
| +0 | pointer to the vtable (7 slots, 8 bytes each) |
| +8, +0xc | two u32 written by `0x66b430` |
| +0x10 | 16-byte vector (base reset) |
| +0x20 | 16-byte vector (base reset) |
| +0x30 | abort byte (base reset) |
| +0x38 | host: pointer to the list the visit slot appends to |
| +0x40 | host: u32 "last key" |

The object must be 16-byte aligned. Before the first reader call every byte other than the vtable pointer and
the list pointer is zero.

Constants: 30.0, 0.1, 0.15, 0.85, 1.0, 0.0 (all float). Phase bit widths: 4 bits scale 15 (frames <= 8),
16 bits scale 65535 (frames > 64), 8 bits scale 255 (otherwise).

### 1.4 Behaviour

#### 1.4.1 Writer call-out (`0x6364b5`)

Register contract: the stub skips the 128-byte red zone below rsp, preserves rax, rcx, rdx, rsi, rdi, r8-r11,
rbx and xmm0-xmm15 and the stack pointer, calls the host with a 16-byte aligned stack, then executes
`inc r10d; inc r14d` (32-bit increments, upper halves zeroed, flags set by them) and jumps to `0x638000`.
The other callee-saved registers survive because the host follows System V. The carry flag is not preserved
across the host call (the reference does not save flags); nothing at the resume address depends on it.

Host action with cmd = r12, stream = r15:

1. anim = pointer at cmd+4. info = pointer at anim+0x28 when anim is non-null, else none.
2. With info: frames = u16 at info+0x22; choose (n, k) from the widths above. q = truncate-to-int32 of
   `(frame * k) / (float(frames) - 1.0f)`, frame = float at cmd+0x10, computed in float, multiply first.
   Conversion: NaN gives INT32_MIN; a value >= 2^31 gives INT32_MAX; a value <= -2^31 gives INT32_MIN; else
   truncation toward zero. (frames = 1 divides by zero; frames = 0 divides by -1: follow these rules.)
3. Without info: n = 8, q = 0.
4. Room check: if position + n > limit (computed without 32-bit wrap), print
   `netanim: snapshot stream full, clip phase not written` and change nothing. This message and the reader's
   overflow message share one counter: together at most 8 lines per process; later overflows are silent.
5. Otherwise write the low n bits of q at the position, MSB first, with this exact word behaviour:
   - Let off = position % 32, word = words[position / 32].
   - If off != 0: the bits of that word from bit index off to the end of the word are cleared, then the field
     bits are ORed in. If off == 0: nothing in the word is cleared; the field is ORed into the existing word
     (existing bits stay set). This odd case is required.
   - If the field does not fit in the rest of that word (32 - off < n), the next word is overwritten entirely:
     it holds the remaining low bits of the field at its top and zeros below.
   - position += n.

#### 1.4.2 Reader call-out (`0x66c035`)

Register contract: first store r14 to the 8 bytes at rbp-0x188 (the replaced store), then skip the red zone,
save the same registers as the writer stub, call the host with (rdi = the value rax had on entry = anim,
rsi = rbp), restore, then do the counter increment: load the 8 bytes at rbp-0x180, increment the low 32 bits
(the upper 32 bits become zero), store the 8 bytes back, leaving that value in rax, and jump to `0x66c250`.

Host action with anim and frame = rbp:

1. Stream at frame-0x150. visitor = pointer at frame-0x190. counter = u32 at frame-0x180 (before the increment).
2. (n, k) = (8, 255) unless anim is non-null and its info pointer (anim+0x28) is non-null; then chosen from
   the info's u16 frame count at +0x22.
3. Room check: if position + n > limit (no wrap), print `netanim: snapshot stream ends before a clip phase`
   (shared 8-line cap, see writer) and return: no position change, no visitor call.
4. Read n bits MSB first at the position (reading the next word only when the field crosses a word boundary);
   position += n.
5. If anim or visitor is null, return (the bits stay consumed).
6. phase = float(value) * (1.0f / k), where the reciprocal is a float constant (1/15, 1/255 or 1/65535 rounded
   to float) and the product is a float multiply, not a division.
7. Call the visitor's slot 3 (the function pointer at offset 0x18 of the table the visitor's first 8 bytes
   point to) with (visitor, anim, counter as u32, phase as float in xmm0).

#### 1.4.3 Generator slot 4 (`0x5f48d0`): (generator, events)

State: one list per generator, keyed by the generator's address, created empty on first use and kept for the
whole process (never removed). Lookup and creation are thread-safe (the layer may run on job threads). A list
holds up to 16 entries {key u32, anim pointer, phase float, dir byte (1 forward, 0 backward), mark byte} and a
count. Appending to a full list (16 entries) does nothing. "Clearing" a list sets the count to 0 and zeroes all
entries.

Steps:

1. Call log: for the first 3 calls of this function per process (any generator), print
   `netanim: generator slot called (generator %p)` with the generator pointer.
2. Get the generator's list L. If the snapshot count (u32 at gen+0x68) is below 3, clear L.
3. If the count is 2 or less: no reads, no guest calls; finish with step 8 (keep-or-clear, which on an empty L
   changes nothing).
4. Snapshot pair. time = float at gen+0x960. Scan i = 0 .. count-2, with s0 = snapshot i, s1 = snapshot i+1
   (t0, t1 = their floats at +4):
   - if t1 == 0.0: skip this i (t0 is not read);
   - else if t0 < time: pick i, frac = 0.0; stop;
   - else if t1 < time: pick i, frac = (t0 - time) / (t0 - t1); stop.
   If nothing is picked: i = 0, frac = 0.0. (No bound check on the count against the snapshot array.)
5. Read both snapshots with one host visitor object (layout 1.3, zero-initialised, the host vtable):
   - list pointer = "newer" (an empty temporary list); call `0x66b430(visitor, s0, gen+0x28, 0, 0, w)`,
     w = u32 at gen+0x24;
   - list pointer = "older" (another empty temporary list); call `0x66b430(visitor, s1, gen+0x28, 1, 1, w)`
     with the same visitor object (its "last key" and other fields are whatever the first call left).
   - dt = ((1.0f - frac) * t0 + t1 * frac) - t1, float, each product and sum rounded separately, t0/t1 of the
     picked pair (read after the two calls).
6. If events is NULL: return now (L is not touched; no keep-or-clear).
7. Let n = older.count. "Keys equal" for lists X, Y over n entries means X[i].key == Y[i].key for every i < n.
   - Case A: L.count == n and L's keys equal older's keys:
     - n == 0: keep-or-clear L;
     - else if newer.count == n and newer's keys equal older's: "advance same" then keep-or-clear;
     - else: "advance main" then keep-or-clear.
     Return.
   - Case B: else if newer.count == n and newer's keys equal older's keys: "rebuild" then keep-or-clear. Return.
   - Case C: otherwise keep-or-clear L.
8. Keep-or-clear: L is kept when at least one entry i < L.count is unfinished: dir != 0 and phase < 1.0, or
   dir == 0 and phase > 0.0 (NaN counts as finished). Otherwise L is cleared.

Helper rules used by the cases (a = older[i], b = newer[i], e = L[i]):

- min(x, y) = NaN if either is NaN, else x when x < y, else y. max(x, y) = NaN if either is NaN, else x when
  x > y, else y. select(c, x, y) = x when c >= 0.0, else y (NaN c gives y).
- Anim data of a.anim (read from guest memory each time, no null checks): event table = pointer at anim+0x48,
  looping = (u32 at anim+0x24) & 1, frames1 = float(u16 at info+0x22) - 1.0f, duration =
  frames1 * (rate * 30.0f) with rate = float at info+0x2c (the inner product first).
- dt30 = dt * 30.0f.
- Advance forward / backward: v = frames1 * a.phase + dt30 (forward) or frames1 * a.phase - dt30 (backward);
  c = max(min(v, frames1), 0.0); new = max(min(c / frames1, 1.0), 0.0).
- Fire (forward or backward, from, to): call `0x4bcfd0(event table, forward ? 1 : 0, looping, 0, 1, events,
  duration * from, duration * to, duration)`.
- Forward step: np = advance forward; old = e.phase; e.phase = select(old - np, old, np); if np > old and not
  suppressed, fire forward (old, np).
- Backward step: np = advance backward; old = e.phase; e.phase = select(old - np, np, old); if np < old and not
  suppressed, fire backward (old, np).

Rebuild: clear L, then for each i < n in order (a.anim is non-null here; looping read from a.anim):
- looping anim: forward when a.phase - b.phase > 0.1 or a.phase < b.phase; else backward when a.phase > b.phase;
  else none.
- non-looping anim: forward when a.phase < b.phase; backward when a.phase > b.phase; else none.
- forward: append {key a.key, anim NULL, phase select(a.phase - 0.15, a.phase, 0.0), dir 1}.
- backward: append {key a.key, anim NULL, phase select(0.85 - a.phase, a.phase, 1.0), dir 0}.
- none: append nothing (so L may end shorter than n and the next frame goes to case B again).

Advance main (older and L have the same keys; newer differs): for each i < n in order, suppressed = some j < i
has older[j].key == a.key. If e.dir != 0 do the forward step, else the backward step.

Advance same (older, newer and L all have the same keys): for each i < n in order, suppressed = some j < i has
older[j].key == a.key and |older[j].phase - a.phase| < 0.1 (float). Then:
1. e.dir != 0 and b.phase > a.phase: forward step; next i.
2. Non-looping and e.dir != 0: nothing; next i.
3. Looping and e.dir != 0: if a.phase - b.phase > 0.1, set e.phase = 0.0; next i.
4. (e.dir == 0) If a.phase > b.phase: backward step; next i.
5. Else, if looping and b.phase - a.phase > 0.1: set e.phase = 1.0.

The anim data is read for every entry processed by the two advance passes, whether or not anything fires.
Fire calls happen in entry order.

#### 1.4.4 Host visitor slots

All take the visitor as first argument and return nothing.

- Slot 0 (reset): call `0x66b270(visitor)`, then set the visitor's last key (+0x40) to 0.
- Slots 1, 2, 5, 6: do nothing.
- Slot 3 (visit; visitor, anim, u32 counter, float phase): set last key to 0. Stop if anim is null, if the event
  table pointer (anim+0x48) is null, if the table's count (u32 at +4) is 0, or if the info pointer (anim+0x28)
  is null. Otherwise last key = u32 at table+0, and append {key, anim, phase, dir 1, mark 0} to the list at
  visitor+0x38 (ignored when that list is full).
- Slot 4 (mark; visitor, u32 ignored): if last key is 0, do nothing. Else find the highest-index entry of the
  visitor's list with that key and set its mark byte to 1. (Marks are never read by the slot 4 update.)

### 1.5 Install (`install_netanim`)

In this order:

1. If `UNCHARTED_U2MP_NETANIM` is exactly `"0"`: log `netanim: off (UNCHARTED_U2MP_NETANIM=0)`; return.
2. Stream checks (all must match; no log per check):

   | Address | Bytes |
   |---|---|
   | `0x6364b5` | `41 ff c2 41 ff c6 e9 40 1b 00 00` |
   | `0x6363f6` | `44 3b 5c 24 54 0f 84 b4` |
   | `0x63646c` | `73 47` |
   | `0x66c035` | `4c 89 b5 78 fe ff ff 48 8b 85 80 fe ff ff ff c0 48 89 85 80 fe ff ff e9 ff 01 00 00` |
   | `0x66bffb` | `48 85 c0 75 35` |
   | `0x66b430` | `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 88 01 00 00` |

   On any mismatch: log `netanim: snapshot writer / reader bytes differ from 1.02, left alone (no clip phases)`;
   return (nothing patched, nothing counted).
3. Request two stubs with `near_stub(512)`: first the writer's, then the reader's. Both requests are made even
   when the first returns NULL. If either is NULL: log `netanim: no stub memory near the image`; return.
   (Stub memory is outside the image; its exact encoding is free, but the request sizes and order are fixed,
   because the jump displacements written into the image depend on them.)
4. Patch the writer site: the displacement is stub - (image + 0x6364b5 + 5); if it does not fit in a signed
   32-bit value, log `netanim: stub out of jump range` and return. Else write `e9 rel32` and fill the remaining
   6 bytes with `cc`. Then the same for the reader site (displacement from `0x66c035 + 5`, 23 bytes of `cc`). If
   the reader's displacement does not fit, the writer site stays patched, the same log line is printed, return.
5. `installed += 2`; log
   `netanim: snapshot clip phases restored (writer 0x6364b5, reader 0x66c035; both PCs need this build)`.
6. (Steps 6 and 7 are the version before `e05a7d9`; the current code skips both, see 1.1.) Slot checks (all must
   match):

   | Address | Bytes |
   |---|---|
   | `0x5f48d0` | `c3 66 66 66 66 66 66 2e 0f 1f 84 00 00 00 00 00` (16 bytes; implies `is_ret_stub`) |
   | `0x391590` | `48 8b 7b 40 48 8b 07 48 8b 40 20` |
   | `0x4bcfd0` | `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 4d 89 ce` |
   | `0x66b270` | `c5 f8 57 c0 c5 f8 29 47 10 c5 f8 28 05 bf a1 7c 00 c5 f8 29 47 20 c6 47 30 00 c3` |

   On any mismatch: log `netanim: generator slot 4 / helpers differ from 1.02, slot left as it is`; return.
7. `replace_function(0x5f48d0, 16, slot 4 host, "NetAnimCmdGenerator slot 4 (PS3 0x453c10)")`; when it returns
   true, `installed += 1`.

Fully installed: +3.

### 1.6 Test scenarios (netanim)

Install:
- N1 env `UNCHARTED_U2MP_NETANIM=0` (and `=00`, `=1`, unset: only exact `"0"` turns it off).
- N2 each of the six stream checks broken in turn; N3 all good but each of the four slot checks broken in turn.
- N4 `near_stub` returning NULL for the first, the second, and both requests (count requests made).
- N5 everything good: compare the image bytes at both sites and the slot, the counter (+3) and the log.

Writer (drive the stub with r12/r15 set, or the installed site through a register thunk; check r10/r14 + 1 and
the resume address):
- W1 frames 8, 9, 64, 65 (widths 4, 8, 8, 16); frame values 0, mid, frames-1, above frames-1, negative, NaN,
  huge (saturation); frames 0 and 1.
- W2 anim NULL and info NULL (8 zero bits).
- W3 position at word offsets 0, 1, 28, 31 (crossing words; the OR-without-clear at offset 0 with a pre-set
  word; the next word fully overwritten when crossing).
- W4 position + n == limit (written) and == limit + 1 (log). Ten overflows across writer and reader: only 8 lines.

Reader:
- R1 the same widths and offsets, words prepared with known bits; phase float bits compared exactly.
- R2 visitor NULL, anim NULL: bits consumed, no slot 3 call.
- R3 overflow: no position change, no call, shared cap.
- R4 the stub's r14 store to rbp-0x188 and the 64-bit counter slot with non-zero upper half (cleared after).

Slot 4 (fake `0x66b430` that calls slots 0/3/4 of the passed visitor with scripted anims and phases; record the
visitor's bytes except the two host pointers, and every `0x4bcfd0` call with its float arguments):
- S1 count 0, 1, 2 (no reader calls, list cleared); count drops below 3 after a populated list.
- S2 pair search: first pair with t0 < time; t1 < time with fractional frac; t1 == 0 entries skipped; none found.
- S3 events NULL with count >= 3 (reader calls happen, list untouched).
- S4 case B rebuild: looping and non-looping, phases forward / backward / equal, wrap > 0.1, phases near 0.15
  and 0.85 thresholds, NaN phase.
- S5 case A advance main (newer keys differ): forward and backward entries, duplicate keys (suppressed fire),
  clamping at 0 and frames1, frames 1 (frames1 = 0: NaN path).
- S6 case A advance same: all five branches, the |delta| < 0.1 suppression, looping wrap resets to 0.0 / 1.0.
- S7 keep-or-clear: all finished (clear) vs one unfinished (keep); n == 0 case A.
- S8 case C (keys differ everywhere). More than 16 visits (list full). Slot 4 mark with and without last key.
- S9 two generators keep separate lists. The first 3 calls log, the 4th does not.

---

## Part 2: natives.cpp

### 2.1 Purpose and hooks

PS4 1.02 stubbed seven multiplayer / co-op DC script natives (six return 0, `net-coop-task` only looks up
`*net-games*` and returns 0). Co-op objective scripts and NPC fights depend on them. Each native is replaced
whole (first 5 bytes, `replace_function`) by a host function giving the multiplayer behaviour, using game state
that survives in the PS4 image.

### 2.2 Interface

Used by other files: `void install_natives(int &installed);` (declared in `u2mp_internal.h`, namespace `u2mp`),
called by `u2mp_install` after `install_gameplay3` and before `install_progression`. Nothing else is exported.

DC native convention (PS4): `uint64_t native(int argc /*edi*/, const uint64_t *argv /*rsi*/)`. Each argv slot
is 8 bytes; a native uses the low 32 bits of a slot as an int, string id or net id. The result is returned in
rax; every native here returns a u32 zero-extended to 64 bits, or 0.

| Native | Address | Expected stub bytes | Replaced length |
|---|---|---|---|
| net-coop-host-script-id | `0x669c40` | `31 c0 c3 66 66` | 5 |
| net-coop-script-id | `0x669c30` | `31 c0 c3 66 66` | 5 |
| net-player-coop-id | `0x669240` | `31 c0 c3 66 66` | 5 |
| net-get-capture-zone | `0x6687d0` | `31 c0 c3 66 66` | 5 |
| net-apply-npc-fight-damage | `0x668db0` | `31 c0 c3 66 66` | 5 |
| net-get-player-by-net-id | `0x6690b0` | `31 c0 c3 66 66` | 5 |
| net-coop-task | `0x6697e0` | `55 48 89 e5 bf 7d 06 9a 3b e8 82 c6 3a 00 31 c0 5d c3` (18 bytes) | 5 |

The `replace_function` name argument is the native name exactly as in the first column.

### 2.3 Game facts

Globals and functions:

| Address | Meaning |
|---|---|
| `0x14ce97c` | i32 net state; multiplayer when > 1 (signed). |
| `0x1065b08` | Match session object. +0x10: u64 context; +0xb00: u32 my id; +0xb04: u32 host id. Host when the context is 0 or my id == host id. |
| `0x113b760` | NetGameManager object itself (the image address is the object, not a pointer to it). |
| `0x1116180` | Clock object; u32 frame counter at +0x204 (image `0x1116384`). |
| `0x10666c8` | 8-byte function pointer: string -> string id, `u32 (u64 zero, const char *s, int one)`. |
| `0xa15e70` | Symbol lookup `void *(u32 string_id)`. |
| `0x64b960` | Capture-zone spawner by id `void *(NetGameManager *, int32 id)`. |
| `0x60eb90` | Kill-info-client sender `void (u32 kind, void *attacker, void *victim, const u32 *frame, bool, bool, bool)` (7th argument on the stack). |

NetGameManager (ngm = image + `0x113b760`):

| Offset | Type | Meaning |
|---|---|---|
| +0x00 | pointer | Local player tracker. |
| +0x10 + i*0x2c0, i = 0..9 | struct | Player trackers. |
| +0x1ba0 | pointer | NPC tracker map: bucket array (pointers). |
| +0x1bdc | i32 | NPC tracker map: bucket count. |
| +0x1ca0 | i16 | Current net game index. |
| +0x1d6c | i32 | Current net task index. |

Player tracker: +0x10 process handle; +0x120 u32 net id; +0x198 i32 coop index.

Process handle (16 bytes): +0 pointer to a slot holding the process pointer; +8 u32 id. It resolves to the
process when the slot pointer is non-null, the id is non-zero, the process pointer in the slot is non-null and
the u32 at process+0x44 equals the id; otherwise to nothing.

Process: +0x44 u32 unique id; +0x68 u32 name id.

NPC tracker map node: +0 next pointer; +0x10 u32 key (name id); +0x18 the NetNpcTracker value. The end marker is
the pointer stored at buckets[count]. Hash: FNV-1a 32 (offset basis `0x811c9dc5`, prime `0x01000193`) over the
4 bytes of the name id, least significant byte first; bucket = hash mod count (unsigned).

NetNpcTracker: +0x00 NPC process handle; +0x24/+0x28 owner; +0x30 u32 attacker net ids [3]; +0x3c i32 damage [3].

NPC / character: +0x1f0 pointer to health component; +0x262a flag byte, bit 0x02 = killed by a player.
Virtual slots (function pointer at vtable + slot*8, vtable = pointer at object+0): character slot 74 (+0x250)
alive test `bool (obj)`; slot 110 (+0x370) take damage `void (obj, int32)`; health component slot 10 (+0x50)
max health `int32 (health)`.

`*net-games*`: string id `0x3b9a067d`. Lookup result: +0 i32 record count, +8 pointer to records. Record stride
0x90; record +0x0c i32 game type; record +0x48 pointer to the task list. Task list: +0 pointer to the default
task, +8 i32 task count, +0x10 pointer to tasks. Task stride 0x28: +0 u32 name id, +0x1c u32 host script id,
+0x20 u32 script id.

Co-op game types: 4, 8, 9, 12.

Co-op player ids by coop index: 0 -> `0x9ba01002`, 1 -> `0x9ffcfd6c`, 2 -> `0x75d3456f`.

Capture zone: spawner id 0; data key `0x7af5d23b`.

Spawner data: spawner+0x68 pointer to a table {+0 pointer to entries, +8 i32 count}; entries 16 bytes {+0 u32
key, +8 pointer to value}, sorted by key; value {+0 u16 type, +8 pointer to data}; type 13 = string id (u32 at
data+0); type 5 = string (char pointer at data+0, converted by the function at `0x10666c8`).

### 2.4 Shared rules

- Multiplayer test, host test, handle resolution: as in 2.3. The host test is re-read every time it is used.
- Player tracker by net id: the first tracker i = 0..9 whose u32 at +0x120 equals the id (an id of 0 matches an
  empty tracker).
- NPC tracker by name: none when the bucket count <= 0 or the bucket pointer is null. Else start at
  buckets[hash mod count] and follow next pointers until a node whose key equals the name or a null pointer
  (the walk does not stop at the end marker). Not found when the result is null or equals the end marker;
  otherwise the tracker is node+0x18.
- Current net game record: call `0xa15e70(0x3b9a067d)` (exactly once per use). None when it returns null, when
  the i16 index at ngm+0x1ca0 is negative or >= the i32 record count, or when the records pointer is null; else
  records + index*0x90.
- Current task: list = pointer at record+0x48 (none without a record). None when the list is null, when the i32
  index at ngm+0x1d6c is negative or >= the i32 count at list+8, or when the tasks pointer is null; else
  tasks + index*0x28.
- Task id field (offset f): 0 when there is no list. If a current task exists and its u32 at +f is non-zero,
  that value. Otherwise the default task's u32 at +f, or 0 when the default task pointer is null.
- Spawner string id (spawner, key, fallback): fallback when the table pointer, the entries pointer is null or the
  count <= 0. Find the first entry whose key is not below the key by a lower-bound binary search: start with
  range [0, count); probe mid = lo + len/2 (integer division); when entries[mid].key < key (unsigned), lo =
  mid + 1 and len -= len/2 + 1, else len = len/2; until len is 0. Fallback when lo == count or entries[lo].key
  != key, or when the value pointer is null. Type 13: the u32 at data+0, fallback when data is null. Type 5:
  read the converter pointer at `0x10666c8`; when data and the converter are non-null call
  `converter(0, string pointer at data+0, 1)` and return its u32; else fallback. Other types: fallback.
- Log lines: each distinct message (each place it is printed; "called outside of multiplayer" counts once per
  native) is printed at most 8 times per process, then silenced. Formats are given exactly below.

### 2.5 Behaviour per native

Arguments are not read unless stated. argc is not checked unless stated.

net-coop-host-script-id: return the task id field at +0x1c.

net-coop-script-id: return the task id field at +0x20.

net-coop-task: return the u32 name id (+0) of the current task, or 0 when there is none (no default fallback).

net-player-coop-id:
1. Not multiplayer: log `net-player-coop-id - called outside of multiplayer`; return 0.
2. Record (one lookup call); type = i32 at record+0x0c, or 0 without a record. Type not 4, 8, 9 or 12: log
   `net-player-coop-id - called outside of a coop game`; return 0.
3. Local tracker pointer (ngm+0) null: log `net-player-coop-id - no player tracker`; return 0.
4. Coop index (i32 at local+0x198) 0/1/2: return the id from 2.3. Otherwise log
   `net-player-coop-id - invalid spawner index`; return 0.

net-get-capture-zone: call `0x64b960(ngm, 0)`. Null: return 0. Else return the spawner string id for key
`0x7af5d23b` with fallback 0. No multiplayer check.

net-get-player-by-net-id:
1. Not multiplayer: log `net-get-player-by-net-id - called outside of multiplayer`; return 0.
2. argc < 1: return 0 silently.
3. Tracker by net id (low 32 bits of argv[0]); none: log `net-get-player-by-net-id - no player tracker`; return 0.
4. Resolve the tracker's handle (+0x10); nothing: log
   `net-get-player-by-net-id - tracker has no character character` (the doubled word is part of the text);
   return 0.
5. Return the process's u32 name id at +0x68.

net-apply-npc-fight-damage (argv: player net id, npc name id, damage, kind):
1. Not multiplayer: log `net-apply-npc-fight-damage - called outside of multiplayer`; return 0.
2. argc < 4: return 0 silently.
3. Tracker by net id (argv[0] low 32); none: log `net-apply-npc-fight-damage - can't find player %u` (net id);
   return 0.
4. Resolve its handle -> player; nothing: log `net-apply-npc-fight-damage - can't find player process %u`
   (net id); return 0.
5. NPC tracker by name (argv[1] low 32); none: log `net-apply-npc-fight-damage - can't npc tracker 0x%08x`
   (name; the missing word is part of the text); return 0.
6. Resolve the NPC tracker's handle (+0) -> npc; nothing: log `net-apply-npc-fight-damage - no npc 0x%08x`
   (name); return 0.
7. damage = argv[2] low 32 (unsigned), kind = argv[3] low 32.
8. If host: add damage to the NPC tracker for attacker id = the tracker's u32 at +0x120: look at slots 0, 1, 2
   in order; a slot whose id equals it gets damage added to its i32 (32-bit wrap) and stops; an empty slot
   (id 0) reached first gets the id and damage = the amount and stops; with three other attackers nothing
   changes.
9. health = pointer at npc+0x1f0. max = sign-extended i32 from the health component's slot 10 call when health
   is non-null (the call is made for host and client), else 0.
10. If not host and damage (as an unsigned 32-bit value widened to signed 64-bit) < max: return 0. A
    "negative" damage such as `0xffffffff` is therefore large and passes.
11. before = alive test (slot 74) of npc; take damage (slot 110) with (npc, damage as int32); after = alive test
    again. The two results are compared as bools.
12. If before == after: return 0. Else, if not host (host re-read here): return 0.
13. frame = u32 at clock+0x204, read now, into a local u32; call `0x60eb90(kind, player, npc, &frame, false,
    false, false)`; then OR 0x02 into the byte at npc+0x262a. Return 0.

### 2.6 Install (`install_natives`)

1. Layout checks, in this order, stopping at the first failure:
   a. Byte anchors (each must match; on the first mismatch log
      `natives: code at 0x%llx differs, co-op natives left stubbed` with the anchor address and stop, nothing
      installed):

   | # | Address | Bytes |
   |---|---|---|
   | 1 | `0x668b39` | `39 94 30 30 01 00 00` |
   | 2 | `0x6428f6` | `39 8b 88 01 00 00` |
   | 3 | `0x642880` | `8b 44 08 0c` |
   | 4 | `0x60ecaf` | `48 8b 09` |
   | 5 | `0x60ecbb` | `8b 89 20 01 00 00` |
   | 6 | `0x668a6c` | `48 63 8e dc 1b 00 00` |
   | 7 | `0x668a7a` | `48 8b 96 a0 1b 00 00` |
   | 8 | `0x668a98` | `39 78 10` |
   | 9 | `0x668abf` | `48 8d 40 18` |
   | 10 | `0x668acc` | `48 c7 42 24 00 00 00 00` |
   | 11 | `0x668d4c` | `ff 90 50 02 00 00` |
   | 12 | `0x25eb1a` | `ff 90 70 03 00 00` |
   | 13 | `0x61f11a` | `48 8b 98 f0 01 00 00` |
   | 14 | `0x61f12b` | `ff 50 50` |
   | 15 | `0x249ef2` | `41 80 8f 2a 26 00 00 02` |
   | 16 | `0x6696d0` | `44 8b 60 68` |
   | 17 | `0x669795` | `48 0f bf 8a a0 1c 00 00` |
   | 18 | `0x6697a1` | `48 8d 0c c9 48 c1 e1 04` |
   | 19 | `0x6697a9` | `48 8b 4c 08 48` |
   | 20 | `0x6697b5` | `48 63 92 6c 1d 00 00` |
   | 21 | `0x6697c5` | `3b 51 08` |
   | 22 | `0x6697d2` | `48 03 41 10` |
   | 23 | `0x66418f` | `48 8d 48 18` |
   | 24 | `0x669c92` | `48 83 79 10 00` |
   | 25 | `0x669c99` | `48 8b 81 00 0b 00 00` |
   | 26 | `0x60ec4c` | `0f 44 81 04 02 00 00` |
   | 27 | `0x64ba83` | `48 8b 48 68` |
   | 28 | `0x20d7cb` | `83 ff 0d` |
   | 29 | `0x20d7db` | `83 fa 05` |
   | 30 | `0x64b960` | `55 48 89 e5 41 57 41 56` |
   | 31 | `0x60eb90` | `55 48 89 e5 41 57 41 56` |
   | 32 | `0xa15e70` | `55 48 89 e5 41 57 41 56` |

   b. RIP-relative references, in this order: for an instruction at A of length L whose 4-byte signed
      displacement is at A+D, A + L + displacement must equal the target. If A + L exceeds the image size, stop
      silently (no log, nothing installed). On a wrong target log
      `natives: 0x%llx does not reference 0x%llx, co-op natives left stubbed` (A, target) and stop.

   | # | A | L | D | Target |
   |---|---|---|---|---|
   | 1 | `0x669c20` | 7 | 3 | `0x113b760` |
   | 2 | `0x669c50` | 7 | 2 | `0x14ce97c` |
   | 3 | `0x669c89` | 7 | 3 | `0x1065b08` |
   | 4 | `0x60ec45` | 7 | 3 | `0x1116180` |
   | 5 | `0x6698fb` | 7 | 3 | `0x10666c8` |
   | 6 | `0x6697e9` | 5 | 1 | `0xa15e70` |
   | 7 | `0x249eed` | 5 | 1 | `0x60eb90` |

2. Then for each native in the order of the table in 2.2: when its expected stub bytes do not match, log
   `%s at 0x%llx is not the expected stub, left alone` (name, address) and go on with the next. Else call
   `replace_function(address, 5, host, name)` and add 1 to `installed` when it returns true.

Fully installed: +7. No environment switch.

### 2.7 Test scenarios (natives)

Install:
- I1 all good: 7 replacements in table order, +7.
- I2 break anchor 1, a middle one and 32 (log names the first broken one); break two (only the first logs).
- I3 each RIP reference wrong (log); image size smaller than A + L (silent stop).
- I4 one native's stub bytes changed (that native skipped with log, others installed); the coop-task stub's last
  byte changed.

Natives (fake `0xa15e70`, `0x64b960`, `0x60eb90`, converter pointer and virtual slots that record their args):
- C1 host-script-id / script-id: lookup returns null; index out of range (negative, == count); records null;
  task list null; task index out of range; tasks null; task field non-zero; zero with default; default null.
- C2 coop-task: task present / absent (no default fallback).
- C3 player-coop-id: not MP; each game type 0, 4, 8, 9, 12, 5; no record; local tracker null; coop index 0, 1,
  2, 3, -1. Call one path 10 times: 8 log lines.
- C4 capture-zone: spawner null; table / entries null; count 0; key absent below, between, above; key present
  with types 13 (data null / not), 5 (converter null / not; data null), 7; value null; an unsorted table to pin
  the probe sequence; count 1, 2, 3, 7.
- C5 player-by-net-id: not MP; argc 0; no tracker; net id 0 matching an empty tracker; handle slot null, id 0,
  process null, id mismatch; success.
- C6 apply-npc-fight-damage: each early exit (not MP, argc 3, no tracker, no player process, bucket count 0,
  buckets null, chain walk past a mismatching node, node equal to end marker, no npc). Host: new attacker, same
  attacker (sum, wrap), three other attackers; alive unchanged (no send); alive changed (send with frame value,
  flag set, other flag bits kept). Client: damage below max (only the max-health call), damage >= max (damage
  applied, no send even when alive changes), damage `0xffffffff`, max negative, health pointer null (max 0).
  Host status changing between its reads (fake virtual writes the match session).

---

## Part 3: changes after the clean-room version

Origin: the clean-room netanim.cpp / natives.cpp of 4c88d7a (2026-11-14) passed this spec with no difference. Every
change below appears first in the squash 934a6ca (2026-11-24, no message; the U3 repository's history reset) and is
unchanged in 8e7965b, 9fd6134 and the current branch. `git log -S` finds no other commit. The reasons below come
from the comments in the current netanim.cpp. Checked against the PS4 1.02 image (2026-11-25): the reader's rax is
the art group (3.3) and the index count is a 5-bit field (3.4); 3.1 steps 2-3 / 9, 3.3 and 3.4 carry the
corrections of that day.

### 3.1 Install (replaces 1.5 for the current code)

1. Switch: unchanged (1.5 step 1).
2. Stream checks: the six of 1.5 step 2, plus (all must match, same log and return on a mismatch: nothing is
   patched):

   | Address | Bytes | Meaning |
   |---|---|---|
   | `0x66b637` | `4c 8d 35 3c c7 81 00` | index site (3.4) |
   | `0x5f3d63` | `48 8d 0d 16 6f a1 00` | prefill site (3.5), `lea rcx, [image + 0x100ac80]` |
   | `0x5f3dd1` | `41 8b 47 0c 49 8b 0f` | depth site (3.6) |
   | `0x6367cb` | `8d 42 04` | writer constant (step 7) |
   | `0x6368a0` | `83 f9 03` | writer constant (step 7) |
   | `0x6368b7` | `83 c0 04` | writer constant (step 7) |

3. Assert skip, after the checks passed (934a6ca ran it before them, so a non-1.02 image got it too; moved
   2026-11-25): for `at` = `0x66b430` upward while `at + 7 <= 0x66c380` and fewer than 32 sites
   were patched: when the bytes at `at` are `48 8b 05` (mov rax, [rip+d]) and `at + 7 + d` (d = the signed 32-bit
   value at `at + 3`) is `0x156df78` (the assertion hook global), write `e9 rel32` jumping to `0x66c380` (the
   snapshot reader's epilogue; rel32 = `0x66c380 - (at + 5)`) followed by `90 90`, count the site and continue at
   `at + 7`. A displacement that does not fit ends the scan. When at least one site was patched, log
   `netanim: snapshot parse aborts on asserts (%d sites)`. Not counted in `installed`. Purpose (code comment): a
   garbled snapshot ends the parse instead of taking the game down.

4. Five `near_stub(512)` requests, always all five, in this order: writer, reader, index, prefill, depth. Any NULL:
   log `netanim: no stub memory near the image`; return.
5. Build the five stubs, then log `netanim: stubs writer=%p reader=%p index=%p prefill=%p depth=%p` with the five
   stub addresses (host addresses: a diagnostic line, its values differ per process).
6. Jumps, in this order: writer (11 bytes), reader (28), index (7), prefill (7), depth (7), each `e9 rel32` and the
   rest `cc`. A displacement that does not fit logs `netanim: stub out of jump range` and returns (sites patched
   before it stay patched, nothing is counted).
7. Writer constants: the byte at `0x6367cd` becomes `0x0a`, at `0x6368a2` `0x09`, at `0x6368b9` `0x0a` (the
   instructions of step 3 become `lea eax, [rdx+10]`, `cmp ecx, 9`, `add eax, 10`: the game's own writer accounts
   for the 10 phase bits of 3.2).
8. `installed += 5`; log the 1.5 step 5 line (same text), then `netanim: anim indices, bind-pose prefill and tree
   depth guard installed`.
9. Bit count padding: when `UNCHARTED_U2MP_NETANIM_PAD` is not exactly `"0"` and the bytes at `0x60e296` are
   `41 89 84 24 bc 00 00 00` and at `0x60e9f3` are `89 83 bc 00 00 00`: two `near_stub(384)` requests (player, then
   other). With both non-NULL, build both stubs (3.7), jump the player site (8 bytes) and then the other site
   (6 bytes) as in step 6; when both jumps succeed, `installed += 1` and log
   `netanim: snapshot bit counts padded to whole words` (a failed jump is silent; a failed second jump leaves the
   first site patched). A NULL stub logs `netanim: no stub memory near the image, no bit count padding`. The switch
   `"0"` logs `netanim: bit count padding off (UNCHARTED_U2MP_NETANIM_PAD=0)`; a byte check failing (switch not
   `"0"`) logs `netanim: snapshot bit count sites differ from 1.02, no bit count padding` (ab11f85c; 934a6ca gave
   the switch line for both and was silent on a NULL stub).
10. Generator slot 4: the slot checks and the `replace_function` of 1.5 steps 6-7 are gone. The slot host function
    and the host visitor (1.4.3, 1.4.4) are still in the source but nothing installs them. See 3.10.

Fully installed: +6 (+5 without the padding).

### 3.2 Writer call-out (replaces 1.4.1 host steps 2-3)

Register contract and the word rules of 1.4.1 step 5 are unchanged. Host action:

1. Call log: for the first 12 calls of the process, before anything else, log
   `netanim: clip cmd %p size %u anim %p stream %p pos %u limit %u`: the command, the u16 at cmd+2, the anim pointer
   (cmd+4), the stream, and the stream's position and limit as they are before the write.
2. The phase always takes 10 bits, scale 1023.0f. q = 0. With an info pointer: frames = u16 at info+0x22, frame =
   float at cmd+0x10; only when frames > 1 and frame > 0.0f (NaN, zero, negative zero and negative frames give 0):
   q = truncate-to-int32 (1.4.1 rules) of `(frame * 1023.0f) / (float(frames) - 1.0f)`, then q = 1023 when q is
   above 1023.
3. Room check and overflow log as in 1.4.1 step 4, with n = 10. The reader no longer prints its overflow line, so
   the 8-line cap is in practice the writer's alone.
4. Write the low 10 bits of q (1.4.1 step 5).

The phase bit widths of 1.3 (4 / 8 / 16 bits by frame count) are no longer used anywhere.

### 3.3 Reader call-out (replaces 1.4.2)

Stub: the r14 store to rbp-0x188, the save, the host call with (rdi = the 8 bytes at [rbp], i.e. the reader's
frame; rsi = rax on entry, the anim art group; rdx = r14, the anim index + 1), the restore, the counter increment and
the jump to `0x66c250`, as in 1.4.2. rax is the art group: the reader (`0x66bf30`..`0x66bff4`) finds it by id in the
art group table (`0x1241418` +0x38, 350 entries of 0x2c0, each a {key, value} hash at +0x18) and asserts it non-NULL
at `0x66c000` with the expression text `pAnimArtGroup` (`0xe8ab2b`, net-snapshot.cpp, Parse) before `0x66c035`.

"Readable n bytes at p" below: p is at least 0x10000 and below 2^47, and `host_readable(p, n)` (host_memory.h: every
byte of the range is committed and readable; `VirtualQuery` on Windows, a pipe write on POSIX). "Readable" alone is
n = 1.

Host action with frame, group, next:

1. Count the call (process-wide leaf counter).
2. q = 0. When position + 10 <= limit (no wrap): q = the next 10 bits (position += 10). Otherwise nothing: no log,
   no position change.
3. gen = pointer at frame-0x190. Return when gen is NULL or not readable, or when its vtable pointer (gen+0) is not
   image + `0x100ac80` (the CreateAnimCmds visitor of PORT-NETANIM.md 2; any other visitor, e.g. the host visitor
   of 1.4.4, gets no call).
4. slot = u32 at frame-0x180 (the anim counter before the increment), sub = u32 at gen+0xc. Return when sub + slot
   (u32) >= 7.
5. anim = the lookup (below) of (group, stored index next - 1) when 1 <= next <= the stored index count (3.4), else
   none.
6. With an anim: call the vtable's slot 3 (gen+0 -> +0x18) with (gen, anim, slot, float(q) * (1.0f / 1023.0f));
   count a clip. Without: call slot 5 (+0x28) with (gen, slot); count a pose.
7. When the leaf count of step 1 is a multiple of 64, log
   `netanim: leaves %d, clips %d, poses %d, misses %d, deep drops %d, index reads %d` (process-wide counters). The
   returns of steps 3 and 4 skip this line.

Lookup (group, idx): none when group is not readable for 0x38 bytes or idx >= 0x800. An 8-entry per-thread cache of
{group, idx, anim} is checked first. Otherwise: P = pointer at group+0x30; none when P is not readable for 0x18
bytes. tab = pointer at P+0x10; none when tab is not readable for 0x10 bytes or idx >= the u32 at tab+0. entry =
(pointer at tab+8) + idx * 16; none when not readable for 0x10 bytes. want = u32 at entry+0, x = pointer at entry+8.
Candidates (while x is non-NULL): a = x, then (when not yet found) a = pointer at x when x is readable for 8 bytes,
else none. A candidate is the anim when a is readable for 0x30 bytes, the name at its name pointer (a+0), read in
16-byte pieces each readable, ends within 128 bytes, `sid(name)` == want, the clip pointer a+0x28 is readable for
0x24 bytes and the u16 at clip+0x22 is non-zero. Found: stored in the cache (round robin). Not found (after the
table entry was reached): count a miss. (934a6ca followed P and the pointer at x unchecked and read a+0 / a+0x28
before testing a: every pointer is now checked before it is followed, 2026-11-25.)

### 3.4 Index call-out (`0x66b637`, new)

The 7 bytes at `0x66b637` jump to a stub: save, host(rdi = [rbp], esi = r14d), restore, jump to `0x66b6a5`. The
replaced `lea r14, [rip+...]` and the game code up to `0x66b6a5` (its own anim index reads) are skipped.

Host (frame, count): count the call (index reads); for the first 4 calls log `netanim: %u anim indices`. Stream =
frame-0x150. The stored count becomes min(count, 32). For i = 0 .. min(count, 32)-1: the index is the 11 bits at
position + 11 * i when position + 11 * (i + 1) <= limit, else `0xffff`; it is stored at entry i. Then the stream
position becomes position + 11 * count (not limited). Indices and count are per thread. count is the snapshot's
5-bit index count (`0x66b549`: the top 5 bits), so at most 31 and every index is stored; 934a6ca wrote an index past
31 into entry 31 (unreachable, removed 2026-11-25).

### 3.5 Bind-pose prefill (`0x5f3d63`, new)

The 7 bytes at `0x5f3d63` jump to a stub: save, host(rdi = rbx = the command list, esi = r12d = the base slot),
restore, `mov rcx, image + 0x100ac80` (the replaced lea), jump to `0x5f3d6a`.

Host: words = pointer at list+0, count = u32 at list+0xc, top = u32 at list+0x10. For s = base, then base + 1:
words[count] = `0x0002000d`, words[count + 1] = s, count += 2, top = s when top < s. Store count and top back, and
keep {list, count, top} as the per-thread mark. No capacity check.

### 3.6 Tree depth guard (`0x5f3dd1`, new)

The 7 bytes at `0x5f3dd1` jump to a stub: save, host(rdi = r15 = the list, esi = ebx = the base slot), restore,
`mov eax, [r15+0xc]; mov rcx, [r15]` (the replaced instructions), jump to `0x5f3dd8`.

Host: top = u32 at list+0x10; depth = top - base when top >= base, else 0. When depth > 5 and the mark's list is
this list: count and top are set back to the mark's (what was appended after the prefill is dropped), count a deep
drop, and for the first 8 drops of the process log `netanim: deep tree dropped (%u slots, limit 5)` (depth).

### 3.7 Bit count padding call-outs (`0x60e296`, `0x60e9f3`, new)

Player site `0x60e296` (`mov [r12+0xbc], eax`, 8 bytes, resume `0x60e29e`, snapshot = r12) and other site
`0x60e9f3` (`mov [rbx+0xbc], eax`, 6 bytes, resume `0x60e9f9`, snapshot = rbx): save, host(rdi = snapshot,
esi = eax = the bit count), restore, jump. Registers (eax included) are as on entry.

Host: n = (bits + 31) & ~31 (u32 arithmetic); when n > 0x550, n = bits. Store n as u32 at snapshot+0xbc. Purpose
(code comment): the game copies ceil(bits / 8) bytes of a stream filled from the top of 32-bit words, so the high
bits of the last partial word never reached the wire.

### 3.8 Host visitor slot 3

1.4.4 slot 3 checks "readable" (3.3) instead of non-NULL for the anim, the event table and the info pointer. Only
reachable through slot 4, which is not installed.

### 3.9 natives.cpp: value logs

net-coop-host-script-id, net-coop-script-id and net-coop-task each remember the last value they returned in this
process (initially `0xffffffff`). When the value they are about to return differs, they log, after all guest calls
and right before returning:

- `natives: net-coop-host-script-id -> %08x`
- `natives: net-coop-script-id -> %08x`
- `natives: net-coop-task -> %08x (a co-op wave script is running)`

No cap. The returned values are unchanged (2.5).

### 3.10 Open: generator slot 4

The current code does not replace `0x5f48d0` (3.1 step 10). Whether it should is being decided on another branch;
until then 1.4.3 / 1.5 steps 6-7 describe the old behaviour and this spec does not say which one is right. The test
keeps that difference visible (3.11).

### 3.11 How the rewrite test maps these changes

`gow3_pc/tests/u2mp_rewrite/test_netanim.cpp` turns the old record and image into the expected new ones by the rules
above (3.1 log lines, request count, installed count, the new patch bytes, 3.2 / 3.3 stream results, 3.9 lines),
prints the 3.1 step 5 pointers as stub pool offsets, and checks 3.1 steps 2-3 / 9, 3.2 step 1 and 3.3-3.7 (an
unreadable art group +0x30 included) with new-only scenarios. The slot 4 lines, the slot's +1 and the slot's 16 image bytes are compared on their own: a
scenario whose only difference is that part is reported as `OPEN-SLOT4`, not as passed.
