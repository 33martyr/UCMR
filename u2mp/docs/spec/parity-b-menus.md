# Spec: PS3 parity, group B (net-menu lists, roster rows, player cards)

Behaviour description for the implementer of the PS3 parity work, group B (81 PS3 1.10 functions,
`0x4b7124..0x4c2bac`). It says what a PS3 player saw and what our port must show; it does not prescribe
code. Addresses are PS3 1.10 (`NPUA80698` EBOOT) unless marked PS4 (PS4 1.02 image offset). Background:
`u2mp/docs/MENUS.md` (sections 1, 2, 3.1, 3.2, 3.5, 3.6 and the audit at the end), `PORT-FRONTEND3.md`
section 7, `PORT-ONLINE.md` section 5a, spec `menus.md`, `session.md`, `remaining.md`.

## 0. Facts every item uses

**Query table.** PS3 `0xe15b50` (12-byte entries `{cond, function, arg}`, end at fn 0) = PS4
`0x105e370` (same conditions in the same order). Every PS3 function of this group was placed by
reading that table (cond and arg per function: section 6).

**Query object** (what the game passes to a query function). PS3 field -> PS4 field, as far as our
code already confirms them:

| meaning | PS3 | PS4 |
|---|---|---|
| op (1 title, 2 row text, 3 image, 4 current index, 5 last index, 7 row flag, 8 refresh, 9 set index, 10 column flag, 17 row colour) | +0x00 | +0x00 |
| in/out value (row number, index, image selector) | +0x04 | +0x04 |
| image descriptor: type, then value | +0x08, +0x10 | +0x08, +0x18 |
| image descriptor: image id words (type 3) | +0x14..+0x1c | +0x1c..+0x24 |
| image descriptor: colour word (-1 = none) | +0x24 | +0x2c |
| image scale (PS4 only, 1.0) | - | +0x30 |
| op 10 result word | +0x28 | +0x38 (section 11.2) |
| op 17 row colour word | +0x2c | +0x3c |
| text out (pointer; NULL = not answered, "" = blank) | +0x30 | +0x40 |
| table arg (32 bits) | +0x34 | +0x48 |
| op 10 input selector (second argument) | +0x38 | +0x4c (section 11.2) |
| element id (which picture of the row) | +0x3c | +0x50 |
| column | +0x40 | +0x54 |

An unanswered op leaves the object as the caller built it (zeroed, op set). "Blank" below means the
text out is set to an empty string, which hides the label.

**Page mode** = PS3 netInfo `+0x2b4` = PS4 netInfo `+0x2c4`: 1 matchmaking lobby, 2 custom game lobby,
3 cinema.

**Member custom data** (the 0x30-byte data every session member publishes; PS3 lookup `0x4d5a18`, the
local member's own copy answers for the local id): byte `+8` rank, byte `+9` team (1 or 2, 0 none),
`+10` clan tag string (empty = none), float array from `+0x14` = skill level per skill board.

**Texts** (all in our banks, `texts.cpp`): c0759450 "Finding Player...", a91680a8 "Found Player",
84041d79 "In lobby", ea3bb70e "Watching a cinema", cf292b57 "Playing match", 77f62775 "Playing
Single-Player", 953825d0 "Playing another game", ba8db526 "[A] players in party", 93ac6446 "Match ends
in [TIME]". fd2de693 = the game's title text, used as the "playing this game" headline; it is in the
game's common bank in every language (section 11.1). Format strings (interop): clan tag `"[%s] %s"`, skill prefix
`"|@S||@R8080FF|%i|@RFFFFFF||@N| %s"` (number in light blue, then the name), friend status blocks start
with the highlight code `|@H|` and switch back with `|@N|`; match lines start with `"- "` and join
fields with `" / "`.

## 1. Party and lobby roster rows (cond 16..35, PS3 `0x4bc728`, PS4 `0x659bb0` = ret)

Our code: `session.cpp roster_query` (name, rank badge, talking / muted icon, team colour). The PS3
behaviour, with what is still missing marked **[missing]**:

1. **Snapshot.** The rows show a member list taken when the list is refreshed (op 8), not the live
   session: op 8 rebuilds it, every other op reads it. Row = `arg % 10` (cond 16..25 have args 0..9 on
   PAGE_MULTIPLAYER, cond 26..35 args 10..19 on PAGE_LOBBY). A row at or beyond the member count, or an
   empty entry, answers nothing. (Our live read is fine as long as the order below holds.)
2. **Order [missing].** The party session host is always row 0. The other members follow in session
   slot order, first those on the host's team, then the rest (members with no team count as the host's
   team when the host has none). Our `slot_of_row` uses plain slot order.
