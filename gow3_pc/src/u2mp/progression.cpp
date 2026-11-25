// u2mp progression: keeps the multiplayer career (NetPlayerData Data) across sessions and gives the
// leaderboard page something to show.
//
// The PS4 1.02 build holds the NetPlayerData Data block in RAM only: nothing loads it, nothing saves it
// and its state never reaches "connected". The leaderboard list no longer fetches rows either. This
// file takes over three places in the game:
//   - the player info update (0x64e810): loads the Data block from a local store file, marks the
//     player data connected and writes the block back whenever it changed;
//   - the DcLookup call in Leave::Enter (0x5f0952): writes the block after the match stats were added,
//     remembers who was in the match and posts the scores online when the online link is up;
//   - the leaderboard list update (0x5b3000): fills the game's leaderboard object with rows from the
//     online score service when linked, otherwise from the local store.
// The store file format and the board maths live in progression_store.cpp.
//
// Reimplemented from the behaviour description u2mp/docs/spec/progression.md.
#include "progression.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "online.h"
#include "progression_store.h"
#include "u2mp_internal.h"
#include "unlocks.h"

extern "C" const char *runtime_file_user_dir(void);

namespace u2mp {

namespace {

namespace store_api = u2mp::progression;
using store_api::kBlobSize;

// ---------------------------------------------------------------------------------------------
// Game image offsets (PS4 1.02).

constexpr uint64_t kNetSmState = 0x14ce97c;      // s32: > 1 means the network state machine is online
constexpr uint64_t kNpdState = 0x1147bb8;        // s32: NetPlayerData state (3 connected, 2 offline)
constexpr uint64_t kNpdData = 0x114753c;         // the 0x228-byte Data block
constexpr uint64_t kNpdCash = 0x11475d0;
constexpr uint64_t kNpdKills = 0x114767c;
constexpr uint64_t kNpdDeaths = 0x1147680;
constexpr uint64_t kNpdWins = 0x1147684;
constexpr uint64_t kNpdLosses = 0x1147688;
constexpr uint64_t kNpdSeconds = 0x1147690;
constexpr uint64_t kNpdCoopPoints = 0x1147694;
constexpr uint64_t kNpdCoopSeconds = 0x1147698;
constexpr uint64_t kNpdPublishCount = 0x114769c;

constexpr int32_t kConnected = 3;
constexpr int32_t kOffline = 2;
constexpr uint32_t kDataVersion = store_api::kBlobVersion;

// NetStatGame: the tallies of the match being left.
constexpr uint64_t kMatchPlayerCount = 0x1147bd8;
constexpr uint64_t kMatchRecords = 0x114b4bc;     // stride 0x80
constexpr uint64_t kRecordStride = 0x80;
constexpr uint64_t kTeamTallies = 0x114b9bc;      // stride 0x34; u16 +0 won marker, u16 +2 score
constexpr uint64_t kPlayerTallies = 0x114ba24;    // stride 0x34
constexpr uint64_t kTallyStride = 0x34;
constexpr uint32_t kMaxMatchPlayers = 10;

constexpr uint64_t kBoardTable = 0xdef0a0;        // 4 entries of 12 bytes: board id, column set
constexpr uint64_t kBoardStride = 12;
constexpr uint64_t kNetGameIndex = 0x113d400;     // s16
constexpr uint64_t kMatchmakingFlag = 0x104f1fc;  // s32
constexpr uint64_t kLeaderboard = 0x113d960;      // the leaderboard object

// Leaderboard object fields.
constexpr uint64_t kLbTotal = 0x24;
constexpr uint64_t kLbRows = 0x28;
constexpr uint64_t kLbInfo = 0x30;
constexpr uint64_t kLbStatus = 0x50;
constexpr uint64_t kLbBusy = 0x58;
constexpr uint64_t kLbRowCount = 0x70;
constexpr uint64_t kLbPage = 0x74;
constexpr uint64_t kLbReset = 0x78;
constexpr uint64_t kLbBoard = 0x80;
constexpr uint64_t kLbFriends = 0x84;
constexpr uint64_t kLbRefresh = 0x85;

// List object fields (the argument of the list update).
constexpr uint64_t kListColumns = 0x40;
constexpr uint64_t kListDelta = 0x44;
constexpr uint64_t kListPage = 0x4c;
constexpr uint64_t kListBoard = 0x50;
constexpr uint64_t kListFriends = 0x54;

// Guest functions and DC symbols.
constexpr uint64_t kDcLookup = 0xa15e70;
constexpr uint32_t kRankTableSid = 0xc85e199d;
constexpr uint32_t kNetGamesSid = 0x3b9a067d;

// Hooked places.
constexpr uint64_t kPlayerInfoUpdate = 0x64e810;
constexpr uint64_t kLeaveCallCheck = 0x5f094d;
constexpr uint64_t kLeaveCall = 0x5f0952;
constexpr uint64_t kListUpdate = 0x5b3000;

constexpr size_t kPageRows = store_api::kRowsPerPage;   // 11
constexpr size_t kRankRecord = 0x80;
constexpr size_t kInfoRecord = 0x40;

// ---------------------------------------------------------------------------------------------
// Little-endian memory access (guest data is not necessarily aligned).

template <typename T> T load_at(const uint8_t *p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
template <typename T> void store_at(uint8_t *p, T v) { std::memcpy(p, &v, sizeof v); }

uint8_t *game(uint64_t offset) { return image.base + offset; }
template <typename T> T game_get(uint64_t offset) { return load_at<T>(game(offset)); }
template <typename T> void game_set(uint64_t offset, T v) { store_at<T>(game(offset), v); }

uint8_t *lb() { return game(kLeaderboard); }

void *dc_lookup(uint32_t sid) {
    return reinterpret_cast<void *(U2MP_GUEST *)(uint32_t)>(game(kDcLookup))(sid);
}

// ---------------------------------------------------------------------------------------------
// Module state.

store_api::Store g_store;
bool g_store_tried = false;
uint8_t g_last_published[kBlobSize] = {};
uint32_t g_last_match = 0;

alignas(16) uint8_t g_rank_rows[kPageRows * kRankRecord];
alignas(16) uint8_t g_info_rows[kPageRows * kInfoRecord];

// The online leaderboard fetch in flight.
enum FetchStage { kIdle = 0, kFindMyPage = 1, kRange = 2, kFriends = 3 };
int g_fetch_stage = kIdle;
uint32_t g_fetch_board = 0;
int32_t g_fetch_page = 0;
constexpr uint32_t kMaxBoardFriends = 100;   // PS3 friends board: at most 100 friends (plus the player)

std::string npid_of(const U2oScore *r) { return std::string(r->npid, strnlen(r->npid, sizeof r->npid)); }

// ---------------------------------------------------------------------------------------------
// The store file.

std::string store_path() {
    const char *env = std::getenv("UNCHARTED_U2MP_PROGRESSION");
    if (env && *env) return env;
    const char *dir = runtime_file_user_dir();
    std::string path = (dir && *dir) ? dir : ".";
    return path + "/u2mp/progression.bin";
}

// Loads the store once per process. A file that cannot be read is set aside rather than overwritten,
// so that the player can still recover it by hand.
void load_store() {
    if (g_store_tried) return;
    g_store_tried = true;
    const std::string path = store_path();
    if (store_api::load(path, g_store)) return;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return;
    std::filesystem::rename(path, path + ".unreadable", ec);
    log("progression: %s unreadable, kept as .unreadable; starting a new profile", path.c_str());
}

// ---------------------------------------------------------------------------------------------
// NetPlayerData connect / publish.

int32_t npd_state() { return game_get<int32_t>(kNpdState); }
bool net_online() { return game_get<int32_t>(kNetSmState) > 1; }

void fresh_data() {
    std::memset(game(kNpdData), 0, kBlobSize);
    game_set<uint32_t>(kNpdData, kDataVersion);
}

void connect() {
    if (npd_state() == kConnected || !net_online()) return;
    load_store();
    const uint32_t version = load_at<uint32_t>(g_store.blob.data());
    if (g_store.has_blob && version == kDataVersion) {
        std::memcpy(game(kNpdData), g_store.blob.data(), kBlobSize);
        log("progression: profile loaded (cash %u, publish %u)", game_get<uint32_t>(kNpdCash),
            g_store.publish_count);
    } else {
        fresh_data();
        log("progression: new profile");
    }
    std::memcpy(g_last_published, game(kNpdData), kBlobSize);
    game_set<int32_t>(kNpdState, kConnected);
}

void publish() {
    if (npd_state() != kConnected) return;
    load_store();
    const uint32_t count = game_get<uint32_t>(kNpdPublishCount) + 1u;
    game_set<uint32_t>(kNpdPublishCount, count);
    std::memcpy(g_store.blob.data(), game(kNpdData), kBlobSize);
    g_store.has_blob = true;
    g_store.publish_count = count;
    std::memcpy(g_last_published, game(kNpdData), kBlobSize);
    const std::string path = store_path();
    if (!store_api::save(path, g_store, uint64_t(std::time(nullptr))))
        log("progression: could not write %s", path.c_str());
}

void publish_if_changed() {
    if (npd_state() != kConnected) return;
    if (std::memcmp(g_last_published, game(kNpdData), kBlobSize) == 0) return;
    publish();
}

// Leaving the network: save what we have, then give the game an empty block again.
void offline_guard() {
    if (npd_state() != kConnected || net_online()) return;
    publish_if_changed();
    fresh_data();
    game_set<int32_t>(kNpdState, kOffline);
}

// ---------------------------------------------------------------------------------------------
// The local player's career values.

std::string local_name() {
    const char *env = std::getenv("UNCHARTED_U2MP_NAME");
    if (env && *env) return std::string(env).substr(0, store_api::kNameMax);
    if (!g_store.local_name.empty()) return g_store.local_name;
    return "Drake";
}

// The rank index for a cash amount: the band of the game's rank table the cash falls in.
uint32_t rank_from_money(uint32_t money) {
    const uint8_t *table = static_cast<const uint8_t *>(dc_lookup(kRankTableSid));
    if (!table) return 0;
    // In 64 bits so that a count of INT_MIN does not wrap to a large positive rank.
    const int64_t last = int64_t(load_at<int32_t>(table + 0x50)) - 1;
    const uint8_t *thresholds = reinterpret_cast<const uint8_t *>(load_at<uint64_t>(table + 0x58));
    if (last <= 0) return 0;
    if (!thresholds) return uint32_t(last);
    const int32_t m = int32_t(money);
    for (int64_t i = 0; i < last; ++i) {
        const int32_t low = load_at<int32_t>(thresholds + 4 * size_t(i));
        const int32_t high = load_at<int32_t>(thresholds + 4 * size_t(i) + 4);
        if (low <= m && high > m) return uint32_t(i);
    }
    return uint32_t(last);
}

store_api::LocalStats local_stats() {
    store_api::LocalStats s;
    s.name = local_name();
    s.money = game_get<uint32_t>(kNpdCash);
    s.kills = game_get<uint32_t>(kNpdKills);
    s.deaths = game_get<uint32_t>(kNpdDeaths);
    s.wins = game_get<uint32_t>(kNpdWins);
    s.losses = game_get<uint32_t>(kNpdLosses);
    s.seconds = game_get<uint32_t>(kNpdSeconds);
    s.coop_points = game_get<uint32_t>(kNpdCoopPoints);
    s.coop_seconds = game_get<uint32_t>(kNpdCoopSeconds);
    s.rank = rank_from_money(s.money);
    return s;
}

// ---------------------------------------------------------------------------------------------
// Leave: remember the match, post the scores.

void put_crc_u32(uint32_t &crc, uint32_t v) {
    uint8_t bytes[4];
    store_at<uint32_t>(bytes, v);
    crc = store_api::crc32(bytes, sizeof bytes, crc);
}

void record_match() {
    const uint32_t count = game_get<uint32_t>(kMatchPlayerCount);
    if (count == 0 || count > kMaxMatchPlayers) return;

    auto record = [](uint32_t i) { return game(kMatchRecords + uint64_t(i) * kRecordStride); };
    auto tally = [](uint32_t i) { return game(kPlayerTallies + uint64_t(i) * kTallyStride); };

    uint32_t team_size[2] = {0, 0};
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t team = record(i)[0x7];
        if (team < 2) ++team_size[team];
    }
    // A result only counts when both sides played and the scores differ.
    const bool decided = team_size[0] > 0 && team_size[1] > 0 &&
                         game_get<uint16_t>(kTeamTallies + 2) != game_get<uint16_t>(kTeamTallies + kTallyStride + 2);

    std::vector<store_api::MatchPlayer> players;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *rec = record(i);
        const uint8_t *tal = tally(i);
        store_api::MatchPlayer p;
        const char *name = reinterpret_cast<const char *>(rec + 0x14);
        p.name.assign(name, strnlen(name, store_api::kNameMax));
        p.local = rec[0xa] != 0;
        p.rank = rec[0xb];
        p.seconds = load_at<uint32_t>(rec + 0xc);
        p.kills = load_at<uint16_t>(tal + 4);
        p.deaths = load_at<uint16_t>(tal + 6);
        p.coop_points = load_at<uint32_t>(tal + 0x14);
        p.money = load_at<uint32_t>(tal + 0x18);
        const uint8_t team = rec[0x7];
        if (decided && team < 2)
            p.result = game_get<uint16_t>(kTeamTallies + uint64_t(team) * kTallyStride) == 1 ? 1 : -1;
        players.push_back(std::move(p));
    }

