# Spec: PS3 parity, group K (Twitter page, account test, auto-posts, post thread)

Clean-room parity check of the 54 PS3 1.10 functions of `maintainer-tools/research/u2-parity/groupK.tsv`
(netmap range "twitter page and thread", `0x4ed4e0..0x4f1d43`) against the UCMR U2 runtime (`gow3_pc/src/u2mp`,
branch `u2mp-ps3-parity-3`). It says what the PS3 player sees and what our port must show, with the Twitter service
treated as **gone** (its old API no longer exists). Nothing here connects to Twitter / X or any other service. It does
not prescribe code, structure or names. Addresses are PS3 1.10 (`NPUA80698` EBOOT) unless marked PS4 (PS4 1.02 image
address). Server host names, paths and keys found in the binary are `<omitted>`.

Status words (as in `parity-e-flow2.md`):

- **COVERED**: our code, or the intact PS4 code it relies on, gives the same visible result.
- **PLATFORM**: PS3 plumbing (HTTP, threads, NetCtl, system modules, live settings, posting) that the PC design
  replaces. The visible result must still match; the replacement is named and any delta stated.
- **MISSING**: absent, or present with a different visible result.

Reading notes. All 54 OPD descriptors carry TOC `0xe183b8`. The module reads its constants through two module data
pointers (TOC slots `0xe10ba8` -> `0xdbbe84` and `0xe10bac` -> `0xdbbef8`); every string and number below was
resolved that way. The decompile `groupK-ps3.c` drops jump tables and several branches; these were read in the
disassembly (llvm-objdump): the account test (`0x4f14d0`), the status text (`0x4f13f0`, not a group row), the label
dispatcher (`0x40b46c`, not a group row), the username label truncation (`0x4f1208`) and the callback registration
(`0x4f0a10`, not a group row). Texts were looked up in the PS4 1.02 and GOTY PS3 English banks; **every text id below
is in the PS4 banks** (common), so no own text is needed except where said. The PS3 `menu.bin` was read from
`build/main/dc1.psarc` (u2import `--psarc extractall`), the PS4 one from `dc1/ps4/menu.bin`.

Rows `0x4f1c44`, `0x4f1ca4`, `0x4f1d34` are **not Twitter code**: they use the next module's data pointer (TOC slot
`0xe10bb0`) and their caller is the player start-phase code (`0x4f23e4`, `game/player/calc-start-phase.cpp`), which
PS4 keeps. The netmap range is one module too long there.

## 1. Facts every item uses

### 1.1 The Twitter manager

One global object (PS3 `0xf3de80`, PS4 **`0x11572a0`**). PS4 kept the object, its constructor (PS4 `0x677100`), its
registration (PS4 `0x6766d0`), its label functions, its save-block functions and its per-frame clock; it dropped the
HTTP calls, the post thread, the keyboard handling, the account test and the event-settings file. Fields the items use
(PS4 offset; PS3 offset in brackets where it differs):

| PS4 | meaning |
|---|---|
| +0x00 u32 | **Remember Password** toggle (menu variable `0x243f9dd2`) |
| +0x04 u32 | **Activate Updates** toggle (menu variable `0x77fc3f6f`): auto-posting on |
| +0x08 u32 | event toggle: when connecting to multiplayer (variable `0x5b9d0048`) |
| +0x0c u32 | event toggle: co-op difficulty unlocked (variable `0x5c9f2681`; **no control** on PS3 or PS4) |
| +0x10 u32 | event toggle: cinema file uploaded (`0xe8116e7e`) |
| +0x14 u32 | event toggle: chapter completed (`0xeac923e6`) |
| +0x18 u32 | event toggle: multiplayer money earned (`0xa629448c`) |
| +0x1c u32 | event toggle: trophies earned (`0xa8c481ce`) |
| +0x20 u32 | event toggle: level milestone (`0x90045323`) |
| +0x58 byte (PS3 +0x48) | **service enabled**: every PS3 HTTP request refuses to run while it is 0, and the Options entry is blank (1.5) |
| +0x60 ptr (PS3 +0x4c) | service text used for the profile link in posts (0 = none) |
| +0x184 (PS3 +0x168), 128 bytes | **credentials** string `<username>:<password>@` (empty account = `:@`) |
| +0x204 byte (PS3 +0x1e8) | host reachable (constructor: 1) |
| +0x205 byte (PS3 +0x1e9) | network up (constructor: 1) |
| +0x206 byte (PS3 +0x1ea) | account verified (constructor: 0) |
| +0x207 byte (PS3 +0x1eb) | last test: wrong username / password (constructor: 0) |
| +0x208 byte (PS3 +0x1ec) | last test: failed for another reason (constructor: 0) |
| +0x34a (PS3 +0x32e) | display copy of the username (NUL-terminated), read by the username label |