3. **Op 1, name.**
   - Every member: if the member's custom data carries a clan tag, the name is shown as
     `[TAG] name`, else just the name. **[missing]**: our member records carry no clan tag, and no
     tag is exchanged; the local tag already exists (`clantag.cpp`, NetPlayerData PS4 `0x11474f8+0x1a8`).
     Needed: send the tag with the member info (session messages kInfo / kRoster) and prefix it here.
   - Rows 10..19 (PAGE_LOBBY) additionally get the skill prefix (section 0 format) when **all** hold:
     page mode is 1, the selected playlist (leader's choice) is found among the playlists, it has a
     game list and a skill board (record `+0x2c` non-zero, `+0x30` not -1; PS4 record `+0x28` / `+0x34`).
     The number is the member's skill value on that board, rounded. Our playlists all have board -1,
     so this never shows on PC: **PLATFORM** (no rating service); keep it out unless a board is added.
4. **Op 3, images** (`+4` selects the picture):
   - 0: rank badge, type 0x27, value = rank byte. (covered)
   - 1: voice icon. Muted -> the muted icon; else the talking icon while the member's voice level is at
     least 1.0. (covered by `voice::talking` / `voice::muted`)
5. **Op 7 [missing, verify].** Answers 0 for the local player's own row and 1 for every other member.
   Our port answers 1 for all and the own-row callback opens a "You" message box. Expected effect on
   PS4: the own row cannot be selected. Check in game that a 0 does not hide the row before shipping.
6. **Op 10 [missing].** With selector 0 the result word is 1 for every listed member. With selector 1
   it is the member's connection state (0 connected, -1 none) when the member has a connection id.
   Our port answers nothing (writes 0 to `+4`). Field offsets on PS4: open question 2.
7. **Op 17, team colour.** Only for rows 10..19, only in team games (not co-op): team 1 / 2 -> that
   team's colour with the top byte forced to 0x20. (covered)

## 2. Matchmaking player rows (cond 6..15, PS3 `0x4bfac8`, PS4 `0x659bc0` = ret)

Our code: the same `roster_query` (party members only). On PS3 the rows show the **matchmaking slot
list** (PS3 global `0xf21de8`: up to 10 entries of 0x20 bytes, count at `+0x140`; entry `+0` kind,
`+4` voice port record, `+8` member, `+0xd` which session holds the member, `+0x10` u64 party id).
Row = arg (0..9). A row at or beyond the count answers nothing. Kinds: 0 open slot, 1 slot reserved
(player found, not in yet), 2 player present. With stranger matchmaking online (`online.cpp`, kind-3
rooms) this list has a PC source: the match room's slots. **PLATFORM, partly missing:**

1. **Op 1, text.** Kind 2: the name as in section 1.3 (clan tag; skill prefix in page mode 1 with a
   skill board). Kind 0: "Finding Player...". Kind 1: "Found Player". **[missing]** kinds 0 and 1: our
   rows are empty while the search runs.
2. **Op 3, images.**
   - 0: kind 2 -> rank badge (type 0x27, rank byte). Kind 0 -> a type 0x10 image with value 0 (the
     searching animation). **[missing]** kind 0.
   - 1: kind 2 with a voice record: muted icon, or talking icon at level >= 1.0 (covered).
   - 2: **[missing]** connection bars, type 0x1d, value 1..5 from the round-trip time to that player
     (up to 49 ms: 5, up to 79: 4, up to 119: 3, up to 199: 2, above: 1; 0 when unknown), colour word
     0x800000FF while the connection is up, 0x80808080 (grey) otherwise (values as PS3 wrote them;
     byte order on PS4 to check). Source on PC: `online.cpp` RTT per peer (LAN: our transport RTT).
3. **Op 10.** Selector 0: result 1 only for kind 2. **[missing]**
4. **Op 17 [missing], party group colour.** The player's party id is looked up in the matchmaking
   state's list of party ids (PS3 `+0x148`, count `+0x198`); found at position i -> row colour =
   group colour `i % 5` (five fixed colours; values not recovered, open question 3). Normally only
   kind-2 entries with a non-zero party id are coloured; while a matchmaking flag (PS3 `0xf2264c+0x80`)
   is set every entry is. Players who came in the same party share a colour. On PC the group = the
   room members' party (member data +0 = party room id, `online.cpp member_blob`).

## 3. Friends list (cond 36, PS3 `0x4c1fe0`, PS4 `0x65bbb0` = ret)

Our code: `menus.cpp query_friends` (online: title "Friends (n of m online)", row 0 "Browse Online
Games", then `online::friend_row` "name - status" and friend requests, all in column 0; LAN: the party).
PS3 (PSN friends) behaviour, **PLATFORM without a full equivalent**:

1. **Rows.** Every friend, sorted: friends playing this game first, then friends online in another
   game, then offline; within a group by online id (byte order, ascending). While the friend list is
   being fetched the list is empty (last index -1). No title (op 1 unanswered: the menu.bin label
   "Friends" shows).
2. **Selection** is kept by friend id, not by row: op 9 stores the row's friend id and the cursor; op 4
   answers the stored friend's current row (clamped to 0..count-1, -1 while fetching); op 8 refreshes
   every friend's presence, re-sorts, then finds the stored friend again, clamps the cursor and stores
   that row's id. So a refresh never moves the cursor to another friend.
3. **Op 2, column 0** = the friend's online id. **Column 3** = the status block (other columns blank):
   - The friend is not in this game's multiplayer (no presence data from this game, see section 5):
     if the friend runs this game (signed in to it, or the presence title equals ours) -> highlight
     line fd2de693, then "Playing Single-Player"; else if online -> "Playing another game" (plain);
     offline -> nothing.
   - In multiplayer: highlight line fd2de693, then (only when the friend's party has 2+ players)
     "[A] players in party" with A = party size and a line break, then by activity: lobby -> "In lobby",
     cinema -> "Watching a cinema", match -> "Playing match" plus a trailing line break.
4. **Op 3, images** (element id at `+0x50`):
   - `0x2138a9b2` avatar: PSN avatar picture (type 2) when loaded, else none. PC: none (RPCN has no
     avatars): COVERED by doing nothing.
   - `0xf5a294f3` status dot, type 0x20, value 2 if the friend is a member of our party, 1 if online,
     0 if offline. **[missing]**
   - element 0 (the row's default picture): in this game's multiplayer -> type 0x21 with value =
     presence level byte (at least 1); in this game but not multiplayer -> type 0x21 value 0; else none.
     **[missing]**; PS4 draw support of types 0x20 / 0x21: open question 4.
5. **PC differences to decide** (open question 5): our "Browse Online Games" row, the friend-request
   rows, the "(n of m online)" title and the single-column "name - status" text are not PS3. A parity
   version shows the PS3 rows above (name column 0, status column 3) and moves the extras elsewhere
   (requests could keep their rows after the friends, below a separator).
6. LAN (no RPCN) has no friends: the party list stays as it is (our design).

## 4. Friend card (cond 38, PS3 `0x4c1a08`, PS4 `0x65bbc0` = ret, PAGE_FRIENDACTIONS header)

PS3 opened PAGE_FRIENDACTIONS when a friend row was selected; its header is this query. On PC no
code opens that page: the row opens our popup (`remaining.cpp friend_options`, View Profile shows
`online::profile_text`). **PLATFORM without equivalent.** The card works on the friend stored by the
list's op 9; if that friend is no longer in the list, nothing is answered.

1. Op 1: the friend's online id (card title).
2. Op 2 row 0: the status headline: highlight text fd2de693 when the friend is in this game (MP
   presence, or the single-player case of 3.3); else "Playing another game" when online; else nothing.
3. Op 2 row 1 (only with MP presence and the friend playing this game): "[A] players in party" is
   prepared, then by activity: lobby -> "In lobby", cinema -> "Watching a cinema", match -> a line
   about the friend's game, read from the presence's net game index (`*net-games*` record; its map
   record gives the map name, its game type record the type name):
   - co-op types 8, 9 and 12: `- <map> / <type>`
   - co-op type 4: `- <map> / <type> / <difficulty>` (difficulty text from the presence's difficulty
     value 0..4)
   - every other type: `- <map> / <type> / <variation>` then a new line `- Match ends in [TIME]`
     (remaining time = presence end time minus now, at least 0, capped at 216000 clock units;
     nothing when the end time is 0)
4. Op 3: `0xb81e2263` avatar (none on PC); `0x0a071025` map picture, type 0x22, value = the map id of
   the friend's net game; `0x57135906` game type icon, type 3, the game type record's image.

A parity implementation either opens a card page with these fields or shows the same lines in our
profile popup (owner decision, open question 5).

## 5. Presence the friends list and card need (writer side)

`online.cpp PresenceData` (32 bytes) carries state, members, max, key, kind, room. The PS3 reader
needs, per friend, beyond "online / same game": activity (lobby, match, cinema), party size, a level
byte (row icon value), the net game index, the co-op difficulty, and the match end time, and it
accepts the data only when its leading signature equals our own build key (PS3 `0x4a9cd4`; ours:
`d.key == room_key()` plus magic and version). **[missing]**: extend PresenceData (new version
number; old versions still read as "in this game, no details") with: activity (add a cinema value to
kPresMenu..kPresMatch), level byte (our rank), net game index (s16, -1 none), difficulty (u8), match end
time (u64, same clock as now). Fill them in `presence_now`.

## 6. Join list (cond 3, PS3 `0x4b8ebc`, PS4 `0x659590` = ret)

Our code: `menus.cpp query_join_list` over `online.cpp join_rows / join_row` (invites first, then
found rooms "owner - kind (n/m)", or a "Searching..." / "No open games found" row). **PLATFORM,
covered.** PS3: rows exist only once a room search has finished with results; each row (column 0) is
the room's name text from the search result (the host's id); op 5 = count - 1; op 4 / op 9 keep the
cursor in one global; no title. The PS3 game showed invites in the NP Basic inbox, not here.
Differences (invites in the list, kind and member counts in the row, status rows) are PC choices:
open question 5.

## 7. Players met, invites, messages, voice (covered, for reference)

- Cond 39 (`0x4b8638`): column 0 = the player's online id, op 5 = count - 1 (-1 while fetching),
  op 8 = fetch again, op 4 / 9 = one global cursor. Ours: `menus.cpp query_players_met` (names from the
  progression store, op 8 reloads) + `remaining.cpp players_met_row`.
- Cond 48 (`0x4b8e08`): op 1 blank while the invite count (netInfo `+0x14`) is 0, op 2 column 1 = the
  count as a number. Ours: `remaining.cpp invites_query` with `online::invite_count()`.
- Cond 71 (`0x4b7a88`): the same with the message count (netInfo `+0x18`). The PS4 query is intact;
  `online.cpp` counts received cinema files there, `remaining.cpp cinema_receive` clears it.
- Cond 104 (`0x4b77ac`): `menus.cpp query_volume`.

## 8. Test scenarios

1. Party of 3, host in slot 2: PAGE_MULTIPLAYER row 0 is the host; custom team game: host's team
   first. Own row: not selectable (after the op 7 check), others open the popup.
2. Member with clan tag ABCD: every other PC shows `[ABCD] name` in party, lobby and player rows.
3. Online stranger matchmaking, 2 of 8 joined: rows 2..7 "Finding Player..." with the searching
   image; a reserved slot "Found Player"; joined players show rank, voice icon, 1..5 bars by RTT (grey
   when the connection drops) and the same row colour for players of one party.
4. Friends: A plays this game in a lobby with a party of 3, B in a match on a competitive map, C in
   another game, D offline, E in single player. Sorted A/B/E (this game, by id), C, D. A's column 3:
   headline, "3 players in party", "In lobby"; B: headline, "Playing match"; E: headline, "Playing
   Single-Player"; C "Playing another game"; D empty. Dots: party member 2, online 1, offline 0.
   Cursor on B, refresh while B moves ahead in the sort: the cursor follows B.
5. Friend card of B: title = B's id; line 2 = `- <map> / <type> / <variation>` and `- Match ends in`
   with the time left; map picture and game type icon shown.

## 9. Open questions

Status 2026-11-25: 1, 2, 3, 4, 6, 7 and 8 are settled in section 11; 5 stays open (owner decision).

1. English (and other) text of fd2de693 (the PS3 presence status title; probably the game's
   multiplayer title). Not in the PS4 or GOTY banks we import; needs an own text in `texts.cpp kOwn`.
2. PS4 field offsets of the op 10 selector and result, and whether PS4 still issues op 10 for rows
   (check the PS4 control method that issues op 10 among `0x65f4e0..0x660660`), and what a result of 1
   changes on screen.
3. The five party group colours of section 2.4 (PS3 keeps them in a table this decompile did not
   recover).
4. Whether the PS4 renderer `0x5bcd70` still draws types 0x10, 0x1d, 0x20, 0x21 (MENUS.md 7 lists only
   0x1e, 0x22..0x25, 0x27 as lost); if not, they need cases like menuimages.cpp's.
5. Owner decisions: keep the PC extras of the friends and join lists (section 3.5, 6), and card page vs
   popup (section 4).
6. Units of the match end time (PS3 clock `0x4e7cf0`) and of the 216000 cap; how [TIME] is formatted.
7. The matchmaking slot list writer (PS3 matchmaking machine `0x4b0a58`, outside this group): how many
   open slots it shows (the match size of the playlist?) and when a slot is "reserved".
8. Op 7 semantics on PS4 for roster rows (section 1.5): confirm 0 = not selectable, not hidden.

## 10. Status table

Status: **C** = covered (same visible result; PS4 code intact or our C++ named), **P** = PS3 platform
plumbing, replaced by our design (equivalent named, or "no equivalent"), **M** = missing behaviour
(section given). Every row's cond / arg comes from the PS3 query table.

| PS3 | role | status | where in our port |
|---|---|---|---|
| 4b7124 | query-control class: default virtual (answers 0) | C | PS4 control class `0x65f4e0..0x660660` |
| 4b712c | cond 64: empty query | C | PS4 `657ae0` (empty on both) |
| 4b7130 | cond 72..75: cinema render option row enabled (flag byte per arg) | C | PS4 intact |
| 4b7178 | query-control class: default virtual (answers -1) | C | PS4 control class |
| 4b7184 | cond 111..123: cinema render sliders (per-arg min / max / steps, value stored scaled) | C | PS4 `660760..` |
| 4b7294 | cond 124: slider -6..6 over a render float | C | PS4 intact |
| 4b732c | cond 125: slider -18..18 | C | PS4 intact |
| 4b73c4 | cond 126: slider 0..30 | C | PS4 intact |
| 4b745c | cond 127: slider 0..30 | C | PS4 intact |
| 4b74f8 | cond 95: EULA page query | C | PS4 `65e210` (page never shown) |
| 4b7650 | cond 76: cinema value / title query | C | PS4 intact |
| 4b7714 | query dispatcher: runs the entry only while NET_SM > 1, else blank text | C | PS4 helpers |
| 4b77ac | cond 104: voice volume slider | C | menus.cpp `query_volume` |
| 4b7a88 | cond 71: messages button count | C | PS4 intact; online.cpp netInfo+0x18; remaining.cpp |
| 4b7d10 | cond 5: enabled = is party host | C | PS4 `659560` |
| 4b8638 | cond 39: LIST_PLAYERSMET | C | menus.cpp `query_players_met`, remaining.cpp `players_met_row` |
| 4b8e08 | cond 48: BUTTON_MULTIPLAYER_INVITES count | C | remaining.cpp `invites_query` |
| 4b8ebc | cond 3: join game list (room search results) | P | menus.cpp `query_join_list`, online.cpp `join_rows/join_row` (sec. 6) |
| 4b9000 | index sort with explicit stack (cinema file list, cond 68) | C | PS4 cinema list (cinema.cpp area) |
| 4b992c | cinema entry order: by +0x314, then name | C | PS4 intact |
| 4b9a88 | cond 51..53, 131, 136: mode button labels | C | PS4 `655cf0`, texts.cpp |
| 4b9cb8 | query builder: finds the cond's table entry, sets op, arg, element | C | PS4 helpers |
| 4b9df4 | control method: op 17 | C | PS4 control class |
| 4b9e4c | control method: op 16, returns value | C | PS4 control class |
| 4b9ea0 | control method: op 15, returns value | C | PS4 control class |
| 4b9ef4 | control method: op 14, returns colour word | C | PS4 control class |
| 4b9f54 | control method: op 13, returns 8 descriptor words | C | PS4 control class |
| 4ba014 | control method: op 12, returns text or blank | C | PS4 control class |
| 4ba0c4 | control method: op 11, returns 6 words | C | PS4 control class |
| 4ba174 | control method: op 18, returns result word | C | PS4 control class |
| 4ba1d4 | control method: op 10, returns result word | C | PS4 control class |
| 4ba244 | control method: op 2 row text or blank | C | PS4 control class |
| 4ba2f4 | control method: op 5 last index | C | PS4 control class |
| 4ba348 | control method: op 6 | C | PS4 control class |
| 4ba3a4 | control method: op 9 set index | C | PS4 control class |
| 4ba400 | control method: op 4 current index | C | PS4 control class |
| 4ba454 | control method: op 3 image descriptor | C | PS4 control class |
| 4ba514 | control method: op 1 title, else the control's own label | C | PS4 control class |
| 4ba5a8 | control method: op 7 | C | PS4 control class |
| 4ba614 | control method: op 8 refresh | C | PS4 control class |
| 4ba7a0 | cond 105: rank widget (badge 0x27, progress 0x26 x1000, cash lines) | C | PS4 intact, menuimages.cpp 0x27 |
| 4baa4c | cond 79: results header | C | PS4 intact |
| 4bac10 | cond 102: controller layout image | C | PS4 intact |
| 4bacb4 | cond 46 / 94: map name of the current game in page mode = arg | C | PS4 intact |
| 4bad48 | cond 92: map picture in page mode 3 | C | PS4 intact |
| 4baef8 | cond 67: cinema mode label, enabled while files exist | C | PS4 intact (cinema.cpp) |
| 4bafa0 | cond 70: cinema list rows | C | PS4 intact (cinema.cpp) |
| 4bb170 | cond 69: cinema list rows (local / received) | C | PS4 intact (cinema.cpp) |
| 4bb4a0 | cond 59: custom game carousel | C | PS4 intact |
| 4bb684 | cond 58: custom game carousel | C | PS4 intact |
| 4bb898 | cond 57: custom game type carousel | C | PS4 intact |
| 4bc188 | cond 63: carousel shown when the game type has that setting | C | PS4 intact |
| 4bc310 | cond 97..101, 130, 137: page panel queries | C | PS4 `65ecc0` |
| 4bc600 | cond 45 / 93: game type name line | C | PS4 intact |
| 4bc728 | cond 16..35: party / lobby roster rows | C | session.cpp `roster_query` (order, own row, clan tag, op 10 done: sec. 12) |
| 4bcf6c | cond 60: carousel | C | PS4 intact |
| 4bd130 | cond 62: carousel | C | PS4 intact |
| 4bd2c4 | cond 61: carousel ("%i %s") | C | PS4 `656fe0` |
| 4bd9c8 | cond 55: PLAYLIST (page mode 1) | C | menus.cpp `query_playlist` |
| 4bdc30 | cond 134: clan tag label | C | PS4 `6609e0`, texts 639e0fa8 |
| 4bddd4 | cond 133: profile panel (stats, cash, badge, booster pair) | C | menus.cpp `query_profile` + PS4 + menuimages.cpp |
| 4be174 | cond 91: cash title | C | PS4 intact |
| 4be254 | cond 65: co-op carousel (game type 4 only) | C | PS4 intact |
| 4be434 | cond 88: scoreboard header widget | C | PS4 `65b490` |
| 4be6e4 | static init of this file's menu state | C | PS4 initialises its own |
| 4be774 | static-init entry of 4be6e4 | C | PS4 |
| 4be788 | cond 77 / 78: booster lists | C | PS4 `659bd0` |
| 4bf0b8 | cond 80: results | C | PS4 intact |
| 4bf768 | cond 90: matchmaking vote list | C | PS4 `65ae50`, data frontend3.cpp |
| 4bfac8 | cond 6..15: matchmaking player rows | P | session.cpp `mm_roster_query` (slot list, placeholders, bars, group colour: sec. 12) |
| 4c0018 | menu object constructor (no visible behaviour of its own) | C | PS4 class code |
| 4c0b40 | cond 138: on / off setting (NetPlayerData +0x1cc) | C | PS4 `65a660` |
| 4c0c78 | cond 82: results | C | PS4 intact |
| 4c0ea8 | cond 40 / 41: preferred hero / villain lists | C | PS4 `6595a0` |
| 4c1190 | cond 66: custom carousel (not co-op, page mode 2) | C | PS4 intact |
| 4c1a08 | cond 38: friend card header (PAGE_FRIENDACTIONS) | P | menus.cpp `query_friend_card`, View Profile via `friend_card_lines` (sec. 12) |
| 4c1fe0 | cond 36: LIST_FRIENDS | P | menus.cpp `query_friends`, online.cpp `friend_view` / presence version 2 (sec. 12) |
| 4c29f8 | mailbox clear (count 0) | P | session.cpp messages |
| 4c2a18 | mailbox peek / take by code | P | session.cpp / frontend3.cpp messages |
| 4c2b70 | static init (mailbox global = -1) | P | session.cpp |
| 4c2bac | static-init entry of 4c2b70 | P | session.cpp |

Counts (81 rows): C 72, C + M 1 (4bc728), P 6 (4b8ebc, 4c1a08, 4c29f8, 4c2a18, 4c2b70, 4c2bac),
P + M 2 (4bfac8, 4c1fe0). Behaviour to implement: sections 1 to 5 (4c1a08 has no PC equivalent; the
presence writer of section 5 is outside this group but feeds sections 3 and 4).

## 11. Clarifications (2026-11-25)

Answers to section 9. Source types: **PS3** = PS3 1.10 EBOOT (Ghidra headless, read-only, and raw data
reads), **PS4** = PS4 1.02 eboot.elf (llvm-objdump; addresses are image addresses), **bank** = the game's
own text banks (PS4 1.02 and GOTY PS3, as unpacked for the import), **hint** = a fact first seen in the
third-party reference notes and then checked in the binary named. Behaviour only; no game code or tables
are reproduced beyond the interop values listed.

### 11.1 Text fd2de693 (question 1): settled (bank, PS3)

- It is the game's **title text**, and it is in the **common** bank (not networking) of every PS4 1.02
  language (all 22 banks have it) and of the GOTY PS3 banks. PS4 English: "Uncharted 2: Among Thieves™
  Remastered"; GOTY PS3 English: "Uncharted 2: Among Thieves™"; other languages carry their local title.
- PS3 use: the friends list and the friend card (and three friend-list helpers, PS3 `0x48f16c`,
  `0x490338`, `0x490a88`) show it as the highlighted headline "this friend is playing this game".
- No own text is needed: the text hook already returns the game's own text first (TEXTS.md 2, step 1).
  Section 0's "not in our banks" was wrong (only the networking bank had been checked). If the owner
  prefers the PS3 wording without "Remastered", that is a kOwn override and an owner choice, not parity.

### 11.2 Op 10 on PS4 (question 2): settled (PS4, PS3; first located by hint)

- Control method: PS4 `0x65fe20` (vtable `0x100b430` slot +0x60) = PS3 `0x4ba1d4`. It takes two
  arguments besides the control: the first goes to query `+0x04` (a sub index), the second to the
  selector field (PS3 `+0x38`, **PS4 `+0x4c`**); it returns the result word (PS3 `+0x28`, **PS4 `+0x38`**).
  The table arg on PS4 is a 32-bit word at `+0x48`; `+0x4c` right after it is the selector, so a 64-bit
  read of `+0x48` mixes the two (harmless only while the selector is 0).
- PS4 still issues op 10. The party-row control class (vtable `0x10079a0`, installed by the control
  factory at `0x5c6416`) has as its "can be selected" check (`+0x40`, `0x590040`) a call of op 10 with sub
  index 0 and selector 0, and it is selectable only when the result is non-zero. Every focus move asks
  this check first (focus setting and the up / down / left / right moves, `0x5a1960..0x5a1d80`).
- What a result of 1 changes: the row can take the cursor. With no answer (result 0) the cursor skips
  every party / lobby row, so the rows can never be selected. Section 1.6 (selector 0: 1 for every
  listed member) and 2.3 (selector 0: 1 only for kind 2) are what make rows selectable.
- Selector 1 (connection state, section 1.6): no PS4 menu call site in `0x550000..0x670000` passes 1 as
  the selector (callers pass 0 or a property id); implementing it is harmless but has no PS4 consumer.
  PS3 answers it with 0 when the member's signaling connection status can be read (PS3 import
  `sceNpSignalingGetConnectionStatus`), -1 when there is no active connection record.

### 11.3 Party group colours (question 3): settled (PS3 data read)

Five row colour words, picked by the party's position in the party-id list modulo 5 (interop values):

| index | word | colour, reading the top byte as a faint alpha (like the team colours' 0x20) |
|---|---|---|
| 0 | `0x1020E0E0` | cyan |
| 1 | `0x102000E0` | blue |
| 2 | `0x10E000E0` | magenta |
| 3 | `0x1020E000` | green |
| 4 | `0x10E02000` | orange-red |

Write them as the same 32-bit values at PS4 `+0x3c`, the way our team colour already writes
`(colour & 0xffffff) | 0x20000000` (the PS3 team colour code does the same with 0x20). The channel order
of PS4's row draw was not traced; the colour names are a reading, check in game. Two facts limit who is
coloured (PS3 slot-list writer, 11.6): only party ids shared by **at least two** listed entries enter the
party-id list, so a lone player is never coloured; our own party's members carry the party id only while
the party has 2 or more members.

### 11.4 PS4 renderer and image types 0x10, 0x1d, 0x20, 0x21 (question 4): settled (PS4)

The PS4 renderer `0x5bcd70` dispatches types 1..7 through a first jump table and types 4..0x29 through
the second (`0x5c2ec4`). In the second table the entries that go straight to the exit `0x5c1ab1` are
0x07, 0x15, 0x1e, 0x22, 0x23, 0x24, 0x25 and 0x27 (MENUS.md 7 listed all of these but 0x15, which no
query of this group produces). **Types 0x10, 0x1d, 0x20 and 0x21 still have their drawing cases**, so no
menuimages.cpp case is needed for them:

- 0x10 (`0x5bf21e`): the searching animation (time-driven).
- 0x1d (`0x5c0eba`): value 0 or less draws nothing; else five bars, the lit count from the value, in the
  descriptor's colour word (modulated by the control's colour). The word's bytes feed the colour as
  (bits 16-23, 8-15, 0-7, 24-31) / 255 with the top byte as alpha, so `0x800000FF` / `0x80808080` keep
  their PS3 meaning when written as the same 32-bit values at `+0x2c`.
- 0x20 (`0x5c1202`, friend status dot): value 2 picks one picture (`0xd2bc74ef`), other values another
  (`0x9cce2b1d`); value 0 picks one tint id (`0x3ca691f3`), non-zero values another (`0x17821399`).
- 0x21 (`0x5c12c9`, presence row icon): a value above 3 picks picture `0x6af2af2f`, else `0xe5bdffbd`.
- The 0x20 / 0x21 pictures are looked up inline in the net picture table and then through `0x5558c0`,
  the function menuimages.cpp replaces whole (the `:` to `/` name fallback), so they resolve like the
  other net pictures. Whether these four ids exist in net.pak was not checked: the menu images log
  ("missing") shows it on the first friends-list draw.

Also for section 2.2 (PS3 `0x4bfac8`): the row code has four colour outcomes for the bars, but the
connection-state function it asks returns only 0 (connection up) or -1, so only `0x800000FF` (up) and
`0x80808080` (otherwise) can appear, as 2.2 says. The bar value comes from the peer's round-trip time in
its connection record, thresholds as in 2.2; 0 when the player has no connection record.

### 11.5 Match end time and [TIME] (question 6): settled (PS3, PS4, bank)

- Clock (PS3 `0x4e7cf0(offset)`): PSN network time plus `offset` seconds, converted to a Unix time in
  **seconds** (imports `sceNpManagerGetNetworkTime`, `cellRtcTickAddSeconds`, `cellRtcSetTick`,
  `cellRtcGetTime_t`). The presence end time uses the same unit. PC equivalent: UTC Unix seconds.
- Remaining time = end - now in seconds, 0 when negative, capped at 216000 s (60 hours); nothing is
  shown when the end time is 0.
- [TIME] format (PS3 `0x929df8`; **PS4 keeps the same formatter at `0xcddfe0`**, arguments: text db,
  output, size, text id, seconds): hours = whole hours **modulo 24** (days are dropped), minutes = whole
  minutes modulo 60, seconds are not shown.
  - hours and minutes both 0: text `20ae4825` ("less than 1 minute");
  - only hours: `"%d %s"` with `669b8ea8` ("hour") when 1, else `5879aefb` ("hours");
  - only minutes: `"%d %s"` with `9d59ec8a` ("minute") when 1, else `38e8ba93` ("minutes");
  - both: `"%d %s %d %s"`, hours first.
  The result replaces the token `[TIME]` in `93ac6446` ("Match ends in [TIME]"). All five unit texts
  are in every PS4 common bank, so calling the PS4 formatter gives the right text in each language.

### 11.6 Matchmaking slot list writer (question 7): settled (PS3)

Writer: PS3 `0x4b0008(list, sort)`, called by the matchmaking machine `0x4b0a58` on each of its update
calls (sort off), and by a lobby page handler (sort on, for its team grouping; that sorted copy is
rebuilt unsorted right after, so rows never show the sorted order). Each build:

1. Clears the list, then adds **our party's members** in party session slot order, all as kind 2
   ("present"), marked as party-session members. Their party id is our party session's id, but only
   while the party has 2 or more members; else 0.
2. Adds the **match session's members** that are not already listed (compared by online id), in
   session slot order, up to 10 entries in all. Their party id comes from their published member data
   (`+0`, u64). Their kind depends on the matchmaking progress value (a word the machine sets: 2 when
   the match set-up message arrives, 3 when the match room is joined; lower while searching):
   progress 0 or 1: kind 1 "Found Player" (**reserved** = found and in the forming match session, the
   match not yet set up); progress 2 or more: kind 2 (present).
3. Counts, per entry, how many listed entries share its non-zero party id.
4. **Open slots** (kind 0 "Finding Player...") are added only while the progress value is 0 or 1: the
   list is filled up to the selected playlist's maximum player count (playlist record `+0x18`, the value
   PS3 `0x49a958` returns), capped at 10; none when the playlist is not found.
5. Each entry gets its voice record from the member's connection id.
6. Rebuilds the party-id list for row colours: every distinct non-zero party id held by 2 or more
   listed entries, in list order (position = colour index of 11.3).

### 11.7 Op 7 on PS4 (question 8): settled (PS4, PS3; press path first located by hint)

- PS4 control method `0x65f5d0` (slot +0xc0) and PS3 `0x4ba5a8` behave the same: the query is built with
  `+0x04` preset to 1 and the method returns "answer is non-zero". So an unanswered op 7 means enabled.
- Effect of 0 ("not enabled"): the control **stays visible and can take the cursor** (focus uses the
  op 10 check of 11.2, not op 7); while it has the cursor, button presses reach it with the button value
  cleared (input dispatch around `0x5a3813`), so CROSS does nothing; its label is drawn with a fixed
  fallback style instead of the theme's normal style (`0x57a910`; how it looks: check in game). No PS4
  path was found that hides a control for op 7 = 0. Other op 7 readers (`0x57eaeb`, `0x586fac`,
  `0x58841b`, `0x593c13`, `0x59fe86`, `0x5a3705`, `0x5a4edf`) were not analysed.
- PS3 answered 0 for the local player's own roster row (member id equals the session's own id) and 1 for
  the others, so on PS3 the cursor could pass over the own row but pressing it did nothing. Our "You"
  message box on the own row is a PC difference, not PS3 behaviour.