    // The same match can reach Leave more than once; a checksum of its tallies tells repeats apart.
    uint32_t crc = 0;
    put_crc_u32(crc, count);
    for (const auto &p : players) {
        crc = store_api::crc32(reinterpret_cast<const uint8_t *>(p.name.data()), p.name.size(), crc);
        put_crc_u32(crc, p.kills);
        put_crc_u32(crc, p.deaths);
        put_crc_u32(crc, p.seconds);
        put_crc_u32(crc, p.money);
    }
    if (crc == g_last_match) return;
    g_last_match = crc;
    load_store();
    store_api::note_match(g_store, players, uint64_t(std::time(nullptr)));
}

int32_t current_game_type() {
    const uint8_t *games = static_cast<const uint8_t *>(dc_lookup(kNetGamesSid));
    if (!games) return -1;
    const uint8_t *records = reinterpret_cast<const uint8_t *>(load_at<uint64_t>(games + 8));
    if (!records) return -1;
    const int16_t index = game_get<int16_t>(kNetGameIndex);
    if (index < 0 || int32_t(index) >= load_at<int32_t>(games)) return -1;
    return load_at<int32_t>(records + 0x90 * size_t(index) + 0xc);
}

void put_be32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

// One score post: the game info carries rank, time, deaths and losses big-endian, as PS3 sent them.
void post_score(int board, uint32_t score, uint32_t time, const store_api::LocalStats &s) {
    const uint32_t board_id = game_get<uint32_t>(kBoardTable + kBoardStride * uint64_t(board));
    uint8_t info[64] = {};
    put_be32(info + 0, s.rank);
    put_be32(info + 4, time);
    put_be32(info + 8, s.deaths);
    put_be32(info + 12, s.losses);
    online::record_score(board_id, int64_t(uint64_t(score)), nullptr, info, sizeof info);
}

void post_online_scores() {
    if (!online::linked()) return;
    const store_api::LocalStats s = local_stats();
    const int32_t type = current_game_type();
    const bool coop = type == 4 || type == 8 || type == 9 || type == 12;
    const bool matchmaking = game_get<int32_t>(kMatchmakingFlag) != 0;
    if (coop) {
        post_score(3, s.coop_points, s.coop_seconds, s);
        post_score(1, s.money, s.coop_seconds, s);
    } else if (matchmaking) {
        const uint32_t time = s.seconds + s.coop_seconds;
        post_score(0, s.kills, time, s);
        post_score(1, s.money, time, s);
        post_score(2, s.wins, time, s);
    } else {
        log("rankings: custom game: no scores posted (PS3 posted matchmaking and co-op only)");
    }
}

// ---------------------------------------------------------------------------------------------
// Leaderboard rows.

void clear_row_buffers() {
    std::memset(g_rank_rows, 0, sizeof g_rank_rows);
    std::memset(g_info_rows, 0, sizeof g_info_rows);
}

void set_rank_row(size_t k, const char *name, size_t name_len, uint32_t rank, int64_t score) {
    uint8_t *row = g_rank_rows + k * kRankRecord;
    std::memcpy(row, name, name_len);
    store_at<uint32_t>(row + 0x58, rank);
    store_at<uint32_t>(row + 0x5c, rank);
    store_at<uint32_t>(row + 0x60, rank);
    store_at<int64_t>(row + 0x70, score);
}

void point_lb_at_rows(uint32_t total, uint32_t rows, int32_t page) {
    store_at<uint64_t>(lb() + kLbRows, uint64_t(reinterpret_cast<uintptr_t>(g_rank_rows)));
    store_at<uint64_t>(lb() + kLbInfo, uint64_t(reinterpret_cast<uintptr_t>(g_info_rows)));
    store_at<uint32_t>(lb() + kLbTotal, total);
    store_at<uint32_t>(lb() + kLbRowCount, rows);
    store_at<int32_t>(lb() + kLbPage, page);
    store_at<uint32_t>(lb() + kLbStatus, 0);
}

void local_fetch(uint32_t board, int32_t request) {
    connect();
    load_store();
    const store_api::LocalStats s = local_stats();
    const std::vector<store_api::Entry> entries = store_api::board(g_store, s, int(board));
    const store_api::Page pg = store_api::page(entries, request);
    clear_row_buffers();
    const size_t n = std::min(pg.rows.size(), kPageRows);
    for (size_t k = 0; k < n; ++k) {
        const store_api::Entry &e = pg.rows[k];
        set_rank_row(k, e.name.data(), std::min(e.name.size(), store_api::kNameMax), e.rank, e.score);
        uint8_t *info = g_info_rows + k * kInfoRecord;
        store_at<uint32_t>(info + 0, e.badge);
        store_at<uint32_t>(info + 4, e.seconds);
        store_at<uint32_t>(info + 8, e.deaths);
        store_at<uint32_t>(info + 0xc, e.losses);
    }
    point_lb_at_rows(pg.total, uint32_t(n), pg.page);
}

// 1.5.12: one page of online rows into the LB buffers. `rows` is what LB rows-on-page says; the rows come from `src`
// (at most 11, and never more than `rows`).
void online_fill(uint32_t total, int32_t page, uint32_t rows, const std::vector<const U2oScore *> &src) {
    clear_row_buffers();
    const size_t n = std::min({src.size(), kPageRows, size_t(rows)});
    for (size_t k = 0; k < n; ++k) {
        const U2oScore *r = src[k];
        set_rank_row(k, r->npid, strnlen(r->npid, sizeof r->npid), r->rank, r->score);
        store_at<uint64_t>(g_rank_rows + k * kRankRecord + 0x78, r->record_date);
        // The game info was recorded big-endian; the game reads host-order words.
        uint8_t raw[16] = {};
        std::memcpy(raw, r->info, std::min<uint32_t>(r->info_len, 16));
        uint8_t *info = g_info_rows + k * kInfoRecord;
        for (int w = 0; w < 4; ++w) {
            const uint8_t *b = raw + 4 * w;
            store_at<uint32_t>(info + 4 * w,
                               uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3]);
        }
    }
    point_lb_at_rows(total, rows, page);
    store_at<uint64_t>(lb() + kLbBusy, 0);
    g_fetch_stage = kIdle;
}

