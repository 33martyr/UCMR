# Spec: PS3 parity, group A (leaderboards and stats)

Clean-room behaviour description of the PS3 1.10 leaderboard ("net-leaderboard": NP Score) and career storage
("NetPlayerData": NP TUS + HDD copy) functions that PS4 1.02 lacks, measured against what our runtime already does.
It says what the player must see and what game memory must hold; it does not prescribe code. Conventions as in
`progression.md` and `session.md`: addresses are PS4 1.02 image offsets unless marked "PS3"; game memory is
little-endian; "NPD" = NetPlayerData `0x11474f8`; "LB" = the leaderboard object `0x113d960`; "NET_SM" = s32
`0x14ce97c` (> 1 = online); log texts are printf formats passed to `u2mp::log` and must match exactly.

Sources: rows of `maintainer-tools/research/u2-parity/groupA-frontend.tsv` and `groupA-gameplay.tsv`, plus the
PS3 callers/thread bodies `0x441e30`, `0x4c643c`, `0x4c699c`, `0x4c6ab8` (read for this spec only).

Already specified and implemented (not repeated here): `progression.md` 1.5.8 (scores posted at Leave), 1.5.11-1.5.15
(leaderboard list update `0x5b3000`, local and online fetch), `remaining.md` part 2 (clan tag entry),
`PORT-ONLINE.md` 5c (RPCN boards 300..303 of NPWR00462, shared with RPCS3).

## 1. Status table

COVERED = same visible result already. PLATFORM = Sony plumbing; our design replaces it (named), with the
listed deltas still to do. MISSING = player-visible PS3 behaviour we lack. Section numbers point to the specs below.