### 11.8 Not settled

- Question 5 (keep the PC extras of the friends and join lists; card page or popup): owner decision.
- The channel order of row colour words (11.3) and the exact look of a not-enabled label (11.7): check
  in game.
- Whether the matchmaking rows (cond 6..15) use the same row control class as the party rows (so that
  11.2's op 10 answer makes present players selectable there too): not traced.

## 12. Implemented (2026-11-25)

Owner decisions: the PC extras of the friends and join lists stay (title with the online count, "Browse Online
Games", friend request rows, the join list's invites / status rows and "(n/m)"); the friend card shows in View
Profile (no PAGE_FRIENDACTIONS page); no skill prefix (no board). Where each item lives:

| item | status | where |
|---|---|---|
| 1.2 row order | done | session.cpp `slot_of_row` (also `member_at`, so the row popups and the LAN friends list follow it) |
| 1.3 / 2.1 clan tags | done | session.cpp: kInfo carries the tag after the skins, new message kTags (11) after every kRoster (older peers ignore it), `shown_name` "[TAG] name" in party, lobby and matchmaking rows |
| 1.5 op 7 | done | own row 0, others 1; remaining.cpp no longer opens the "You" box (a press that arrives does nothing) |
| 1.6 / 2.3 op 10 | done | selector at +0x4c, result at +0x38; table arg read as 32 bits |
| 2 matchmaking rows | done | session.cpp `mm_roster_query` / `mm_list` (11.6), online.cpp `in_own_party`, `own_party_id`, `member_party_id` (member data +0 kept per room member), `peer_rtt`, `mm_slots`; `mm_member_at` for the row popup |
| 3 friends list | done | menus.cpp `query_friends_online`, online.cpp sorted `friend_list`, `friend_view`, `friend_index` |
| 4 friend card | done, partly | menus.cpp `query_friend_card` (cond 38 stub replaced) and `friend_card_lines` (View Profile); the co-op difficulty text and the variation are left out: their text ids / net game record field are not in this spec |
| 5 presence | done | online.cpp PresenceData version 2 (48 bytes, within the ABI's 128), version 1 still read; invites of 32 bytes still taken |

PC differences that remain: "Finding Player..." / "Found Player" rows only with stranger matchmaking online (LAN
gathers the party only); the open-slot count is the matchmaking room's size (10 on PC rooms, PS3 the playlist
maximum, which our playlists do not carry); connection bars only online (the LAN transport keeps no round-trip
time); the "matchmaking flag" that colours every row (2.4) is not modelled (only present players are coloured).
Older builds reject a version 2 presence (they check version 1), so to them our friends show no Join Party.