// A failed online fetch shows an empty list (PS3; never the local board while linked).
void online_failed(int32_t page) {
    log("rankings: online board %u failed: empty list", g_fetch_board);
    online_fill(0, page < 0 ? 0 : page, 0, {});
}

// The rows of the answer, in order.
std::vector<const U2oScore *> answer_rows() {
    std::vector<const U2oScore *> rows;
    const int n = online::board_rows();
    for (int i = 0; i < n; ++i)
        if (const U2oScore *r = online::board_row(i)) rows.push_back(r);
    return rows;
}

// The global range from rank page * 11 + 1. False: the request could not start.
bool request_range(int32_t page) {
    g_fetch_stage = kRange;
    g_fetch_page = page;
    return online::board_request(g_fetch_board, 0, uint32_t(page) * 11u + 1u, 11);
}

// Rows on a page: total - page * 11, clamped to 0..11.
uint32_t page_rows(int64_t total, int32_t page) {
    return uint32_t(std::clamp<int64_t>(total - int64_t(page) * 11, 0, int64_t(kPageRows)));
}

void fetch_start(uint32_t board, int32_t request) {
    if (online::linked()) {
        const uint32_t id = game_get<uint32_t>(kBoardTable + kBoardStride * uint64_t(board));
        const bool friends = lb()[kLbFriends] != 0;
        g_fetch_stage = kIdle;
        g_fetch_board = id;
        g_fetch_page = request;
        bool started;
        if (friends) {
            g_fetch_stage = kFriends;
            started = online::board_request(id, 1, 0, kMaxBoardFriends);
        } else if (request < 0) {
            g_fetch_stage = kFindMyPage;
            started = online::board_request(id, 2, 0, 1);
            if (!started) started = request_range(0);   // the lookup failed: page 0 of the global range
        } else {
            started = request_range(request);
        }
        if (started) {
            store_at<uint64_t>(lb() + kLbBusy, 1);
            store_at<uint32_t>(lb() + kLbRowCount, 0);
            return;
        }
        online_failed(request);
        return;
    }
    local_fetch(board, request);
}