The menu variables are resolved by the PS4 toggle lookup (`0x5d1cd0`, a switch on the variable id returning the field
address), so the toggles of both pages already read and write these fields on PS4.

"Username set" = the credentials string has a `:` that is not its first character. "Password set" = it has a `:` and
an `@`, and the `@` comes at least two characters after the `:`. "Credentials complete" = both.

### 1.2 The pages (identical on PS3 and PS4 menu.bin)

PAGE_TWITTER (`0x2ad3b7ab`, title text `0xd5a69a1f` "TWITTER"), top to bottom:

1. label, page-owner text query 2 (status line, 1.4);
2. BUTTON_TWITTER_USERNAME `0x2c738b98`, text query 0 (1.3);
3. BUTTON_TWITTER_PASSWORD `0xb1668731`, text query 1;
4. toggle "Remember Password" (`0x645cb383`), variable `0x243f9dd2`;
5. toggle "Activate Updates" (`0x5e46c2ec`), variable `0x77fc3f6f`;
6. BUTTON_TWITTER_SIGN_IN `0xa989fea1`, text query 3 ("Test Account");
7. BUTTON_TWITTER_MESSAGES `0x8cbfb139` "Events" (`0x891df5c4`) -> PAGE_TWITTER_MESSAGES;
8. description label "Tweet your progresses as you play" (`0x4bd57b41`, flag 8).

PAGE_TWITTER_MESSAGES (`0x5b98956e`, title `0x2eb994f7` "TWITTER EVENTS"): header "Post An Update..."
(`0xe60454c7`), then six toggles: When Connecting To Multiplayer (`0x9fc5aa5c`), When A Cinema File Is Uploaded
(`0xa4b65b91`), When A Chapter Is Completed (`0xefacaca6`), When Earning Multiplayer Money (`0x1671d01e`), When
Earning Trophies (`0xf6c852e8`), When Reaching A Level Milestone (`0x5893890c`).

"Page-owner text query" is PS3 action 27 / PS4 action 28 (0x1c): the control's label comes from a dispatcher
by the control's condition 0..4 (PS3 `0x40b46c`, PS4 **`0x5b0410`**, condition at query +0x44, intact). The only other
users of action 28 are the two PLAYGO stall labels.

The Options entry: PS3 PAGE_OPTIONS (`0xc7597732`) has, last of six, **BUTTON_OPTIONS_TWITTER** (`0xc173d8f7`, label
`0x168207da` "Twitter", text query 4, target PAGE_TWITTER). **PS4 menu.bin has no such control**: its PAGE_OPTIONS
ends with BUTTON_OPTIONS_CREDITS instead, and no PS4 control leads to PAGE_TWITTER. So on PS4 (and on our port today)
the Twitter pages cannot be reached.

### 1.3 The button labels (PS3 and PS4 the same)

- **Username** (query 0; PS4 `0x676d30`, intact): username set -> "Username:" (`0x6aa70c0e`), one space, then the
  display copy of the name cut to at most 22 characters and at most 79 bytes without splitting a UTF-8 sequence;
  when it was cut and fewer than 75 bytes were kept, "..." is appended. Not set -> "Enter Username" (`0x51994978`).
- **Password** (query 1; PS4 inline in `0x5b0410`): password set -> "Password: ********************" (`0x640d492f`,
  always 20 stars, the length is never shown); else "Enter Password" (`0xcc8c45d1`).
- **Sign in** (query 3): always "Test Account" (`0x4db5091c`).

### 1.4 The status line (query 2; PS3 `0x4f13f0`, PS4 `0x677000`, intact)

First match wins:

