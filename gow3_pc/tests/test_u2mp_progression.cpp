// u2mp progression store test (no game needed): ninja -C out/gpu u2mp-progression-test
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../src/u2mp/progression_store.h"

using namespace u2mp::progression;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

static Store sample() {
    Store s;
    s.publish_count = 7;
    s.has_blob = true;
    for (size_t i = 0; i < kBlobSize; ++i) s.blob[i] = uint8_t(i * 7 + 3);
    s.blob[0] = 10, s.blob[1] = s.blob[2] = s.blob[3] = 0;
    s.local_name = "Drake";
    PlayerStats p;
    p.name = "Chloe";
    p.kills = 12, p.deaths = 3, p.wins = 2, p.losses = 1, p.seconds = 900, p.coop_points = 0, p.money = 4000;
    p.matches = 3, p.rank = 5, p.last_seen = 1700000000;
    s.recent.push_back(p);
    return s;
}

static void test_roundtrip() {
    const Store s = sample();
    const std::vector<uint8_t> bytes = encode(s, 1234);
    Store d;
    CHECK(decode(bytes.data(), bytes.size(), d));
    CHECK(d.publish_count == 7 && d.has_blob && d.blob == s.blob && d.local_name == "Drake");
    CHECK(d.recent.size() == 1 && d.recent[0].name == "Chloe" && d.recent[0].kills == 12 &&
          d.recent[0].money == 4000 && d.recent[0].rank == 5 && d.recent[0].last_seen == 1700000000);
    // any flipped byte is rejected (CRC), as are short files and a wrong magic
    for (size_t i : {size_t(0), size_t(9), size_t(0x40), bytes.size() / 2, bytes.size() - 1}) {
        std::vector<uint8_t> bad = bytes;
        bad[i] ^= 0x40;
        Store x;
        CHECK(!decode(bad.data(), bad.size(), x));
    }
    Store x;
    CHECK(!decode(bytes.data(), bytes.size() - 1, x));
    CHECK(!decode(bytes.data(), 16, x));
}

// Format 2 keeps the local career headshots; a format 1 file (no headshots word) still reads, with 0.
static void test_headshots() {
    Store s = sample();
    s.headshots = 41;
    const std::vector<uint8_t> bytes = encode(s, 5);
    Store d;
    CHECK(decode(bytes.data(), bytes.size(), d) && d.headshots == 41 && d.recent.size() == 1);
    std::vector<uint8_t> old(bytes.begin(), bytes.end() - 8);   // without the headshots word and the CRC
    old[8] = 1, old[9] = old[10] = old[11] = 0;
    const uint32_t crc = crc32(old.data(), old.size());
    for (int i = 0; i < 4; ++i) old.push_back(uint8_t(crc >> (8 * i)));
    Store o;
    CHECK(decode(old.data(), old.size(), o) && o.headshots == 0 && o.recent.size() == 1 && o.local_name == "Drake");
    // the local line adds its headshots; the other players' lines do not
    std::vector<MatchPlayer> m(2);
    m[0].name = "Drake", m[0].local = true, m[0].headshots = 3;
    m[1].name = "Chloe", m[1].headshots = 9;
    note_match(s, m, 300);
    CHECK(s.headshots == 44);
}

static void test_files() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "u2mp-progression-test";
    fs::remove_all(dir);
    const std::string path = (dir / "sub" / "progression.bin").string();
    Store s = sample();
    Store none;
    CHECK(!load(path, none));
    CHECK(save(path, s, 1));          // creates the directories
    CHECK(fs::exists(path) && !fs::exists(path + ".tmp"));
    s.publish_count = 8;
    CHECK(save(path, s, 2));          // previous file becomes .bak
    CHECK(fs::exists(path + ".bak"));
    s.publish_count = 9;
    CHECK(save(path, s, 3));          // .bak replaced again
    Store l;
    CHECK(load(path, l) && l.publish_count == 9);
    // a damaged main file falls back to the previous one
    {
        FILE *f = std::fopen(path.c_str(), "r+b");
        CHECK(f);
        if (f) {
            std::fseek(f, 0x50, SEEK_SET);
            std::fputc(0xee, f);
            std::fclose(f);
        }
    }
    Store b;
    CHECK(load(path, b) && b.publish_count == 8);
    fs::remove_all(dir);
}