// 2.2: every friend (the first 100 of the friend list) and the player with their records on the board; unscored ones
// dropped, ordered by rank; total reported twice the kept count, as PS3 did.
void friends_fill() {
    const std::string me = online::my_npid_text();
    const std::vector<std::string> friends = online::friend_npids(kMaxBoardFriends);
    std::vector<const U2oScore *> kept;
    bool me_taken = false;
    for (const U2oScore *r : answer_rows()) {
        const std::string id = npid_of(r);
        const bool is_me = !me.empty() && id == me;
        if (is_me ? me_taken : std::find(friends.begin(), friends.end(), id) == friends.end()) continue;
        me_taken = me_taken || is_me;
        if (r->rank == 0) continue;   // no score on this board
        kept.push_back(r);
    }
    std::stable_sort(kept.begin(), kept.end(), [](const U2oScore *a, const U2oScore *b) { return a->rank < b->rank; });
    const int32_t count = int32_t(kept.size());
    int32_t page = g_fetch_page;
    if (page < 0) {   // the player's page; not on the board: page 0
        page = 0;
        for (int32_t k = 0; k < count; ++k)
            if (npid_of(kept[size_t(k)]) == me)
                page = k / 11;
    } else if (count > 0) {
        page = std::min(page, (count - 1) / 11);
    }
    const uint32_t rows = page_rows(count, page);
    std::vector<const U2oScore *> shown;
    for (uint32_t k = 0; k < rows; ++k) shown.push_back(kept[size_t(page) * 11u + k]);
    online_fill(2u * uint32_t(count), page, rows, shown);
}