| condition | text |
|---|---|
| credentials not complete | "Incomplete Username/Password" `0x30426bd8` |
| network up = 0 | "The Network Is Down" `0xb6fd52dc` |
| host reachable = 0 | "Twitter Host Is Down" `0xe09857a2` |
| verified = 1 | "Account Verified Successfully" `0x24089a11` |
| wrong credentials = 1 | "Invalid Username/Password Combination" `0xd419eae1` |
| failed = 1 | "Failed To Test Account" `0x43811d2e` |
| otherwise | "Account Not Verified" `0x7293eac4` |

Unused bank texts of the feature (no code reads them): "Checking..." `0xdffa83c3`, "Signed-In" `0x992d1e78`,
"Signed-Out" `0xad7802ca`, "Sign-Out" `0x37f05bff`, "Account Sign-Out", "Twitter message1/2". A fifth button id,
`0xddaab65a`, is registered to the button callback on both PS3 and PS4 but no control has it.

### 1.5 When PS3 shows the Options entry (query 4; PS3 `0x4efc6c`, PS4 inline in `0x5b0410`)

The label is "Twitter" only when all hold, else empty (the entry shows no label):

- the manager's service-enabled byte is non-zero;
- the game is not online (NET_SM, PS4 `0x14ce97c`, below 2);
- the menu manager (PS4 `0x1136640`) has its field +0x1014 non-zero and its field +0 zero (PS3 `0xf0e800` +0x5b4 and
  +0; meaning not traced, most likely "front-end menu, not the in-level pause menu").

The service-enabled byte is only ever **cleared** in this module (constructor); no byte store anywhere in the PS3
executable sets it. It is most likely part of the downloaded Twitter live settings (the module also frees a
downloaded buffer it owns, and keeps the name of an encrypted settings file `<omitted>`), copied in as a block. With
the service gone that download fails, so **a PS3 today shows no Twitter entry** and every request it would make
refuses to run (open question 1).

### 1.6 What PS3 stores, and where

- **In the game save** (PS3 builds the block at `0x4f10f8`, applies it at `0x4f15cc`; PS4 **`0x676a70`** /
  **`0x676bc0`**, called by the PS4 save writer / reader, intact): a status word (0 = valid), the 128-byte
  credentials string, and two flags (Remember Password, Activate Updates). When Remember Password is off the password
  part is emptied in the saved copy (the username is kept). Applying a valid block sets the credentials, both flags and
  the display copy of the username; on PS3 it then **runs the account test when the credentials are complete** (PS4
  dropped that call).
- **In a small file `%s/twitter.msg`** in the game's data folder (load `0x4eda98`, save `0x4f0c34`; both absent on PS4):
  148 bytes, version word 4, the seven event toggles as bytes (+0x08..+0x20 order of 1.1), then seven per-event
  records with the last post times. Missing file or another version: toggles stay at 0 (off). The save is reached
  through a class method (vtable `0xd720f8`, slot 4); its trigger was not traced.

### 1.7 The account test (PS3 `0x4f14d0`; absent on PS4)

Run by Test Account, by entering a username or password when the credentials are then complete, and after a save
block with complete credentials is applied. Synchronous, in this order:

1. network up := 1; ask the system network state; when the call fails or there is no IP address: network up := 0, stop.
2. host reachable := result of a test request to the service host (OK only for HTTP 200; refused, so 0, while
   service enabled is 0).
3. Only when the credentials are complete **and** the host is reachable: clear verified, wrong, failed; then a
   verify request with the credentials: HTTP 200 -> verified := 1; HTTP 401 -> wrong := 1; anything else -> failed := 1.

(Flags from an earlier test stay when step 1 or 2 stops; the status line hides them behind the network / host lines.)

### 1.8 Posting (all PLATFORM, never visible offline)