| PS3 | Role (what it does) | Status | Where in our code / to do |
|---|---|---|---|
| `4a9e8c` | clan-tag check: "rejected" flag of the last finished check | MISSING | 2.1 (clantag.cpp + online) |
| `4a9f78` | clan-tag check: poll the running check; on finish free it and store the flag | MISSING | 2.1. (The audit called it "leaderboard poll"; it is the censor poll. PS4 `64e060` is its dead remnant: leave it.) |
| `4aa000` | LB reset: counters 0, page shown -1, the 4 score slots cleared | COVERED | PS4 LB object is static data; progression.cpp module state |
| `4aa084`, `4aa0c4` | static constructor of the LB global (compiler) | COVERED | nothing to port |
| `4aa0d4` | clan-tag check: start a censor request on a text | MISSING | 2.1 |
| `4aa170` | ranking fetch (global "my page", global range, friends) | PLATFORM | progression.cpp 1.5.13/1.5.14 + online.cpp `board_request`; deltas 2.2, 2.3, 2.4 |
| `4ab3e8` | start a page fetch (board id, page; ignored while one runs) | PLATFORM | progression.cpp fetch start (1.5.13) |
| `4ab45c` | start a score post in one of 4 slots; all busy = dropped | PLATFORM | progression.cpp 1.5.8 + online.cpp `record_score`; delta 2.5 |
| `4ab620` | ranking thread entry (runs `4aa170`, ends the thread) | PLATFORM | online.cpp async request |
| `4ab66c` | score service term (cancel pending check, drop contexts) | PLATFORM | online link teardown; delta 2.4 (no hanging fetch) |
| `4ab6d0` | score post thread: record (board, score, empty comment, 64-byte info), log, free slot | PLATFORM | online.cpp `record_score` (comment NULL, info 64 bytes) |
| `4ab854` | score service init from NetInit: own NpId into LB, title context, row buffers (11 rows, 101 friend entries) | PLATFORM | online program sign-in (`my_npid_text`), progression.cpp row buffers |
| `4aba2c`, `4aba74` | static constructor of the live-data module (compiler) | COVERED | nothing to port |
| `4aba88` | NetInit live-data download (manifest, "ingame" files, net-patch files: download, inflate, write, rename; NetInit progress 8..31) | PLATFORM | not leaderboards: netpatch.cpp + import provide net-patch9 (PORT-NETPATCH.md); other live files: group owning netinit |
| `4dbe70` | bandwidth graph (two value lines, 126 samples) of the debug net display | COVERED | debug overlay only (from net clock `43b698`); not shown to players |
| `4c5e08` | write the HDD copy of the career (16-byte header + 0x228 Data, encrypted) | COVERED | progression_store.cpp `save` (progression.bin) |
| `4c5f48` | write the career to TUS slot n (10 s timeout); slot 1 also writes the HDD copy | PLATFORM (no equivalent) | 2.6 account copy (owner decision) |
| `4c6298` | Publish: counter +1, copy + encrypt Data twice, start "Save Game Stats" thread; re-publish flag while one runs | COVERED | progression.cpp publish (1.5.4); delta 2.7 (state words) |
| `4c65f8` | FetchTusData: read slot n; "no data" errors = new profile; version != 10 = new profile | PLATFORM (no equivalent) | 2.6 |
| `4c6d18` | NPD term: wait, drop TUS, state 0, clear the three Data copies | COVERED | progression.cpp offline guard (1.5.5, PS4's own remnant semantics) |
| `4c6e18` | TUS init + title context; failure = term | PLATFORM (no equivalent) | 2.6 |
| `4c7820` | Connect: TUS init, start "NetPlayerData Connect Thread" (state 1), failure = state 2 | COVERED | progression.cpp connect (1.5.3, synchronous, state 3); 2.6 when the account copy exists |
| `4e3648` | net stat game vtable slot returning a constant (PS3 16; PS4 `0x673900` returns 32) | COVERED | PS4's own accessor; open question 4.6 |
| (dup) `4aa170`, `4ab6d0`, `4ab854` | gameplay TSV repeats of the rows above | as above | |

Counts (25 distinct rows): COVERED 11, PLATFORM 11 (8 with an equivalent, 3 without), MISSING 3 (one feature).

## 2. Behaviour to implement

### 2.1 Clan tag check (MISSING; PS3 `4aa0d4`, `4a9f78`, `4a9e8c`, driven every online frame by `0x441e30`)

PS3 checked every new clan tag with the score service's comment censor before keeping it, and published the
career after the verdict. What the player saw: an offensive tag (or one whose check failed) became `*`.

Facts: the tag is the 8 bytes at NPD+0x1a8 (`0x11476a0`), NUL-padded (PS3: Data+0x164). PS3 compared the first
4 bytes; our entry stores at most 4 characters.

State: "last checked tag" (8 bytes, starts all zero, kept for the process), "check running" (starts false).

Every frame while NET_SM > 1 (PS3: the per-frame net update; ours: any per-frame hook that runs online, e.g.
`online::frame()`):

1. Check not running: compare the current tag (first 4 bytes) with "last checked tag". Equal: nothing.
   Different: "last checked tag" := current tag. If the current tag is empty: publish the career (progression
   publish, 1.5.4) and the member data (`online::member_data_changed()`), done. Otherwise start a censor check of
   the tag text; when it starts, "check running" := true. When it cannot start, nothing more (the tag stays as
   typed and is not re-checked until it changes).
2. Check running and finished: verdict "rejected" = the check ended with any non-zero result (refused **or**
   failed). If "last checked tag" is non-empty and rejected, it becomes `*` followed by zeros. Write the 8 bytes
   of "last checked tag" to NPD+0x1a8, publish the career, publish the member data, "check running" := false.

Consequences to keep: a non-empty tag loaded from the profile is checked again once per process when the player
goes online; clearing the tag publishes at once without a check; a tag replaced by `*` is not checked again
(`*` equals "last checked").

Censor service: a request in our online program taking the text (<= 63 bytes) and answering accepted / refused /
failed. Without such a service (online program lacks it, or not linked) the check answers "accepted" at once:
PS3's "failed = `*`" rule must not turn every tag into `*` when we have no censor at all.

Interaction with clantag.cpp: its immediate `progression_publish()` on accept may stay (it saves the typed tag);
the check then runs on the next online frame. Logs:
`clan tag: [%.4s] check started`, `clan tag: [%.4s] kept`, `clan tag: [%.4s] refused, now [*]`,
`clan tag: check unavailable, [%.4s] kept`.

### 2.2 Friends board (PLATFORM delta; PS3 `4aa170` friends branch)

What PS3 showed with the Friends filter (LB u8 +0x84 on PS4):

1. Entries: every friend of the friend list (all, online or not; at most 100) plus the player, each with its
   **global** ranking record on the board (rank column = global rank, score, record date, 64-byte info).
   When the friend list is being refreshed at that moment, only the player.
2. Entries without a score are dropped. The rest are ordered by global rank, lowest first (ties keep list order).
3. Page: requested page -1 ("go to my page", after a board/filter switch or reset) = index of the player among
   the kept entries / 11 (player not on the board: page 0). Requested page p >= 0: min(p, (kept - 1) / 11)
   (signed compare; with kept = 0 the page stays p and the page is empty).
4. Rows: kept entries from page * 11, at most 11, into the LB rows/info buffers as in progression.md 1.5.12;
   LB rows-on-page, page shown as usual.
5. Total (LB u32 +0x24): PS3 reported **twice** the kept count (it counted the entries a second time while copying
   rows). Visible effect: Next on the last friends page is allowed, fetches again and lands on the same last page.
   Reproduce it (owner's exact-parity rule) unless the owner says otherwise (4.4).

Our current path asks `board_request(id, 1, 0, 100)` and treats the answer as already filtered and sorted. The
implementation must check (test A-F1..F4) that mode 1 returns global ranks, includes the player, omits unscored
players and is rank-ordered; where it does not, sort/filter host-side, or fetch the friends' global records by
NpId list (the PS3 method) through the online program.

### 2.3 Global board, "my page" and row count (PLATFORM delta)

- Requested page -1: look up the player's record. Player has no score, **or the lookup fails**: page 0 of the
  global range (PS3 did not fall back to anything here). Player has a score: page (rank - 1) / 11.
- Requested page p >= 0: not clamped by the fetch (the list update clamps scrolling).
- Rows on page = total - page * 11 clamped to 0..11, where total is the board total from the range answer.
- Range request failing: PS3 showed an empty list (total 0, 0 rows). Ours shows the local board with the log
  `rankings: online board %u failed: local board shown`. Keep ours (better for the player) until the owner rules
  (4.3); the "my page" case above must not use the local fallback when the service merely says "no score".

### 2.4 No hanging fetch (PLATFORM delta; PS3 `4ab66c` + library timeouts)

While a fetch runs the list waits (LB busy). It must always end: when the online link goes down, or no answer
arrives within 10 s (the limit PS3 set on its stats storage requests; its score calls used the library default),
the fetch ends as failed (2.3 rules apply). Log: `rankings: board %u: no answer, failed`.

### 2.5 Score posts in flight (PLATFORM delta; PS3 `4ab45c`)

At most 4 score posts may be unanswered at once; a fifth is dropped (not queued). Comment empty, info 64 bytes
(first 16 big-endian: rank badge, seconds, deaths, losses), the service's rank answer only logged (as now).
Log on drop: `rankings: 4 scores pending, score %lld for board %u dropped`.

### 2.6 Account copy of the career (PLATFORM, no equivalent; PS3 TUS `4c6e18`, `4c5f48`, `4c65f8`, `4c6ab8`)

PS3 stored the career (Data, 0x228 bytes, version 10) on Sony's per-account storage, so it followed the account to
any console; the HDD file was a backup. Ours keeps only progression.bin per PC. Implement only if the owner wants
career roaming (4.1). PS3 behaviour to reproduce then:

- Connect (PAGE_MULTIPLAYER_CONNECTING): NPD state 1 while running. Read the account copy, up to 10 attempts
  (each with a 10 s limit). "No data stored" (PS3 errors `0x80022a2b`, `0x80022a4a`) counts as a successful read of
  a new profile (version 10, all zero, publish counter 0). A stored copy whose version is not 10 also becomes a new
  profile (log). Then read the local copy (progression.bin).
- Choice: both read: the one with the higher publish counter (Data+0x160 = NPD+0x1a4), the account copy on a tie;
  only one read: that one; none: Data stays new. Copy it into Data.
- NPD state: 3 when the account read succeeded, else 2 (PS3: then nothing is published that session, so a stale
  copy never overwrites the account). Decide with the owner whether our local save still runs in that case (4.1).
- Publish (each 1.5.4 publish): write the account copy (slot "main"), then the local copy. A publish requested while
  one is running sets a "publish again" mark; when the running one ends it publishes once more. Result: publish
  state 0 when all writes succeeded, else 2 (2.7).
- Storage: **never** PS3's slots 1 and 2 of NPWR00462 in our own format. RPCS3 players on RPCN share that
  communication id; their U2 reads slot 1 (and writes 2) in the PS3 encrypted format. Either use a slot the PS3
  game never touches, or write the exact PS3 format (needs a spec of PS3 Encrypt/Decrypt `0x4c4df0`/`0x4c4cd8`,
  not in this group).

Logs: `progression: account copy read (publish %u)`, `progression: account copy empty: new profile`,
`progression: account copy version %u, expected 10: new profile`, `progression: account copy unreadable (%d)`,
`progression: using the %s copy (account %u, local %u)` ("account"/"local"), `progression: account copy written`,
`progression: account copy not written (%d)`.

### 2.7 Publish state words (COVERED delta; PS3 `4c6298`, `4c643c`)

PS3 kept NPD+0x6bc = publish state (1 running, 0 done ok, 2 failed or not connected) and NPD+0x6d0 = "publish
again". Our publish is synchronous and writes neither. If any PS4 code reads `0x1147bb4` / `0x1147bc8` (to verify,
4.5), write 1 before and 0/2 after each publish (2 when the store save fails or state is not 3).

## 3. Test scenarios

- A-C1 online, tag changes `AB` -> check started, accepted: tag stays `AB`, one career publish, one member data
  publish, logs. A-C2 refused: tag `*`, publish, member data. A-C3 check fails (service error): `*`. A-C4 no censor
  service: `kept` log, tag unchanged, publish. A-C5 tag cleared: no check, immediate publish. A-C6 start fails: no
  publish, no retry next frame. A-C7 profile loaded with `XYZ`, first online frame: checked once; second frame:
  nothing. A-C8 NET_SM <= 1: nothing.
- A-F1 friends board: friends with ranks 40, 7, unscored, player rank 12: rows 7, 12, 40 with global ranks; total 6
  (2 x 3). A-F2 120 friends: at most 100 used. A-F3 page -1 with the player 15th: page 1. A-F4 page 5 with 14 kept
  entries: page 1; with 0 kept: page 5, 0 rows.
- A-G1 global page -1, player unranked: range from rank 1, page 0. A-G2 "my rank" lookup error: page 0, no local
  fallback. A-G3 range of total 25 on page 2: 3 rows. A-G4 range error: local board + log (until 4.3).
- A-H1 link drops while loading: fetch failed, list stops waiting. A-H2 no answer 10 s: same.
- A-S1 five posts with none answered: the fifth dropped with the log.
- A-T1..T6 (only with 2.6): account newer, local newer, tie (account), account "no data" + local counter 5
  (local), account read fails (state 2), version 9 on the account (new profile), publish while publishing (one extra).

## 4. Open questions

1. Career roaming (2.6): wanted? If yes: own slot or PS3-compatible format; keep local saves when the account read
   fails (PS3 did not save at all)?
2. Censor service (2.1): does our online program / RPCN offer a comment censor? RPCS3's implementation is believed
   to accept everything; with none, 2.1 reduces to the publish-after-change rule.
3. Online failure on the global range: PS3 empty list vs our local board (2.3).
4. Reproduce the doubled friends total (2.2 step 5)?
5. Does PS4 code read NPD+0x6bc / +0x6d0 (2.7)? Find readers of `0x1147bb4`, `0x1147bc8`.
6. `4e3648` / PS4 `0x673900`: which net stat game slot it is and whether 16 vs 32 is a real difference (e.g. a count
   PS4 changed on purpose) or a value PS4 should return as PS3 did.
7. PS3 wrote TUS slot 2 only when a global flag (PS3 `0x94de8` query, unknown meaning) was false; meaning unknown.
8. How other players see a clan tag (the member blob 0x59 does not carry it): outside this group, check in-match
   roster data.
9. Own-row highlight on PAGE_MENU_LEADERBOARD: which id the PS4 page compares rows with (row +0 npid text vs the
   local name); verify in a linked test.