// Moves the online fetch along. False: the list update has to wait for the score service.
bool fetch_progress() {
    if (g_fetch_stage == kIdle) return true;
    const int state = online::board_state();
    if (state == 1) return false;
    if (g_fetch_stage == kFindMyPage) {
        // Where the player stands; no score or a failed lookup: page 0 of the global range.
        const U2oScore *me = state == 0 ? online::board_row(0) : nullptr;
        const int32_t page = (me && me->rank != 0) ? int32_t((me->rank - 1u) / 11u) : 0;
        if (request_range(page)) return false;
        online_failed(page);
        return true;
    }
    if (state < 0) {
        online_failed(g_fetch_page);
        return true;
    }
    if (g_fetch_stage == kRange) {
        const uint32_t total = online::board_total();
        online_fill(total, g_fetch_page, page_rows(total, g_fetch_page), answer_rows());
        return true;
    }
    friends_fill();
    return true;
}

// ---------------------------------------------------------------------------------------------
// Entry points the game reaches.

U2MP_GUEST void player_info_update() {
    if (net_online()) {
        unlocks_refresh();
        connect();
        publish_if_changed();
        online::member_data_changed();
    }
    offline_guard();
}

U2MP_GUEST void *leave_publish(uint32_t id) {
    record_match();
    publish();
    post_online_scores();
    return dc_lookup(id);
}