A post is queued only when Activate Updates is on **and** the account is verified (`0x4efb64`), into a 4-deep queue
served by a worker thread ("Twitter Thread") that sends a status update per entry. Event sources and their bank
templates (tokens `[GAME]`, `[PROFILE]`, `[SITE]`, `[#COUNT]`, `[#DAYS]`, `[#LEVEL]`, `[#CHAPTER]`, `[#EARNED]`,
`[NET_MONEY_SIGN]`, `[CINEMALINK]` are filled in): connecting to multiplayer (`0xa1bf88c1`, from the connecting
page), cinema upload (`0xa83db3af`), chapter completed (`0xacfcae18`), level milestones (`0x73a6491d`), money and
trophies over a day / several days (`0xb2bbfe1d`, `0xb67ae3aa`, `0x776754aa`, `0x7a247273`, `0xbb39c573`,
`0xbff8d8c4`, `0x87b76579`, `0x837678ce`), the four "finished in Easy / Normal / Hard / Crushing" trophies
(`0x692004af`, `0x7ee56fc4`, `0x60a23fc1`, `0x64632276`), co-op difficulty unlocked (`0xa57e9576`). Each event has a
minimum interval and a rate window (the constructor table, PS4 `0x677100` +0x104..+0x154, intact). Because the account
is never verified offline, **nothing is ever queued**; the PS4 copies of the composers (e.g. `0x675b30`) only build
texts.

## 2. What our port does today

- PS4 slots `0x6769d0` (PAGE_TWITTER) and `0x6769e0` (BUTTON_TWITTER_*) are `ret` stubs registered by PS4's own
  registration; remaining.cpp replaces them: page event 6 pops the page and shows the box `kTextTwitter` ("Twitter is
  not available on PC."), every button shows the same box (remaining.md B26 / B27).
- The PS4 label dispatcher, status line, save block, constructor and toggle variables work unchanged.
- The pages are unreachable (no Options entry), so the player never sees any of it.

## 3. Items to implement

The owner wants a working page that reports the service as unavailable (section 6, question 1, for the strict-parity
alternative). Items K1..K5 describe that page. The service itself stays absent: no request of any kind is made.

### K1. The Options entry (PS3 PAGE_OPTIONS control 6, `0x4efc6c`) - MISSING (menu data)

- The import's menu overlay adds to PAGE_OPTIONS a control equal to the PS3 BUTTON_OPTIONS_TWITTER (id `0xc173d8f7`,
  button kind, label `0x168207da`, action 28 condition 4, target PAGE_TWITTER `0x2ad3b7ab`), placed after
  BUTTON_OPTIONS_LANGUAGE (PS3 position; before PS4's Credits).
- The runtime sets the manager's service-enabled byte (PS4 +0x58) to 1 once at install, so the intact PS4 label
  dispatcher shows "Twitter" under PS3's other rules of 1.5 (front-end, not online). This is a deliberate delta (1.5:
  a PS3 today shows nothing). The only PS4 readers of that byte found are the two copies of the entry label
  (`0x5b0410`, `0x676fa0`); the PS4 has no request code it could unlock.
- To check in the game: a control whose page-owner label is empty must not be shown or selectable (pause menu,
  online). If PS4 shows it blank but selectable, the overlay entry must be hidden by the same means the menu already
  uses for hidden rows instead.

### K2. PAGE_TWITTER callback (PS3 `0x4f1950` -> `0x4f1744`; row `0x4f1744` string "twitter") - MISSING

Replaces remaining.cpp B26. Page events (arg +0):

- **0 (opened)**: PS3 pushes the "twitter" system-module set and prepares the two keyboard titles (`0x4eff24`) -
  PLATFORM, nothing to do. Ours: when the credentials are complete and no account test ran since the game started,
  run the offline account test (K4) now, so the status line is right on the first frame (replaces PS3's test after
  loading the save, 1.6; the status is only visible on this page).
- **2, 3, 4 (left)**: PS3 pops the system-module set. Ours: nothing (a text box is modal and closed before the page
  can be left; if one is still open, close it as cancelled).
- **6 (every frame)**: poll the open text box (K3), if any.
- The page is never popped by our code and no message box is shown.

### K3. Buttons (PS3 `0x4f1c38` -> `0x4f195c`; `0x4efda4`, `0x4efe54`, `0x4eff24`) - MISSING

Replaces remaining.cpp B27 for all ids registered to the slot (`0x2c738b98`, `0xb1668731`, `0xa989fea1`,
`0xddaab65a`). PS3 reads the pressed control's id from the first 32-bit word of the callback argument (verify that
the PS4 button dispatch passes the same; remaining.md 179 lists arg +8 as "button number / answer"). Any press first
closes a text box that is still open (as cancelled).

- **Username** (`0x2c738b98`): open the PC text input (the one the clan tag uses) with the prompt "Please Enter Your
  Twitter Username" (`0xd3373c12`, the game's text) and the current display username as initial text.