static void test_matches() {
    Store s;
    std::vector<MatchPlayer> m(3);
    m[0].name = "Drake", m[0].local = true, m[0].kills = 5;
    m[1].name = "Chloe", m[1].kills = 7, m[1].deaths = 2, m[1].result = 1, m[1].money = 1500, m[1].seconds = 600;
    m[2].name = "Flynn", m[2].kills = 1, m[2].deaths = 9, m[2].result = -1, m[2].rank = 3;
    note_match(s, m, 100);
    note_match(s, m, 200);
    CHECK(s.local_name == "Drake");
    CHECK(s.recent.size() == 2);
    const PlayerStats *chloe = nullptr;
    for (const PlayerStats &p : s.recent)
        if (p.name == "Chloe") chloe = &p;
    CHECK(chloe && chloe->kills == 14 && chloe->wins == 2 && chloe->losses == 0 && chloe->matches == 2 &&
          chloe->money == 3000 && chloe->seconds == 1200 && chloe->last_seen == 200);
    // capped at kMaxRecent, most recent kept
    for (int i = 0; i < 100; ++i) {
        std::vector<MatchPlayer> one(1);
        one[0].name = "P" + std::to_string(i);
        one[0].kills = 1;
        note_match(s, one, 1000 + uint64_t(i));
    }
    CHECK(s.recent.size() == kMaxRecent);
    CHECK(s.recent.front().name == "P99");
    // the local player is never listed as a remote one
    std::vector<MatchPlayer> again(1);
    again[0].name = "Drake", again[0].local = true;
    s.recent.push_back(PlayerStats{"Drake"});
    note_match(s, again, 5000);
    for (const PlayerStats &p : s.recent) CHECK(p.name != "Drake");
}

// Players met at match / round start (spec parity-e-flow2.md 2.9, parity-g-friends.md G1): new names come in
// without statistics, a player known only from a match's end is listed with its statistics kept, the local
// player never; the Leave statistics add up later.
static void test_met() {
    Store s;
    s.local_name = "Drake";
    std::vector<MatchPlayer> m(1);
    m[0].name = "Chloe", m[0].kills = 3;
    note_match(s, m, 100);   // a late joiner: statistics, but no Players Met row
    CHECK(s.recent.size() == 1 && s.recent[0].unlisted);
    CHECK(players_met(s, nullptr).empty());
    CHECK((note_met(s, {"Sully", "Chloe", "Drake", ""}, 200, nullptr) == std::vector<std::string>{"Sully", "Chloe"}));
    CHECK(s.recent.size() == 2);
    const PlayerStats *sully = nullptr, *chloe = nullptr;
    for (const PlayerStats &p : s.recent) (p.name == "Sully" ? sully : chloe) = &p;
    CHECK(sully && sully->matches == 0 && sully->kills == 0 && sully->last_seen == 200 && !sully->unlisted);
    CHECK(chloe && chloe->matches == 1 && chloe->kills == 3 && chloe->last_seen == 200 && !chloe->unlisted);
    // already listed: skipped, last_seen unchanged (a new round adds nobody)
    CHECK(note_met(s, {"Sully"}, 250, nullptr).empty());
    CHECK(sully && sully->last_seen == 200);
    note_match(s, m, 300);
    for (const PlayerStats &p : s.recent)
        if (p.name == "Chloe") CHECK(p.matches == 2 && p.kills == 6 && p.last_seen == 300 && !p.unlisted);
    CHECK((players_met(s, nullptr) == std::vector<std::string>{"Chloe", "Sully"}));   // most recently seen first

    // the unlisted flag survives the file (old files hold 0 there: listed)
    Store d;
    const std::vector<uint8_t> bytes = encode(s, 1);
    note_match(s, std::vector<MatchPlayer>(1, MatchPlayer{"Flynn"}), 400);
    const std::vector<uint8_t> bytes2 = encode(s, 1);
    CHECK(decode(bytes2.data(), bytes2.size(), d));
    for (const PlayerStats &p : d.recent) CHECK(p.unlisted == (p.name == "Flynn"));
    CHECK(decode(bytes.data(), bytes.size(), d) && d.recent.size() == 2);
}

