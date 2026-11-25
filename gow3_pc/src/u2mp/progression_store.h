// u2mp progression store: the local replacement for the PS3 online progression back end.
//
// PS3 1.10 kept the multiplayer progression (NetPlayerData::Data, 0x228 bytes: version 10, cash,
// unlocks, kill/death/win/loss counters, trophy counters) on Sony's TUS server (Publish 0x4c6298,
// FetchTusData 0x4c65f8, connect thread 0x4c6ab8) with an HDD copy, and the scores on NP Score
// leaderboards (RecordScore 0x4ab6d0, boards 300..303). The PS4 1.02 build keeps the Data block in
// RAM only (NetPlayerData 0x11474f8 + 0x44) and has neither back end. This file is the host side:
// a small file holding that block plus what this PC saw of other players, and the leaderboard
// rows built from it. It knows nothing about the guest (unit-tested on the host).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace u2mp::progression {

constexpr size_t kBlobSize = 0x228;        // NetPlayerData::Data (u32 version + 0x224 bytes)
constexpr uint32_t kBlobVersion = 10;      // PS3 4c4be0 / PS4 662560 write 10
constexpr size_t kNameMax = 31;
constexpr size_t kMaxRecent = 64;
constexpr size_t kMetShown = 50;           // Players Met rows (PS3 players met list, 50 history entries)
constexpr size_t kMetBatch = 16;           // names one match start adds at most (PS3 players history batch)
constexpr int kBoards = 4;                 // NP Score boards 300..303
constexpr int kRowsPerPage = 11;           // PS3 GetRankingByRange(start = page * 11 + 1, 11 rows)

// What this PC saw of another player, summed over the matches played together.
struct PlayerStats {
    std::string name;
    uint32_t kills = 0, deaths = 0, wins = 0, losses = 0, seconds = 0, coop_points = 0, money = 0,
             matches = 0, rank = 0;
    uint64_t last_seen = 0;   // unix seconds
    // Known only from a match's end (joined late, or a friend when the match started): its statistics count
    // (leaderboards, View Profile) but it is no Players Met row until a match start adds it (note_met).
    bool unlisted = false;
};

// One player's line of a finished match (from NetStatGame, PS4 0x1147bd0).
struct MatchPlayer {
    std::string name;
    bool local = false;
    int result = 0;   // 1 won, -1 lost, 0 no result (draw / no teams)
    uint32_t kills = 0, deaths = 0, seconds = 0, coop_points = 0, money = 0, rank = 0;
};

struct Store {
    uint32_t publish_count = 0;
    bool has_blob = false;
    std::array<uint8_t, kBlobSize> blob{};
    std::string local_name;
    std::vector<PlayerStats> recent;
};

// File format (little-endian), see progression_store.cpp.
std::vector<uint8_t> encode(const Store &store, uint64_t now);
bool decode(const uint8_t *data, size_t size, Store &store);
// load tries `path`, then `path.bak` (the previous good file); save writes `path.tmp`, keeps the old
// file as `path.bak` and renames. Both return false on any error and leave `store` untouched then.
bool load(const std::string &path, Store &store);
bool save(const std::string &path, const Store &store, uint64_t now);

// Adds a finished match. The local player's line only updates `local_name`; the others are merged
// by name into `recent` (most recent kept, at most kMaxRecent). A name not stored yet comes in `unlisted`
// (Players Met membership is decided at match start only, PS3 parity G1).
void note_match(Store &store, const std::vector<MatchPlayer> &players, uint64_t now);

// "Is this name a friend" (online: the RPCN friend list); empty = no friends (LAN).
using IsFriend = std::function<bool(const std::string &)>;
// The Players Met rows (PS3 players met load): listed players that are no friend, most recently seen first, at
// most kMetShown. A player who became a friend drops out here.
std::vector<std::string> players_met(const Store &store, const IsFriend &is_friend);
// Players met at a match or round start (PS3 players met add): of `names`, the ones not empty, not the local
// player, not a friend and not already a Players Met row form the batch (at most kMetBatch, in order); each is
// added to `recent` with no statistics, or a stored one is listed again, with last_seen = now. Returns the batch
// (empty: nothing changed).
std::vector<std::string> note_met(Store &store, const std::vector<std::string> &names, uint64_t now,
                                  const IsFriend &is_friend);

// The local player's career values, read from the Data block by the guest side.
struct LocalStats {
    std::string name;
    uint32_t money = 0, kills = 0, deaths = 0, wins = 0, losses = 0, seconds = 0, coop_points = 0,
             coop_seconds = 0, rank = 0;
};

// One leaderboard line, laid out by the guest side into the PS3 rank/game-info records.
struct Entry {
    std::string name;
    int64_t score = 0;
    uint32_t rank = 0;      // 1-based position (equal scores share it)
    uint32_t badge = 0;     // game info +0: rank badge (rank index from cash)
    uint32_t seconds = 0;   // game info +4: time played
    uint32_t deaths = 0;    // game info +8
    uint32_t losses = 0;    // game info +0xc
    bool local = false;
};

// Board `board` (0..3 = NP Score boards 300 kills, 301 cash, 302 wins, 303 co-op points): the local
// player and every recent player with a score, best first.
std::vector<Entry> board(const Store &store, const LocalStats &local, int board);

struct Page {
    int page = 0;        // page shown (0-based)
    uint32_t total = 0;  // entries on the board
    std::vector<Entry> rows;
};
// `request` < 0 = the page holding the local player (PS3: GetRankingByNpId, then that page).
Page page(const std::vector<Entry> &entries, int request);

uint32_t crc32(const uint8_t *data, size_t size, uint32_t crc = 0);

}  // namespace u2mp::progression