- **Password** (`0xb1668731`): open it with the prompt "Please Enter Your Twitter Password" (`0x4e2230bb`) and an
  empty initial text. PS3 used the system keyboard; whether it masked the input was not traced (question 3).
- **Test Account** (`0xa989fea1`): when the credentials are complete, run the account test (K4); otherwise nothing.
- `0xddaab65a`: nothing.

Text box result (polled by K2 event 6): cancelled -> nothing changes. Accepted username -> the username part of the
credentials string (before `:`) becomes the text, the password part is kept, and the display copy (+0x34a) becomes
the text; the whole string stays within its 128 bytes with the `:` and `@` intact. Accepted password -> the password
part (between `:` and `@`) becomes the text, the username is kept. An empty accepted text empties that part. After
an accepted text, when the credentials are complete, run the account test (K4). Log (our wording; never log the
password): `twitter: username entered`, `twitter: password entered`.

### K4. Offline account test (PS3 `0x4f14d0`) - MISSING

Same flags and order as 1.7, with fixed answers: network up := 1 (no NetCtl on PC); host reachable := 0 (the service
is gone); step 3 never runs, so verified / wrong / failed keep their values (all 0 offline). Result on the status line:
"Twitter Host Is Down". No request is made. Log once per test: `twitter: account test: service unavailable`.

Visible sequence on a fresh profile: page opens with "Incomplete Username/Password", "Enter Username", "Enter
Password", "Test Account"; after entering both: "Username: <name>", "Password: ****...", "Twitter Host Is Down".
After restarting with Remember Password on: the same filled page with "Twitter Host Is Down"; with it off: the name
kept, "Enter Password", "Incomplete Username/Password".

### K5. Event toggles remembered (PS3 `0x4ed4e0`, `0x4ed684`, `0x4eda98`, `0x4f0c34`) - MISSING

PS4 keeps the seven event toggles only in memory (1.6: the twitter.msg file was dropped), so they reset to off at
every start. Ours keeps them in a small local file of our own format (the seven 0/1 values of PS4 +0x08..+0x20, in
that order, plus a version) in the runtime's own data folder: read once when the game starts (missing or unreadable
file: all off, as PS3), written whenever one of the seven values differs from the last written state (PS3's exact
write trigger was not traced; writing on change keeps every change, which PS3 also did by the time it saved). The
last-post times of the PS3 file are not kept (they only rate-limit posts, which never happen). Remember Password,
Activate Updates and the credentials need nothing: they travel in the game save through the intact PS4 block (1.6).

## 4. PLATFORM items and what replaces them

| PS3 | what | replacement / delta |
|---|---|---|
| `0x4ef8d0`, `0x4f0828`, `0x4ef9ac`, `0x4f0bf8`, `0x4ed8b0`, `0x4ed8ec` | post thread, its queue (4 entries), result read, module handle init | none; nothing is queued (1.8) |
| `0x4f0428`, `0x4f0510`, `0x4f05dc` | HTTP verify, host test, status update (`<omitted>` host and paths) | K4 fixed answers; no request |
| `0x4f14d0` NetCtl part | system network state | network up := 1 (K4) |
| `0x4f1744` events 0 / 2..4 | push / pop the "twitter" system-module set | none |
| `0x4f0058` | frees the downloaded live-settings buffer | none (nothing downloaded) |
| `0x4ed748`, `0x4edda8`, `0x4efb64` | "event allowed" (toggle on and live allowance), rate checks, queue only when verified and Activate Updates | never true offline; no post |
| `0x4edea0`, `0x4ee590`, `0x4eea40`, `0x4ef314`, `0x4ef490` | post composers (money + trophies, single event, trophies / difficulty finished, cinema upload, "playing multiplayer" from PAGE_MULTIPLAYER_CONNECTING) | PS4 copies build texts only; no post |
| `0x4ef584`, `0x4ef600`, `0x4f01e0`, `0x4f0374`, `0x4f03f0` | bounded copy, buffer clear, token replace (post buffer 428 bytes, at most 419 used), number token, token test | only feed posts |

## 5. Test scenarios

1. Main menu -> Options shows "Twitter" after Language; pause menu inside a level and the online menus do not (K1).
2. Fresh profile, open the page: status "Incomplete Username/Password", buttons "Enter Username" / "Enter Password" /
   "Test Account", both toggles off; no message box, the page stays (K2).