// G1: friends never appear and are never added; a stranger who became a friend leaves the rows; batches of 16;
// 50 rows at most.
static void test_met_friends() {
    Store s;
    s.local_name = "Drake";
    std::vector<std::string> friends = {"Elena"};
    const IsFriend is_friend = [&](const std::string &n) {
        return std::find(friends.begin(), friends.end(), n) != friends.end();
    };
    CHECK((note_met(s, {"Elena", "Lazarevic"}, 100, is_friend) == std::vector<std::string>{"Lazarevic"}));
    CHECK(s.recent.size() == 1);
    // the friend's Leave statistics are kept (leaderboards), unlisted
    std::vector<MatchPlayer> m(2);
    m[0].name = "Elena", m[0].kills = 4;
    m[1].name = "Lazarevic", m[1].kills = 9;
    note_match(s, m, 150);
    CHECK(s.recent.size() == 2);
    CHECK((players_met(s, is_friend) == std::vector<std::string>{"Lazarevic"}));
    friends.push_back("Lazarevic");   // befriended: gone at the next refresh
    CHECK(players_met(s, is_friend).empty());
    friends = {};                      // Elena unfriended: listed by the next match start, not before
    CHECK((players_met(s, is_friend) == std::vector<std::string>{"Lazarevic"}));
    CHECK((note_met(s, {"Elena", "Lazarevic"}, 200, is_friend) == std::vector<std::string>{"Elena"}));
    CHECK((players_met(s, is_friend) == std::vector<std::string>{"Elena", "Lazarevic"}));

    // at most 16 per batch, in order; duplicates once
    std::vector<std::string> many;
    for (int i = 0; i < 20; ++i) many.push_back("P" + std::to_string(i));
    many.insert(many.begin(), "P0");
    const std::vector<std::string> batch = note_met(s, many, 300, is_friend);
    CHECK(batch.size() == kMetBatch && batch.front() == "P0" && batch.back() == "P15");
    // 50 rows at most, most recent first
    for (int i = 0; i < 60; ++i) note_met(s, {"Q" + std::to_string(i)}, 1000 + uint64_t(i), is_friend);
    const std::vector<std::string> rows = players_met(s, is_friend);
    CHECK(rows.size() == kMetShown && rows.front() == "Q59" && rows.back() == "Q10");
    // a stored player past the 50 rows is no row: a match start lists it again at the top
    CHECK((note_met(s, {"Q5"}, 2000, is_friend) == std::vector<std::string>{"Q5"}));
    CHECK(players_met(s, is_friend).front() == "Q5");
}

static void test_boards() {
    Store s;
    for (int i = 0; i < 30; ++i) {
        PlayerStats p;
        p.name = "P" + std::to_string(i);
        p.kills = uint32_t(i);   // P0 has no kills: not on the kills board
        p.money = uint32_t(1000 * i);
        s.recent.push_back(p);
    }
    LocalStats me;
    me.name = "Drake";
    me.kills = 15, me.money = 999999, me.deaths = 4, me.losses = 2, me.seconds = 100, me.coop_seconds = 50, me.rank = 9;
    const std::vector<Entry> kills = board(s, me, 0);
    CHECK(kills.size() == 30);   // 29 remote with kills + local
    for (size_t i = 1; i < kills.size(); ++i) CHECK(kills[i - 1].score >= kills[i].score);
    // P15 and Drake tie at 15: same rank, name order
    size_t at = 0;
    for (size_t i = 0; i < kills.size(); ++i)
        if (kills[i].local) at = i;
    CHECK(kills[at].score == 15 && kills[at].badge == 9 && kills[at].seconds == 150 && kills[at].deaths == 4);
    CHECK(kills[at].name == "Drake" && kills[at + 1].name == "P15" && kills[at + 1].rank == kills[at].rank);
    CHECK(kills[0].rank == 1 && kills[0].name == "P29");
    const std::vector<Entry> cash = board(s, me, 1);
    CHECK(cash[0].local && cash[0].rank == 1);
    const std::vector<Entry> coop = board(s, me, 3);
    CHECK(coop.size() == 1 && coop[0].local && coop[0].seconds == 50);
    CHECK(board(s, me, 4).empty() && board(s, me, -1).empty());

    // pages of 11; -1 = the local player's page
    const Page mine = page(kills, -1);
    CHECK(mine.total == 30 && mine.page == int(at / kRowsPerPage));
    bool found = false;
    for (const Entry &e : mine.rows) found = found || e.local;
    CHECK(found);
    const Page last = page(kills, 99);
    CHECK(last.page == 2 && last.rows.size() == 8);
    const Page first = page(kills, 0);
    CHECK(first.page == 0 && first.rows.size() == 11 && first.rows[0].name == "P29");
    CHECK(page({}, -1).total == 0);
}

int main() {
    // CRC-32 check value
    const char *check = "123456789";
    CHECK(crc32(reinterpret_cast<const uint8_t *>(check), 9) == 0xcbf43926u);
    test_roundtrip();
    test_headshots();
    test_files();
    test_matches();
    test_met();
    test_met_friends();
    test_boards();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("u2mp progression store: all tests passed\n");
    return 0;
}
