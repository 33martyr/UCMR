// u2mp progression store (host side, no guest code). See progression_store.h.
//
// File layout, little-endian:
//   0x00 char[8]  "U2MPPRG1"
//   0x08 u32      format (2; format 1 files are read too: they end before the headshots)
//   0x0c u32      blob size (0x228)
//   0x10 u32      publish count (NetPlayerData::Data +0x160, raised by every publish as on PS3)
//   0x14 u32      flags: bit 0 = blob present
//   0x18 u64      time of the save (unix seconds)
//   0x20 char[32] local player name
//   0x40 blob     NetPlayerData::Data, exactly as the guest holds it
//   ...  u32      recent player count, then that many 80-byte records:
//                 char name[32]; u32 kills, deaths, wins, losses, seconds, coop_points, money,
//                 matches, rank; u32 flags (bit 0 = unlisted; files before it hold 0 = listed); u64 last_seen
//   ...  u32      the local player's career headshots (format 2)
//   end  u32      CRC-32 (IEEE) of everything before it
#include <string>
#include <utility>
#include <vector>
#include "progression_store.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace u2mp::progression {
namespace {

constexpr char kMagic[8] = {'U', '2', 'M', 'P', 'P', 'R', 'G', '1'};
constexpr uint32_t kFormat = 2, kFormatNoHeadshots = 1;
constexpr size_t kHeader = 0x40, kRecord = 80;

void put32(std::vector<uint8_t> &out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
}
void put64(std::vector<uint8_t> &out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(uint8_t(v >> (8 * i)));
}
void put_name(std::vector<uint8_t> &out, const std::string &s) {
    char buf[32] = {};
    std::memcpy(buf, s.data(), std::min(s.size(), kNameMax));
    out.insert(out.end(), buf, buf + 32);
}
uint32_t get32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t get64(const uint8_t *p) { return uint64_t(get32(p)) | uint64_t(get32(p + 4)) << 32; }
std::string get_name(const uint8_t *p) {
    size_t n = 0;
    while (n < kNameMax && p[n]) ++n;
    return std::string(reinterpret_cast<const char *>(p), n);
}

bool read_file(const std::string &path, std::vector<uint8_t> &out) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        out.insert(out.end(), buf, buf + n);
        if (out.size() > (1u << 20)) break;   // far larger than any valid file
    }
    const bool ok = !std::ferror(f);
    std::fclose(f);
    return ok;
}

int64_t score_of(int board, uint32_t kills, uint32_t money, uint32_t wins, uint32_t coop) {
    switch (board) {
    case 0: return kills;   // board 300 (PS3 Leave 0x446114: Data +0x140)
    case 1: return money;   // board 301 (Data +0x94, the cash total)
    case 2: return wins;    // board 302 (Data +0x148)
    default: return coop;   // board 303 (Data +0x158, co-op games)
    }
}

}  // namespace