3. Enter "abcdefghijklmnopqrstuvwxyz": label "Username: abcdefghijklmnopqrstuv..." (22 characters + "..."); a
   multi-byte name is never cut inside a character (1.3).
4. Enter a password: label "Password: ********************", status "Twitter Host Is Down"; the log has no password.
5. Cancel each text box: nothing changes. Test Account with an incomplete account: nothing changes.
6. Remember Password on, save, restart, open the page: name and stars kept, status "Twitter Host Is Down". Off:
   "Enter Password", "Incomplete Username/Password".
7. PAGE_TWITTER_MESSAGES: switch three toggles on, restart: the same three are on (K5); delete the file: all off.
8. Play a match, finish a chapter, earn money with Activate Updates on: no post, no thread, no request, no crash.
9. Languages: every label above appears in the selected language (all ids are in the PS4 banks).

## 6. Open questions

1. **Owner decision: page or no page.** Strict parity with a PS3 whose Twitter live settings no longer download is
   *no Options entry at all* (1.5), which is what our port shows today (nothing reachable) and how the U3 helper
   handled dead services (agent a011: "no dead options visible", entries hidden). The task asked for a working page
   that reports the service as unavailable; K1..K5 specify that. If the owner prefers the hidden form, only the
   unreachable stubs remain and K1..K5 are dropped (remaining.cpp B26 / B27 could stay as they are).
2. Whether an empty page-owner label hides the row on PS4 (K1 check).
3. Password entry: the PC text input shows what is typed; PS3's keyboard mode was not traced. Masking needs the text
   input to support it; otherwise the password is visible while typed.
4. Privacy: with Remember Password on, the password is written in plain text into the game save (PS3 behaviour,
   kept by the intact PS4 block). For a dead service the owner may prefer never saving it (a small delta in the
   applied block: always store the password part empty).
5. PS4 button callback argument: where the pressed control id is (K3).

## 7. Status table

Counts: 18 COVERED, 23 PLATFORM, 13 MISSING (plus the missing Options entry in menu.bin, K1).