U2MP_GUEST void leaderboard_list_update(uint8_t *list) {
    if (!fetch_progress()) return;
    if (load_at<uint64_t>(lb() + kLbBusy) != 0) return;

    int32_t first = load_at<int32_t>(lb() + kLbPage);
    store_at<int32_t>(list + kListPage, first);

    int32_t shown, delta;
    if (lb()[kLbReset] != 0) {
        store_at<int32_t>(list + kListBoard, -1);
        std::memset(list + kListColumns, 0, 16);
        list[kListFriends] = 0;
        lb()[kLbReset] = 0;
        shown = -1;
        delta = 0;
        first = 0;
    } else {
        delta = load_at<int32_t>(list + kListDelta);
        shown = load_at<int32_t>(list + kListBoard);
    }

    uint32_t board = load_at<uint32_t>(lb() + kLbBoard);
    uint8_t refresh = lb()[kLbRefresh];
    if (delta == 0) {
        // Nothing to do unless the board, the friends filter or a refresh request changed things.
        if (shown == int32_t(board) && list[kListFriends] == lb()[kLbFriends] && refresh == 0) return;
    } else {
        // The player scrolled: move to the next page, kept within the board.
        int32_t p = int32_t(uint32_t(first) + uint32_t(delta));
        store_at<int32_t>(list + kListPage, p);
        store_at<int32_t>(list + kListDelta, 0);
        const int32_t last = int32_t((load_at<uint32_t>(lb() + kLbTotal) - 1u) / 11u);
        if (p < 0)
            p = 0;
        else if (p > last)
            p = last;
        store_at<int32_t>(list + kListPage, p);
        if (p == first) return;
        refresh = lb()[kLbRefresh];
    }

    if (refresh != 0) {
        store_at<int32_t>(list + kListPage, 0);
        lb()[kLbRefresh] = 0;
    }

    board = load_at<uint32_t>(lb() + kLbBoard);
    const uint8_t friends = lb()[kLbFriends];
    if (shown != int32_t(board) || list[kListFriends] != friends) store_at<int32_t>(list + kListPage, -1);
    list[kListFriends] = friends;
    store_at<int32_t>(list + kListBoard, int32_t(board));

    if (board < 4) fetch_start(board, load_at<int32_t>(list + kListPage));

    // The column set of the board; like the game, this reads the table entry even for an index out of range.
    const uint64_t columns = uint64_t(reinterpret_cast<uintptr_t>(image.base)) + (kBoardTable + 4) +
                             kBoardStride * uint64_t(int64_t(int32_t(board)));
    store_at<uint32_t>(list + kListColumns, load_at<uint32_t>(reinterpret_cast<const uint8_t *>(uintptr_t(columns))));
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Interface.

void install_progression(int &installed) {
    // Each board id must be 300 + i; image.matches also makes sure the table lies inside the image.
    bool boards_ok = true;
    for (uint32_t i = 0; i < 4; ++i) {
        uint8_t id[4];
        store_at<uint32_t>(id, 300 + i);
        if (!image.matches(kBoardTable + kBoardStride * i, id, sizeof id)) boards_ok = false;
    }

    static const uint8_t player_info_bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x8d, 0x1d, 0xdb,
                                                0x8c, 0xaf, 0x00, 0x8b, 0x83, 0xc0, 0x06, 0x00, 0x00};
    if (image.matches(kPlayerInfoUpdate, player_info_bytes, sizeof player_info_bytes) &&
        replace_function(kPlayerInfoUpdate, sizeof player_info_bytes, reinterpret_cast<void *>(&player_info_update),
                         "NetPlayerData connect/publish (64e810)"))
        ++installed;
    else
        log("progression: 64e810 bytes differ, NetPlayerData connect not installed");

    static const uint8_t leave_bytes[] = {0xbf, 0x9d, 0x19, 0x5e, 0xc8};
    if (image.matches(kLeaveCallCheck, leave_bytes, sizeof leave_bytes) &&
        retarget_call(kLeaveCall, kDcLookup, reinterpret_cast<void *>(&leave_publish), "Leave publish"))
        ++installed;
    else
        log("progression: Leave::Enter call site differs, publish on leave not installed");

    static const uint8_t list_bytes[] = {0x4c, 0x8d, 0x0d, 0x59, 0xa9, 0xb8, 0x00, 0x49, 0x83,
                                         0x79, 0x58, 0x00, 0x0f, 0x85, 0x03, 0x01, 0x00, 0x00};
    if (boards_ok && image.matches(kListUpdate, list_bytes, sizeof list_bytes) &&
        replace_function(kListUpdate, sizeof list_bytes, reinterpret_cast<void *>(&leaderboard_list_update),
                         "leaderboard list update (5b3000)"))
        ++installed;
    else
        log("progression: leaderboard list bytes differ, leaderboards not installed");
}