uint32_t crc32(const uint8_t *data, size_t size, uint32_t crc) {
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

std::vector<uint8_t> encode(const Store &store, uint64_t now) {
    std::vector<uint8_t> out(kMagic, kMagic + 8);
    put32(out, kFormat);
    put32(out, uint32_t(kBlobSize));
    put32(out, store.publish_count);
    put32(out, store.has_blob ? 1u : 0u);
    put64(out, now);
    put_name(out, store.local_name);
    out.insert(out.end(), store.blob.begin(), store.blob.end());
    const size_t count = std::min(store.recent.size(), kMaxRecent);
    put32(out, uint32_t(count));
    for (size_t i = 0; i < count; ++i) {
        const PlayerStats &p = store.recent[i];
        put_name(out, p.name);
        for (uint32_t v : {p.kills, p.deaths, p.wins, p.losses, p.seconds, p.coop_points, p.money, p.matches,
                           p.rank, p.unlisted ? 1u : 0u})
            put32(out, v);
        put64(out, p.last_seen);
    }
    put32(out, store.headshots);
    put32(out, crc32(out.data(), out.size()));
    return out;
}

bool decode(const uint8_t *data, size_t size, Store &store) {
    if (size < kHeader + kBlobSize + 8 || std::memcmp(data, kMagic, 8) != 0) return false;
    if (get32(data + size - 4) != crc32(data, size - 4)) return false;
    const uint32_t format = get32(data + 8);
    if ((format != kFormat && format != kFormatNoHeadshots) || get32(data + 0xc) != kBlobSize) return false;
    const size_t tail = format == kFormat ? 4 : 0;   // the headshots word
    const uint8_t *p = data + kHeader + kBlobSize;
    const uint32_t count = get32(p);
    p += 4;
    if (count > kMaxRecent || size != kHeader + kBlobSize + 4 + size_t(count) * kRecord + tail + 4) return false;
    Store s;
    s.publish_count = get32(data + 0x10);
    s.has_blob = (get32(data + 0x14) & 1) != 0;
    s.local_name = get_name(data + 0x20);
    std::memcpy(s.blob.data(), data + kHeader, kBlobSize);
    for (uint32_t i = 0; i < count; ++i, p += kRecord) {
        PlayerStats r;
        r.name = get_name(p);
        uint32_t *fields[] = {&r.kills, &r.deaths, &r.wins, &r.losses, &r.seconds, &r.coop_points, &r.money,
                              &r.matches, &r.rank};
        for (size_t f = 0; f < 9; ++f) *fields[f] = get32(p + 32 + 4 * f);
        r.unlisted = (get32(p + 68) & 1) != 0;
        r.last_seen = get64(p + 72);
        if (!r.name.empty()) s.recent.push_back(std::move(r));
    }
    if (tail) s.headshots = get32(p);
    store = std::move(s);
    return true;
}

bool load(const std::string &path, Store &store) {
    std::vector<uint8_t> bytes;
    for (const std::string &p : {path, path + ".bak"})
        if (read_file(p, bytes) && decode(bytes.data(), bytes.size(), store)) return true;
    return false;
}

bool save(const std::string &path, const Store &store, uint64_t now) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path target(path);
    if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
    const std::vector<uint8_t> bytes = encode(store, now);
    const std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    ok = (std::fflush(f) == 0) && ok;
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) {
        fs::remove(tmp, ec);
        return false;
    }
    // Keep the previous good file as .bak (load falls back to it), then move the new one in.
    if (fs::exists(target, ec)) {
        fs::rename(target, path + ".bak", ec);
        if (ec) {
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, target, ec);
    return !ec;
}

// Drops the local player if an older file listed them as a remote one, then orders and caps by recency.
static void trim_recent(Store &store) {
    if (!store.local_name.empty())
        store.recent.erase(std::remove_if(store.recent.begin(), store.recent.end(),
                                          [&](const PlayerStats &p) { return p.name == store.local_name; }),
                           store.recent.end());
    std::stable_sort(store.recent.begin(), store.recent.end(),
                     [](const PlayerStats &a, const PlayerStats &b) { return a.last_seen > b.last_seen; });
    if (store.recent.size() > kMaxRecent) store.recent.resize(kMaxRecent);
}

void note_match(Store &store, const std::vector<MatchPlayer> &players, uint64_t now) {
    for (const MatchPlayer &m : players) {
        if (m.name.empty()) continue;
        const std::string name = m.name.substr(0, kNameMax);
        if (m.local) {
            store.local_name = name;
            store.headshots += m.headshots;
            continue;
        }
        auto it = std::find_if(store.recent.begin(), store.recent.end(),
                               [&](const PlayerStats &p) { return p.name == name; });
        if (it == store.recent.end()) {
            store.recent.push_back(PlayerStats{});
            it = store.recent.end() - 1;
            it->name = name;
            it->unlisted = true;   // not added at the match start: statistics only
        }
        it->kills += m.kills;
        it->deaths += m.deaths;
        it->seconds += m.seconds;
        it->coop_points += m.coop_points;
        it->money += m.money;
        it->wins += m.result > 0 ? 1 : 0;
        it->losses += m.result < 0 ? 1 : 0;
        it->matches += 1;
        it->rank = m.rank;
        it->last_seen = now;
    }
    trim_recent(store);
}