| PS3 | what | status | where / item |
|---|---|---|---|
| 0x4ed4e0 | default event-settings block (version 4, toggles off, per-event records) | MISSING | K5 |
| 0x4ed684 | copy the seven event toggles into the block (non-zero -> 1) | MISSING | K5 |
| 0x4ed748 | event allowed: toggle on and live allowance > 0 | PLATFORM | 4 |
| 0x4ed8b0 | static init: a module handle := -1 | PLATFORM | 4 |
| 0x4ed8ec | its wrapper | PLATFORM | 4 |
| 0x4ed8fc | event poster init: owner, default block, per-event interval / window table | COVERED | PS4 constructor `0x677100` (same values) |
| 0x4eda98 | load `%s/twitter.msg`: last-post times := now - 1 day, accept version 4, apply toggles | MISSING | K5 |
| 0x4edd24 | per-frame clock; 2 s after a post, read the thread result | COVERED | PS4 inline (`0x3fd7e7`); result part never used |
| 0x4edda8 | rate checks for money / trophy posts | PLATFORM | 4 |
| 0x4edea0 | compose money + trophies post, queue it, stamp both events | PLATFORM | 4 (PS4 `0x675b30` texts only) |
| 0x4ee590 | compose single-event post, queue it, stamp | PLATFORM | 4 |
| 0x4eea40 | trophy events: count per day, "finished in <difficulty>" trophies | PLATFORM | 4 |
| 0x4ef314 | cinema upload post | PLATFORM | 4 |
| 0x4ef490 | "playing multiplayer" post (from PAGE_MULTIPLAYER_CONNECTING) | PLATFORM | 4 |
| 0x4ef584 | bounded string copy | PLATFORM | 4 |
| 0x4ef600 | clear a 127-byte buffer | PLATFORM | 4 |
| 0x4ef6f8 | empty method | COVERED | nothing to do |
| 0x4ef6fc | current time in seconds (frame clock + boot time) | COVERED | PS4 inline |
| 0x4ef74c | UTF-8-safe copy limited by characters and bytes, says if cut | COVERED | PS4 username label `0x676d30` (inline) |
| 0x4ef8d0 | start "Twitter Thread" | PLATFORM | 4 |
| 0x4ef9ac | queue a post (4 entries, lock, wake) | PLATFORM | 4 |
| 0x4efb64 | queue only when verified and Activate Updates on | PLATFORM | 4 |
| 0x4efbdc | credentials := `:@` | COVERED | PS4 registration / constructor |
| 0x4efda4 | set the password part | MISSING | K3 |
| 0x4efe54 | set the username part | MISSING | K3 |
| 0x4eff24 | keyboard titles (`0xd3373c12`, `0x4e2230bb`) | MISSING | K3 (PC text input prompts) |
| 0x4f0058 | free the live-settings buffer | PLATFORM | 4 |
| 0x4f00f0 | "password set" test | COVERED | PS4 inline in `0x5b0410`, `0x677000` |
| 0x4f0178 | password label | COVERED | PS4 inline in `0x5b0410` |
| 0x4f01e0 | token replace in the post buffer | PLATFORM | 4 (PS4 `0x6764a0`) |
| 0x4f0374 | number token | PLATFORM | 4 |
| 0x4f03f0 | token present | PLATFORM | 4 |
| 0x4f0428 | verify credentials request (200 / 401 / other) | PLATFORM | K4 |
| 0x4f0510 | host test request | PLATFORM | K4 |
| 0x4f05dc | status update request | PLATFORM | 4 |
| 0x4f0828 | post thread loop ("TwitterPostThread::Unkown event") | PLATFORM | 4 |
| 0x4f0bf8 | read the last post result | PLATFORM | 4 |
| 0x4f0c34 | save `%s/twitter.msg` | MISSING | K5 |
| 0x4f0cdc | manager constructor (flags 1.1, `:@`, poster init) | COVERED | PS4 `0x677100` |
| 0x4f0de8 | static init / exit: manager, queue reset, thread start | COVERED | PS4 `0x677100` (thread part PLATFORM) |
| 0x4f10d8 | static exit wrapper | COVERED | as above |
| 0x4f10e8 | static init wrapper | COVERED | as above |
| 0x4f10f8 | build the save block (password dropped unless remembered) | COVERED | PS4 `0x676a70` (question 4) |
| 0x4f1208 | username label ("%s %s") | COVERED | PS4 `0x676d30` |
| 0x4f1380 | credentials complete | COVERED | PS4 inline; K3 / K4 use the same rule |
| 0x4f14d0 | account test (NetCtl, host, verify) | MISSING | K4 |
| 0x4f15cc | apply the save block, then account test | MISSING | apply COVERED (PS4 `0x676bc0`); test at page open, K2 |
| 0x4f1744 | page events (open / leave / frame), keyboard result | MISSING | K2, K3 |
| 0x4f1950 | PAGE_TWITTER | MISSING | K2 (ours pops the page and shows a box) |
| 0x4f195c | button handler | MISSING | K3 |
| 0x4f1c38 | BUTTON_TWITTER_* | MISSING | K3 (ours shows a box) |
| 0x4f1c44 | player start-phase helper (not Twitter) | COVERED | PS4 player module |
| 0x4f1ca4 | player start-phase statics (not Twitter) | COVERED | PS4 player module |
| 0x4f1d34 | their wrapper (not Twitter) | COVERED | PS4 player module |

## 8. Files to touch

- `gow3_pc/src/u2mp/remaining.cpp`: B26 / B27 replaced by K2 / K3 (or a new `twitter.cpp` next to `clantag.cpp`, which
  holds the PC text input use); install sets the service-enabled byte (K1).
- `gow3_pc/src/u2mp/texts.cpp`, `texts.h`: `kTextTwitter` no longer used (remove, or keep for question 1's fallback).
- K5 store: a small file of our own (next to the runtime's other local stores, e.g. beside `progression_store.cpp`).
- `u2mp/patcher/importer/overlay.cpp`: PAGE_OPTIONS gets BUTTON_OPTIONS_TWITTER (K1).
- `gow3_pc/tests/u2mp_rewrite/test_remaining.cpp`: B26 / B27 cases follow the new behaviour.
- `u2mp/docs/spec/remaining.md` B26 / B27, `MENUS.md` 1 ("only menu.bin difference" also misses this Options entry).