void progression_publish() {
    if (!image.base) return;
    publish_if_changed();
}

void progression_publish_career() {
    if (!image.base) return;
    publish();
}

// Players Met never shows a friend (PS3 parity G1): online, the RPCN friend list decides; LAN has none.
static bool met_friend(const std::string &name) { return online::active() && online::is_friend(name); }

std::vector<std::string> progression_players_met() {
    // While the friend list is not known yet the online list is empty (PS3: while its friend load runs).
    if (online::active() && !online::friend_list_ready()) return {};
    load_store();
    return store_api::players_met(g_store, met_friend);
}

// The batch is written before this returns, so a later match start never meets a write still running (PS3
// drops a batch then; that case cannot occur here).
void progression_note_met(const std::vector<std::string> &names) {
    if (!image.base || names.empty()) return;
    load_store();
    const uint64_t now = uint64_t(std::time(nullptr));
    const std::vector<std::string> batch = store_api::note_met(g_store, names, now, met_friend);
    if (batch.empty()) return;
    for (const std::string &n : batch) log("progression: adding player %s to Players Met", n.c_str());
    const std::string path = store_path();
    if (!store_api::save(path, g_store, now)) log("progression: could not write %s", path.c_str());
}

std::string progression_player_summary(const std::string &name) {
    load_store();
    for (const auto &p : g_store.recent) {
        if (p.name != name) continue;
        char text[256];
        std::snprintf(text, sizeof text, "%s: %u matches with you, %u kills, %u deaths, %u wins, %u losses, rank %u.",
                      name.c_str(), p.matches, p.kills, p.deaths, p.wins, p.losses, p.rank);
        return text;
    }
    return name;
}

}  // namespace u2mp