std::vector<std::string> players_met(const Store &store, const IsFriend &is_friend) {
    std::vector<const PlayerStats *> order;
    for (const PlayerStats &p : store.recent)
        if (!p.unlisted && p.name != store.local_name && !(is_friend && is_friend(p.name))) order.push_back(&p);
    std::stable_sort(order.begin(), order.end(),
                     [](const PlayerStats *a, const PlayerStats *b) { return a->last_seen > b->last_seen; });
    std::vector<std::string> names;
    for (size_t i = 0; i < order.size() && i < kMetShown; ++i) names.push_back(order[i]->name);
    return names;
}

std::vector<std::string> note_met(Store &store, const std::vector<std::string> &names, uint64_t now,
                                  const IsFriend &is_friend) {
    const std::vector<std::string> listed = players_met(store, is_friend);
    std::vector<std::string> batch;
    for (const std::string &n : names) {
        const std::string name = n.substr(0, kNameMax);
        if (batch.size() >= kMetBatch) break;
        if (name.empty() || name == store.local_name || (is_friend && is_friend(name)) ||
            std::find(listed.begin(), listed.end(), name) != listed.end() ||
            std::find(batch.begin(), batch.end(), name) != batch.end())
            continue;
        batch.push_back(name);
    }
    for (const std::string &name : batch) {
        auto it = std::find_if(store.recent.begin(), store.recent.end(),
                               [&](const PlayerStats &p) { return p.name == name; });
        if (it == store.recent.end()) {
            store.recent.push_back(PlayerStats{});
            it = store.recent.end() - 1;
            it->name = name;
        }
        it->unlisted = false;
        it->last_seen = now;
    }
    if (!batch.empty()) trim_recent(store);
    return batch;
}

std::vector<Entry> board(const Store &store, const LocalStats &local, int b) {
    std::vector<Entry> out;
    if (b < 0 || b >= kBoards) return out;
    Entry me;
    me.name = local.name.empty() ? (store.local_name.empty() ? std::string("Player") : store.local_name) : local.name;
    me.score = score_of(b, local.kills, local.money, local.wins, local.coop_points);
    me.badge = local.rank;
    me.seconds = b == 3 ? local.coop_seconds : local.seconds + local.coop_seconds;
    me.deaths = local.deaths;
    me.losses = local.losses;
    me.local = true;
    out.push_back(me);
    for (const PlayerStats &p : store.recent) {
        if (p.name == me.name) continue;
        Entry e;
        e.name = p.name;
        e.score = score_of(b, p.kills, p.money, p.wins, p.coop_points);
        if (e.score <= 0) continue;
        e.badge = p.rank;
        e.seconds = p.seconds;
        e.deaths = p.deaths;
        e.losses = p.losses;
        out.push_back(e);
    }
    std::stable_sort(out.begin(), out.end(), [](const Entry &x, const Entry &y) {
        if (x.score != y.score) return x.score > y.score;
        return x.name < y.name;
    });
    for (size_t i = 0; i < out.size(); ++i)
        out[i].rank = (i > 0 && out[i].score == out[i - 1].score) ? out[i - 1].rank : uint32_t(i + 1);
    return out;
}

Page page(const std::vector<Entry> &entries, int request) {
    Page pg;
    pg.total = uint32_t(entries.size());
    if (entries.empty()) return pg;
    const int last = int((entries.size() - 1) / kRowsPerPage);
    int p = request;
    if (p < 0) {
        p = 0;
        for (size_t i = 0; i < entries.size(); ++i)
            if (entries[i].local) p = int(i / kRowsPerPage);
    }
    pg.page = std::clamp(p, 0, last);
    const size_t first = size_t(pg.page) * kRowsPerPage;
    const size_t end = std::min(entries.size(), first + kRowsPerPage);
    pg.rows.assign(entries.begin() + std::ptrdiff_t(first), entries.begin() + std::ptrdiff_t(end));
    return pg;
}

}  // namespace u2mp::progression
