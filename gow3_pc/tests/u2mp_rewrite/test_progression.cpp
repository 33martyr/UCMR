// u2mp rewrite test "progression": the old (git) and the new progression.cpp, netinit.cpp and netpatch.cpp on the
// same inputs.
//   bash gow3_pc/tests/u2mp_rewrite/run.sh progression progression.cpp netinit.cpp netpatch.cpp
//
// The three files keep process-lifetime state (store loaded once, last published, fetch stage, netinit and netpatch
// flags), so every scenario runs in its own child process (the test program starts itself with the scenario name).
// In the child the scenario runs on the old build, then on a cleared image on the new build. Each side gets its own
// empty folder as the current directory (store file, net-patch9.bin); paths are relative, so logs compare as text.
// Compared: guest and host calls, logs, the store files (bytes and decoded), the image. Host pointers written into
// the image (leaderboard rows/info) are compared by what they point at; the netpatch block address is masked in logs.
#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <process.h>
#include <string>
#include <vector>

#define HX_NO_TIME_MACRO
#include "fake_time.h"
#include "deps_progression.h"
#include "harness.h"
#include "progression_store.h"
#include "u2_online_abi.h"

namespace u2mp_old {
void install_progression(int &installed);
void progression_publish();
std::vector<std::string> progression_recent_players(size_t max);
std::string progression_player_summary(const std::string &name);
void netinit_prepare();
void netinit_match();
void netinit_dump(const char *when);
void install_netpatch(int &installed);
int netpatch_load();
}  // namespace u2mp_old
namespace u2mp {
void install_progression(int &installed);
void progression_publish();
std::vector<std::string> progression_players_met();   // was progression_recent_players (parity G1, f11e480)
void progression_note_met(const std::vector<std::string> &names);
std::string progression_player_summary(const std::string &name);
void netinit_prepare();
void netinit_match();
void netinit_dump(const char *when);
void install_netpatch(int &installed);
int netpatch_load();
}  // namespace u2mp

namespace hx::pg {
State st;
}

namespace fs = std::filesystem;
namespace ps = u2mp::progression;
using hx::addr;
using hx::get;
using hx::mem;
using hx::put;
using hx::rec;
using hx::pg::st;

namespace {

struct Api {
    const char *tag;
    void (*install_progression)(int &);
    void (*publish)();
    std::vector<std::string> (*recent)(size_t);
    std::string (*summary)(const std::string &);
    void (*ni_prepare)();
    void (*ni_match)();
    void (*ni_dump)(const char *);
    void (*np_install)(int &);
    int (*np_load)();
};
const Api OLD{"old",
              u2mp_old::install_progression,
              u2mp_old::progression_publish,
              u2mp_old::progression_recent_players,
              u2mp_old::progression_player_summary,
              u2mp_old::netinit_prepare,
              u2mp_old::netinit_match,
              u2mp_old::netinit_dump,
              u2mp_old::install_netpatch,
              u2mp_old::netpatch_load};
// parity-g-friends.md G1 (f11e480): progression_recent_players(max) became progression_players_met(), the Players
// Met rows: at most kMetShown (50), no friend, no row a match's end added (unlisted). For what the old scenarios ask
// (LAN, rows of the store, fewer than 50) the new list cut at `max` is the old answer. The friend-list question G1
// adds (online::active, answered 0 = LAN) is dropped from the record here; the G1 scenarios (run_new) check it.
std::vector<std::string> new_recent(size_t max) {
    const size_t from = hx::calls.size();
    std::vector<std::string> v = u2mp::progression_players_met();
    hx::calls.erase(std::remove(hx::calls.begin() + std::ptrdiff_t(from), hx::calls.end(), std::string("online::active -> 0")),
                    hx::calls.end());
    if (v.size() > max) v.resize(max);
    return v;
}
const Api NEW{"new",
              u2mp::install_progression,
              u2mp::progression_publish,
              new_recent,
              u2mp::progression_player_summary,
              u2mp::netinit_prepare,
              u2mp::netinit_match,
              u2mp::netinit_dump,
              u2mp::install_netpatch,
              u2mp::netpatch_load};
const Api *A = nullptr;

// ---- guest fakes (one copy) --------------------------------------------------------------------------
std::map<uint32_t, uint64_t> g_lookup;   // DcLookup sid -> image offset of the value
std::map<uint32_t, uint64_t> g_find;     // DcFind sid -> image offset of the slot
uint64_t g_bind_slots = 0x16b0000;       // slots for symbols BindSymbol defines
uint8_t *g_block = nullptr;
size_t g_block_size = 0;
int g_reloc_mode = 0;   // 0 normal, 1 returns block+0x18, 2 does not relocate the root slot

uint32_t crc(const void *p, size_t n) { return ps::crc32(static_cast<const uint8_t *>(p), n); }
uint64_t fnv(const std::vector<uint8_t> &b) {   // (a file ending in its own CRC-32 always has the same CRC)
    uint64_t h = 1469598103934665603ull;
    for (uint8_t c : b) h = (h ^ c) * 1099511628211ull;
    return h;
}

U2MP_GUEST void *fake_dc_lookup(uint32_t sid) {
    auto it = g_lookup.find(sid);
    const bool ok = it != g_lookup.end() && it->second;
    rec("DcLookup 0x%08x -> %s", sid, ok ? hx::where(mem + it->second).c_str() : "null");
    return ok ? mem + it->second : nullptr;
}
U2MP_GUEST void *fake_dc_find(uint32_t sid) {
    auto it = g_find.find(sid);
    const bool ok = it != g_find.end() && it->second;
    rec("DcFind 0x%08x -> %s", sid, ok ? hx::where(mem + it->second).c_str() : "null");
    return ok ? mem + it->second : nullptr;
}
std::string describe(uint64_t v) {
    char b[64];
    if (g_block && v >= uint64_t(uintptr_t(g_block)) && v < uint64_t(uintptr_t(g_block)) + g_block_size)
        std::snprintf(b, sizeof b, "block+0x%llx", (unsigned long long)(v - uint64_t(uintptr_t(g_block))));
    else if (v >= uint64_t(uintptr_t(mem)) && v < uint64_t(uintptr_t(mem)) + hx::kImageSize)
        std::snprintf(b, sizeof b, "img+0x%llx", (unsigned long long)(v - uint64_t(uintptr_t(mem))));
    else
        std::snprintf(b, sizeof b, "0x%llx", (unsigned long long)v);
    return b;
}
U2MP_GUEST void *fake_relocate(void *buf) {
    uint8_t *b = static_cast<uint8_t *>(buf);
    g_block = b;
    rec("RelocateModule align16=%d size=%zu crc=%08x", int(uintptr_t(b) % 16 == 0), g_block_size, crc(b, g_block_size));
    uint64_t R;
    std::memcpy(&R, b + 8, 8);
    uint32_t L;
    std::memcpy(&L, b + R, 4);
    for (uint64_t s = 0; s < uint64_t(L) * 8; ++s) {
        if (!((b[R + 4 + s / 8] >> (s % 8)) & 1)) continue;
        if (g_reloc_mode == 2 && s == 2) continue;
        uint64_t v;
        std::memcpy(&v, b + s * 8, 8);
        v += uint64_t(uintptr_t(b));
        std::memcpy(b + s * 8, &v, 8);
    }
    return g_reloc_mode == 1 ? b + 0x18 : b + 0x10;
}
U2MP_GUEST void fake_bind(uint32_t sid, const uint64_t *value) {
    rec("BindSymbol 0x%08x value=%s", sid, describe(*value).c_str());
    const uint64_t slot = g_bind_slots;
    g_bind_slots += 8;
    uint64_t v = *value;
    if (g_block && v >= uint64_t(uintptr_t(g_block)) && v < uint64_t(uintptr_t(g_block)) + g_block_size) v -= uint64_t(uintptr_t(g_block)) - 0xb10c0000;
    put<uint64_t>(slot, v);   // block addresses differ between the builds: kept relative
    g_find[sid] = slot;
}

// replace the netpatch block address (printed with %p) in the record
void mask_block(std::vector<std::string> &r) {
    if (!g_block) return;
    for (int k : {0, 0x10, 0x18, 0x20}) {
        char p[64], m[32];
        std::snprintf(p, sizeof p, "%p", static_cast<void *>(g_block + k));
        std::snprintf(m, sizeof m, "BLOCK+0x%x", k);
        for (auto &s : r)
            for (size_t at; (at = s.find(p)) != std::string::npos;) s.replace(at, std::strlen(p), m);
    }
}

// ---- progression game facts ---------------------------------------------------------------------------
constexpr uint64_t NETSM = 0x14ce97c, NPD = 0x11474f8, DATA = 0x114753c, STATE = 0x1147bb8;
constexpr uint64_t CASH = 0x11475d0, KILLS = 0x114767c, DEATHS = 0x1147680, WINS = 0x1147684, LOSSES = 0x1147688,
                   SECS = 0x1147690, COOP = 0x1147694, COOPSECS = 0x1147698, PUBS = 0x114769c;
constexpr uint64_t LB = 0x113d960, LIST = 0x1660000;
constexpr uint64_t RANKTAB = 0x1650000, NETGAMES = 0x1651000;
constexpr uint64_t SG_COUNT = 0x1147bd8, SG_REC = 0x114b4bc, SG_TEAM = 0x114b9bc, SG_TALLY = 0x114ba24;

const uint8_t kPlayerInfo[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x8d, 0x1d, 0xdb,
                               0x8c, 0xaf, 0x00, 0x8b, 0x83, 0xc0, 0x06, 0x00, 0x00};
const uint8_t kListBytes[] = {0x4c, 0x8d, 0x0d, 0x59, 0xa9, 0xb8, 0x00, 0x49, 0x83,
                              0x79, 0x58, 0x00, 0x0f, 0x85, 0x03, 0x01, 0x00, 0x00};
const uint8_t kLeaveBytes[] = {0xbf, 0x9d, 0x19, 0x5e, 0xc8};

void putb(uint64_t off, const uint8_t *b, size_t n) { std::memcpy(mem + off, b, n); }

void rank_table(int32_t count, std::vector<int32_t> thr, bool null_thr = false) {
    put<int32_t>(RANKTAB + 0x50, count);
    for (size_t i = 0; i < thr.size(); ++i) put<int32_t>(RANKTAB + 0x100 + 4 * i, thr[i]);
    put<uint64_t>(RANKTAB + 0x58, null_thr ? 0 : addr(RANKTAB + 0x100));
    g_lookup[0xc85e199d] = RANKTAB;
}

void prog_image() {
    putb(0x64e810, kPlayerInfo, sizeof kPlayerInfo);
    putb(0x5b3000, kListBytes, sizeof kListBytes);
    putb(0x5f094d, kLeaveBytes, sizeof kLeaveBytes);
    put<uint8_t>(0x5f0952, 0xe8);
    for (uint32_t i = 0; i < 4; ++i) {
        put<uint32_t>(0xdef0a0 + 12 * i, 300 + i);
        put<uint32_t>(0xdef0a4 + 12 * i, 0xc0 + i);
        put<uint32_t>(0xdef0a8 + 12 * i, 0xeee0 + i);
    }
    put<uint32_t>(0xdef098, 0xbad1);     // column read for board -1
    put<uint32_t>(0xdef0d4, 0xbad4);     // column read for board 4
    hx::stub(0xa15e70, reinterpret_cast<void *>(&fake_dc_lookup));
    rank_table(5, {0, 1000, 5000, 20000, 100000});
}
void prog_install() {
    int n = 0;
    A->install_progression(n);
    rec("installed %d", n);
}
void prog_setup() {
    prog_image();
    prog_install();
}
void npd() {
    rec("npd state=%d netsm=%d data crc=%08x version=%u cash=%u pubs=%u", get<int32_t>(STATE), get<int32_t>(NETSM),
        crc(mem + DATA, 0x228), get<uint32_t>(DATA), get<uint32_t>(CASH), get<uint32_t>(PUBS));
}
void player_info() {
    rec("-> player_info");
    reinterpret_cast<void(U2MP_GUEST *)()>(mem + 0x64e810)();
    npd();
}
void leave(uint32_t id = 0xc85e199d) {
    rec("-> leave 0x%08x", id);
    auto fn = reinterpret_cast<void *(U2MP_GUEST *)(uint32_t)>(hx::retarget_host(0x5f0952));
    if (!fn) {
        rec("leave not installed");
        return;
    }
    void *r = fn(id);
    rec("leave returned %s", hx::where(r).c_str());
    npd();
}

uint64_t last_ptr[2] = {0, 0};
void lb_observe() {
    const uint64_t fields[2] = {0x28, 0x30};
    const size_t sizes[2] = {11 * 0x80, 11 * 0x40}, stride[2] = {0x80, 0x40};
    for (int f = 0; f < 2; ++f) {
        const uint64_t p = get<uint64_t>(LB + fields[f]);
        if (!p) {
            rec("lb+0x%llx null", (unsigned long long)fields[f]);
            continue;
        }
        const bool same_as_before = last_ptr[f] == 0 || last_ptr[f] == p;
        last_ptr[f] = p;
        rec("lb+0x%llx %s align16=%d same=%d", (unsigned long long)fields[f], hx::where(reinterpret_cast<void *>(p)).c_str(),
            int(p % 16 == 0), int(same_as_before));
        if (p >= addr(0) && p < addr(hx::kImageSize)) continue;
        const uint8_t *q = reinterpret_cast<const uint8_t *>(uintptr_t(p));
        for (size_t k = 0; k < 11; ++k) {
            const uint8_t *row = q + k * stride[f];
            size_t n = stride[f];
            while (n && !row[n - 1]) --n;
            std::string hex;
            char b[4];
            for (size_t i = 0; i < n; ++i) {
                std::snprintf(b, sizeof b, "%02x", row[i]);
                hex += b;
            }
            rec("  %s[%zu] %s", f ? "info" : "rows", k, hex.c_str());
        }
        (void)sizes;
    }
}
void lb_fields(uint64_t list = LIST) {
    rec("lb total=%u count=%u page=%d status=%u busy=%llu reset=%u board=%u friends=%u refresh=%u", get<uint32_t>(LB + 0x24),
        get<uint32_t>(LB + 0x70), get<int32_t>(LB + 0x74), get<uint32_t>(LB + 0x50), (unsigned long long)get<uint64_t>(LB + 0x58),
        get<uint8_t>(LB + 0x78), get<uint32_t>(LB + 0x80), get<uint8_t>(LB + 0x84), get<uint8_t>(LB + 0x85));
    rec("list cols=%08x delta=%d +48=%08x page=%d board=%d friends=%u", get<uint32_t>(list + 0x40), get<int32_t>(list + 0x44),
        get<uint32_t>(list + 0x48), get<int32_t>(list + 0x4c), get<int32_t>(list + 0x50), get<uint8_t>(list + 0x54));
}
void list_update(uint64_t list = LIST) {
    rec("-> list_update");
    reinterpret_cast<void(U2MP_GUEST *)(uint8_t *)>(mem + 0x5b3000)(mem + list);
    lb_fields(list);
    lb_observe();
}
void list_state(uint32_t board, int32_t shown, int32_t delta, uint8_t friends_lb, uint8_t friends_list, int32_t lb_page,
                uint32_t total) {
    put<uint32_t>(LB + 0x80, board);
    put<int32_t>(LIST + 0x50, shown);
    put<int32_t>(LIST + 0x44, delta);
    put<uint8_t>(LB + 0x84, friends_lb);
    put<uint8_t>(LIST + 0x54, friends_list);
    put<int32_t>(LB + 0x74, lb_page);
    put<uint32_t>(LB + 0x24, total);
    put<uint32_t>(LIST + 0x40, 0x11111111);
    put<uint32_t>(LIST + 0x48, 0x22222222);
}

// ---- store files ---------------------------------------------------------------------------------------
ps::Store make_store(uint32_t version = 10, bool has_blob = true, uint32_t cash = 1234, uint32_t pubs = 7) {
    ps::Store s;
    s.has_blob = has_blob;
    s.publish_count = pubs;
    std::memcpy(s.blob.data(), &version, 4);
    std::memcpy(s.blob.data() + (CASH - DATA), &cash, 4);
    std::memcpy(s.blob.data() + (KILLS - DATA), "\x05\0\0\0", 4);
    std::memcpy(s.blob.data() + (PUBS - DATA), &pubs, 4);
    s.local_name = "Stored";
    return s;
}
void add_recent(ps::Store &s, const std::string &name, uint32_t kills, uint32_t money, uint32_t wins, uint32_t coop,
                uint64_t seen) {
    ps::PlayerStats p;
    p.name = name;
    p.kills = kills;
    p.money = money;
    p.wins = wins;
    p.coop_points = coop;
    p.deaths = kills / 2 + 1;
    p.losses = 2;
    p.seconds = 600 + kills;
    p.matches = 3;
    p.rank = 4;
    p.last_seen = seen;
    s.recent.push_back(p);
}
void write_file(const std::string &path, const std::vector<uint8_t> &b) {
    fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(b.data()), std::streamsize(b.size()));
}
void write_store(const std::string &path, const ps::Store &s) { write_file(path, ps::encode(s, 1600000000)); }
void use_store_path(const char *p = "progression.bin") { _putenv_s("UNCHARTED_U2MP_PROGRESSION", p); }

void observe_files() {
    std::vector<std::string> blocks;   // one block of lines per file, sorted by name
    std::vector<std::string> lines;
    for (auto it = fs::recursive_directory_iterator(fs::current_path()); it != fs::recursive_directory_iterator(); ++it) {
        const std::string rel = fs::relative(it->path(), fs::current_path()).generic_string();
        if (it->is_directory()) {
            blocks.push_back("dir " + rel);
            continue;
        }
        std::ifstream f(it->path(), std::ios::binary);
        std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        lines.clear();
        char t[256];
        std::snprintf(t, sizeof t, "file %s size=%zu fnv=%016llx", rel.c_str(), b.size(), (unsigned long long)fnv(b));
        lines.push_back(t);
        ps::Store s;
        if (ps::decode(b.data(), b.size(), s)) {
            uint64_t saved = 0;
            std::memcpy(&saved, b.data() + 0x18, 8);
            std::snprintf(t, sizeof t, "  store pubs=%u has_blob=%d name=[%s] blob crc=%08x saved=%llu recent=%zu", s.publish_count,
                          int(s.has_blob), s.local_name.c_str(), crc(s.blob.data(), s.blob.size()), (unsigned long long)saved,
                          s.recent.size());
            lines.push_back(t);
            for (const auto &p : s.recent) {
                std::snprintf(t, sizeof t, "  recent [%s] k%u d%u w%u l%u s%u c%u m%u n%u r%u seen%llu", p.name.c_str(), p.kills,
                              p.deaths, p.wins, p.losses, p.seconds, p.coop_points, p.money, p.matches, p.rank,
                              (unsigned long long)p.last_seen);
                lines.push_back(t);
            }
        }
        std::string blk;
        for (auto &l : lines) blk += (blk.empty() ? std::string() : std::string("\n")) + l;
        blocks.push_back(blk);
    }
    std::sort(blocks.begin(), blocks.end());
    for (auto &blk : blocks) {
        size_t s = 0;
        for (size_t e; (e = blk.find('\n', s)) != std::string::npos; s = e + 1) rec("%s", blk.substr(s, e - s).c_str());
        rec("%s", blk.substr(s).c_str());
    }
}

// ---- match tallies ------------------------------------------------------------------------------------
struct MP {
    std::string name;
    uint8_t team, local, rank;
    uint32_t seconds;
    uint16_t kills, deaths;
    uint32_t coop, money;
};
void match(const std::vector<MP> &ps_, uint16_t score0, uint16_t score1, uint16_t mark0, uint16_t mark1, uint32_t count = ~0u) {
    put<uint32_t>(SG_COUNT, count == ~0u ? uint32_t(ps_.size()) : count);
    for (size_t i = 0; i < ps_.size(); ++i) {
        const uint64_t r = SG_REC + i * 0x80, t = SG_TALLY + i * 0x34;
        std::memset(mem + r, 0, 0x80);
        put<uint8_t>(r + 7, ps_[i].team);
        put<uint8_t>(r + 0xa, ps_[i].local);
        put<uint8_t>(r + 0xb, ps_[i].rank);
        put<uint32_t>(r + 0xc, ps_[i].seconds);
        std::memcpy(mem + r + 0x14, ps_[i].name.data(), std::min<size_t>(ps_[i].name.size(), 0x80 - 0x14));
        put<uint16_t>(t + 4, ps_[i].kills);
        put<uint16_t>(t + 6, ps_[i].deaths);
        put<uint32_t>(t + 0x14, ps_[i].coop);
        put<uint32_t>(t + 0x18, ps_[i].money);
    }
    put<uint16_t>(SG_TEAM, mark0);
    put<uint16_t>(SG_TEAM + 2, score0);
    put<uint16_t>(SG_TEAM + 0x34, mark1);
    put<uint16_t>(SG_TEAM + 0x36, score1);
}
std::vector<MP> two() { return {{"Me", 0, 1, 3, 300, 7, 2, 0, 900}, {"Rival", 1, 0, 5, 280, 4, 6, 0, 400}}; }

void connected_state() {   // NET_SM 2, connected, Data version 10 (as after a connect)
    put<int32_t>(NETSM, 2);
    put<int32_t>(STATE, 3);
    put<uint32_t>(DATA, 10);
}
void career(uint32_t cash, uint32_t kills, uint32_t deaths, uint32_t wins, uint32_t losses, uint32_t secs, uint32_t coop,
            uint32_t coopsecs) {
    put<uint32_t>(CASH, cash);
    put<uint32_t>(KILLS, kills);
    put<uint32_t>(DEATHS, deaths);
    put<uint32_t>(WINS, wins);
    put<uint32_t>(LOSSES, losses);
    put<uint32_t>(SECS, secs);
    put<uint32_t>(COOP, coop);
    put<uint32_t>(COOPSECS, coopsecs);
}
void net_games(int32_t count, int32_t type_at_1, bool null_records = false) {
    put<int32_t>(NETGAMES, count);
    put<uint64_t>(NETGAMES + 8, null_records ? 0 : addr(NETGAMES + 0x100));
    for (int i = 0; i < 4; ++i) put<int32_t>(NETGAMES + 0x100 + 0x90 * i + 0xc, i == 1 ? type_at_1 : 1);
    g_lookup[0x3b9a067d] = NETGAMES;
}
U2oScore score_row(const char *npid, uint32_t rank, int64_t score, uint64_t date, const std::vector<uint8_t> &info,
                   uint32_t info_len, size_t npid_len = ~size_t(0)) {
    U2oScore s{};
    std::memcpy(s.npid, npid, npid_len == ~size_t(0) ? std::strlen(npid) : npid_len);
    std::snprintf(s.name, sizeof s.name, "nm%u", rank);
    s.rank = rank;
    s.score = score;
    s.record_date = date;
    std::memcpy(s.info, info.data(), std::min<size_t>(64, info.size()));
    s.info_len = info_len;
    return s;
}
std::vector<uint8_t> seq(size_t n, uint8_t start = 1) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = uint8_t(start + i);
    return v;
}
void store_with_players(int n) {
    ps::Store s = make_store();
    for (int i = 0; i < n; ++i) add_recent(s, "P" + std::to_string(i), 10 + i, 100 * (i + 1), i % 4, i % 3, 100 + i);
    write_store("progression.bin", s);
    use_store_path();
}
void online_rows(int n, const char *mine_prefix = nullptr) {
    st.scores.clear();
    for (int i = 0; i < n; ++i) {
        char id[32];
        std::snprintf(id, sizeof id, "user%02d", i);
        st.scores.push_back(score_row(id, uint32_t(i + 1), 1000 - i, 63800000000000000ull + i, {0, 0, 0, 2, 0, 0, 1, 0x2c}, 8));
    }
    (void)mine_prefix;
}

// ---- netinit -------------------------------------------------------------------------------------------
struct Check {
    uint64_t at;
    std::vector<uint8_t> b;
};
const std::vector<Check> kNiChecks = {
    {0x640fe4, {0x49, 0x8b, 0xbf, 0xa0, 0x1b, 0x00, 0x00}},
    {0x641012, {0x4d, 0x63, 0x87, 0xdc, 0x1b, 0x00, 0x00}},
    {0x6410e0, {0x4c, 0x8d, 0x60, 0x60}},
    {0x64119e, {0x49, 0x8b, 0xbf, 0x10, 0x1c, 0x00, 0x00}},
    {0x6411d2, {0x49, 0x63, 0x8f, 0x4c, 0x1c, 0x00, 0x00}},
    {0x641290, {0x4c, 0x8d, 0x68, 0x38}},
    {0x5d90bb, {0x41, 0xc7, 0x47, 0x10, 0x58, 0x00, 0x00}},
    {0x5ec590, {0x48, 0x8d, 0x0d, 0x71, 0x95, 0xa7, 0x00, 0x31, 0xc0, 0x80, 0xb9, 0xc8, 0x00, 0x00, 0x00, 0x00}},
    {0x5f6750, {0x48, 0x8b, 0x44, 0xd7, 0x08, 0x39, 0x70, 0x0c, 0x74}},
    {0x5fd8ef, {0x48, 0x8d, 0x05, 0x92, 0xce, 0xb3, 0x00}},
    {0xb332a0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x41, 0x89}},
    {0xa15300, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec, 0x18, 0x4c, 0x8b, 0x05, 0xf0, 0x58, 0x63, 0x00}},
};
// The current netinit checks one more place (in 934a6ca, after the rewrite): the call in NetEventPlayerSetup::Apply
// that resets the slots, PS4 1.02 bytes (boot-linked.bin, image 0x632148).
const Check kNiPlayerSetup = {0x632148, {0xe8, 0x43, 0xba, 0x00, 0x00}};
constexpr uint64_t TA = 0x113d300, TB = 0x113d370, BOOST = 0x113a788, BSLOT = 0x1670000, BTAB = 0x1670100, BRECS = 0x1671000;

// DcFind with its checked prologue kept: after the prologue, undo it and jump to the fake
void dcfind_trampoline() {
    const uint8_t epi[] = {0x48, 0x83, 0xc4, 0x18, 0x5b, 0x5d};   // add rsp,0x18; pop rbx; pop rbp
    putb(0xa15300, kNiChecks.back().b.data(), 16);
    putb(0xa15310, epi, sizeof epi);
    hx::stub(0xa15316, reinterpret_cast<void *>(&fake_dc_find));
}
void ni_image() {
    for (const auto &c : kNiChecks) putb(c.at, c.b.data(), c.b.size());
    putb(kNiPlayerSetup.at, kNiPlayerSetup.b.data(), kNiPlayerSetup.b.size());
    dcfind_trampoline();
    std::memset(mem + 0x1600000, 0xcd, 0x40000);
}
void boosters(int32_t n, const std::vector<uint32_t> &masks, bool null_recs = false) {
    g_find[0x916e03b0] = BSLOT;
    put<uint64_t>(BSLOT, addr(BTAB));
    put<int32_t>(BTAB, n);
    put<uint64_t>(BTAB + 8, null_recs ? 0 : addr(BRECS));
    for (size_t i = 0; i < masks.size(); ++i) {
        put<uint32_t>(BRECS + 0x20 * i, uint32_t(0x5000 + i));
        put<uint32_t>(BRECS + 0x20 * i + 0x18, masks[i]);
    }
}
void ni_state() {
    rec("history %016llx leave %u %u boosters %u", (unsigned long long)get<uint64_t>(0x113d402), get<uint8_t>(0x1065bd0),
        get<uint8_t>(0x105f080), get<uint32_t>(BOOST));
}
void prepare() {
    rec("-> netinit_prepare");
    A->ni_prepare();
    ni_state();
}
void nmatch() {
    rec("-> netinit_match");
    A->ni_match();
    ni_state();
}

// ---- netpatch ------------------------------------------------------------------------------------------
const uint8_t kRelocBytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                               0x53, 0x48, 0x83, 0xec, 0x18, 0x48, 0x89, 0x7d, 0xd0, 0x40, 0xf6, 0xc7};
const uint8_t kBindBytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                              0x53, 0x48, 0x83, 0xec, 0x18, 0x4c, 0x8b, 0x0d, 0x78, 0x3e, 0x63, 0x00};
void free_list(int n, bool cyclic = false) {
    const uint64_t base = 0x1690000;
    put<uint64_t>(0x15340d8, n ? addr(base) : 0);
    for (int i = 0; i < n; ++i) put<uint64_t>(base + 16 * i, i + 1 < n ? addr(base + 16 * (i + 1)) : (cyclic ? addr(base) : 0));
}
void np_image(int nodes = 200) {
    putb(0xa19a00, kRelocBytes, sizeof kRelocBytes);
    putb(0xa16d70, kBindBytes, sizeof kBindBytes);
    dcfind_trampoline();
    put<uint64_t>(0x15340d0, addr(0x1680000));
    put<int32_t>(0x153410c, 64);
    free_list(nodes);
}
void np_install() {
    int n = 0;
    A->np_install(n);
    rec("installed %d", n);
    hx::stub(0xa19a00, reinterpret_cast<void *>(&fake_relocate));
    hx::stub(0xa16d70, reinterpret_cast<void *>(&fake_bind));
}
void np_load() {
    const int r = A->np_load();
    rec("netpatch_load -> %d", r);
}
// a module: root entries `sids`, each value a pointer to its own 8-byte data slot; extra_l more bitmap bytes
std::vector<uint8_t> module(const std::vector<uint32_t> &sids, uint32_t extra_l = 0) {
    const uint64_t C = sids.size(), data = 0x28 + 16 * C;
    uint64_t R = (data + 8 * C + 7) & ~uint64_t(7);
    if (R < 0x40) R = 0x40;
    const uint32_t L = uint32_t((R + 63) / 64) + extra_l;
    std::vector<uint8_t> v(R + 4 + L, 0);
    auto p32 = [&](uint64_t o, uint32_t x) { std::memcpy(v.data() + o, &x, 4); };
    auto p64 = [&](uint64_t o, uint64_t x) { std::memcpy(v.data() + o, &x, 8); };
    auto mark = [&](uint64_t slot) { v[R + 4 + slot / 8] |= uint8_t(1u << (slot % 8)); };
    std::memcpy(v.data(), "00CD", 4);
    p64(8, R);
    p64(0x10, 0x20);
    p32(0x1c, 0x916aa605);
    p32(0x20, 1);
    p32(0x24, uint32_t(C));
    p32(R, L);
    mark(2);
    for (uint64_t i = 0; i < C; ++i) {
        p32(0x28 + 16 * i, sids[i]);
        p64(0x30 + 16 * i, data + 8 * i);
        p64(data + 8 * i, 0xa000 + i);
        mark((0x30 + 16 * i) / 8);
    }
    return v;
}
void write_module(const std::vector<uint8_t> &v) {
    write_file("net-patch9.bin", v);
    g_block_size = v.size();
}

// ---- running ---------------------------------------------------------------------------------------------
struct Result {
    std::vector<std::string> rec;
    std::vector<uint8_t> img;
};
struct Scenario {
    std::string name;
    std::function<void()> fn;
    // compared scenarios: the old result in the form a documented change gives it (named where it is set)
    std::function<void(Result &)> to_new;
    // set: a new-only scenario (behaviour the old code did not have); EXPECTs on the new result
    std::function<void(const Result &)> check;
};
std::vector<Scenario> scenarios;
void S(const std::string &name, std::function<void()> fn, std::function<void(Result &)> to_new = {}) {
    scenarios.push_back({name, std::move(fn), std::move(to_new), {}});
}
void N(const std::string &name, std::function<void()> fn, std::function<void(const Result &)> check) {
    scenarios.push_back({name, std::move(fn), {}, std::move(check)});
}

fs::path g_root, g_base;

// ---- new-only checks ---------------------------------------------------------------------------------------
const char *g_scenario = "";
int g_failed_checks = 0;
#define EXPECT(c)                                                                     \
    do {                                                                              \
        if (!(c)) {                                                                   \
            std::printf("FAIL %s (line %d): %s\n", g_scenario, __LINE__, #c);         \
            ++g_failed_checks;                                                        \
        }                                                                             \
    } while (0)
size_t count_of(const Result &r, const std::string &line) { return size_t(std::count(r.rec.begin(), r.rec.end(), line)); }
std::vector<std::string> starting(const Result &r, const std::string &prefix) {
    std::vector<std::string> v;
    for (const auto &l : r.rec)
        if (l.rfind(prefix, 0) == 0) v.push_back(l);
    return v;
}
std::vector<std::string> requests(const Result &r) { return starting(r, "online::board_request "); }
std::string last_lb(const Result &r) {   // the LB fields at the end of the scenario
    const auto v = starting(r, "lb total=");
    return v.empty() ? std::string() : v.back();
}
std::string lb_line(uint32_t total, uint32_t count, int32_t page, uint64_t busy, uint32_t board, uint8_t friends,
                    uint8_t refresh = 0) {
    char b[160];
    std::snprintf(b, sizeof b, "lb total=%u count=%u page=%d status=0 busy=%llu reset=0 board=%u friends=%u refresh=%u", total,
                  count, page, (unsigned long long)busy, board, friends, refresh);
    return b;
}
// row k of the LB rank rows (progression.md 1.5.12: online id text at +0, rank at +0x58)
const uint8_t *lb_row(size_t k) {
    const uint64_t p = get<uint64_t>(LB + 0x28);
    return p ? reinterpret_cast<const uint8_t *>(uintptr_t(p)) + k * 0x80 : nullptr;
}
std::string row_id(size_t k) {
    const uint8_t *r = lb_row(k);
    return r ? std::string(reinterpret_cast<const char *>(r), strnlen(reinterpret_cast<const char *>(r), 0x58)) : "(none)";
}
uint32_t row_rank(size_t k) {
    const uint8_t *r = lb_row(k);
    uint32_t v = 0;
    if (r) std::memcpy(&v, r + 0x58, 4);
    return v;
}

// ---- documented changes: old record -> new form -------------------------------------------------------------
// netinit, in 934a6ca (after the rewrite; reasons in netinit.cpp, no spec section): the first netinit_prepare /
// netinit_match whose code checks pass also runs the net bit writer pass first (the fake image holds none of the 342
// 1.02 sites: logged, nothing patched) and, after the boosters, turns the player-setup slot reset (0x632148, the real
// call bytes) into NOPs. The other two additions (NetCharacter heap, foreground meshes) need guest state these
// scenarios do not set. The N-G scenarios check all four.
void ni_expected(Result &r) {
    auto &v = r.rec;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] != "-> netinit_prepare" && v[i] != "-> netinit_match") continue;
        size_t end = i + 1;
        while (end < v.size() && v[end].rfind("history ", 0) != 0) ++end;
        for (size_t k = i + 1; k < end; ++k)
            if (v[k].size() > 20 && v[k].compare(v[k].size() - 20, 20, "differ; nothing done") == 0) return;
        if (r.img[0x632148] == 0xe8) {
            v.insert(v.begin() + std::ptrdiff_t(end), "log netinit: the player-setup event keeps the joiner's slots");
            std::memset(r.img.data() + 0x632148, 0x90, 5);
        }
        v.insert(v.begin() + std::ptrdiff_t(i) + 1, "log netinit: 0 net bit writer sites, not the 342 of 1.02: masks left alone");
        return;
    }
}
// Headshots (475fba8, parity-d-flow1.md 2.10): a match the Leave hook records first asks the game type (net-games
// lookup) to count the local player's headshots. Whether the old code recorded a match is not visible in its record,
// so this one question is taken out of the new record where it stands (right after the Leave call); the HS scenarios
// check it and the count.
void drop_headshot_lookup(Result &r) {
    auto &v = r.rec;
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i - 1].rfind("-> leave ", 0) == 0 && v[i].rfind("DcLookup 0x3b9a067d -> ", 0) == 0) v.erase(v.begin() + std::ptrdiff_t(i));
}
// c91d21d (parity-a-leaderboards.md 2.3): every row of a range answer is read (the rows on the page come from the
// answer's rows), not only the first 11 up to a missing one: the old record gets the reads it did not make.
void read_whole_answer(Result &r) {
    auto &v = r.rec;
    for (size_t i = 0; i < v.size(); ++i) {
        int n = 0;
        if (std::sscanf(v[i].c_str(), "online::board_rows -> %d", &n) != 1) continue;
        size_t j = i + 1;
        int m = 0;
        for (; j < v.size() && v[j].rfind("online::board_row ", 0) == 0; ++j) ++m;
        std::vector<std::string> add;
        for (int k = m; k < n; ++k) {
            const bool ok = size_t(k) < st.scores.size() && !(size_t(k) < st.null_row.size() && st.null_row[size_t(k)]);
            add.push_back("online::board_row " + std::to_string(k) + " -> " + (ok ? "row" : "NULL"));
        }
        v.insert(v.begin() + std::ptrdiff_t(j), add.begin(), add.end());
        i = j + add.size() - 1;
    }
}

Result run_side(const Api &api, const Scenario &sc) {
    hx::clear();
    hx::bind_both();
    st = hx::pg::State{};
    g_lookup.clear();
    g_find.clear();
    g_bind_slots = 0x16b0000;
    g_block = nullptr;
    g_block_size = 0;
    g_reloc_mode = 0;
    last_ptr[0] = last_ptr[1] = 0;
    hx::fake_time_s = 1700000000;
    for (const char *e : {"UNCHARTED_U2MP_PROGRESSION", "UNCHARTED_U2MP_NAME", "UNCHARTED_U2MP_NETPATCH"}) _putenv_s(e, "");
    const fs::path dir = g_root / api.tag;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    fs::current_path(dir);
    A = &api;
    sc.fn();
    hx::bind_both();
    rec("== end");
    lb_fields();
    lb_observe();
    observe_files();
    fs::current_path(g_root);
    Result r;
    r.img = hx::snapshot();
    for (uint64_t f : {LB + 0x28, LB + 0x30}) {
        const uint64_t p = get<uint64_t>(f);
        if (p && !(p >= addr(0) && p < addr(hx::kImageSize))) std::memset(r.img.data() + f, 0x48, 8);
    }
    r.rec = hx::calls;
    mask_block(r.rec);
    return r;
}

int run_one(const std::string &name, bool verbose) {
    for (const auto &sc : scenarios) {
        if (sc.name != name) continue;
        fs::path safe = g_base / "fs";
        std::string n2;
        for (char c : name) n2 += (std::isalnum(uint8_t(c)) || c == '-') ? c : '_';
        g_root = safe / n2;
        fs::create_directories(g_root);
        fs::current_path(g_root);
        if (sc.check) {   // new only
            Result n = run_side(NEW, sc);
            g_scenario = sc.name.c_str();
            sc.check(n);
            const bool ok = g_failed_checks == 0;
            if (verbose || !ok) {
                std::printf("---- %s new record\n", name.c_str());
                for (auto &s : n.rec) std::printf("  %s\n", s.c_str());
            }
            std::printf("%s %s (new only, %zu records)\n", ok ? "PASS" : "FAIL", name.c_str(), n.rec.size());
            return ok ? 0 : 1;
        }
        Result o = run_side(OLD, sc);
        ni_expected(o);
        if (sc.to_new) sc.to_new(o);
        Result n = run_side(NEW, sc);
        drop_headshot_lookup(n);
        const bool ok = hx::same(name.c_str(), o.rec, n.rec, &o.img, &n.img);
        if (verbose || !ok) {
            std::printf("---- %s old record\n", name.c_str());
            for (auto &s : o.rec) std::printf("  %s\n", s.c_str());
            if (!ok) {
                std::printf("---- %s new record\n", name.c_str());
                for (auto &s : n.rec) std::printf("  %s\n", s.c_str());
            }
        }
        std::printf("%s %s (%zu records)\n", ok ? "PASS" : "FAIL", name.c_str(), o.rec.size());
        return ok ? 0 : 1;
    }
    std::printf("no scenario %s\n", name.c_str());
    return 2;
}

// =====================================================================================================
void define() {
    // ---------------- progression install ----------------
    S("P-I1 install all match", [] { prog_setup(); });
    S("P-I2 64e810 differs", [] { prog_image(); mem[0x64e810 + 18] = 1; prog_install(); });
    S("P-I3 5f094d differs", [] { prog_image(); mem[0x5f094d + 4] = 0; prog_install(); });
    for (int e = 0; e < 4; ++e)
        S("P-I4 board entry " + std::to_string(e) + " wrong", [e] { prog_image(); put<uint32_t>(0xdef0a0 + 12 * e, 303); prog_install(); });
    S("P-I5 5b3000 differs", [] { prog_image(); mem[0x5b3000 + 3] = 0; prog_install(); });
    S("P-I6 replace 64e810 fails", [] { prog_image(); hx::fail_offsets = {0x64e810}; prog_install(); });
    S("P-I7 retarget 5f0952 fails", [] { prog_image(); hx::fail_offsets = {0x5f0952}; prog_install(); });
    S("P-I8 replace 5b3000 fails", [] { prog_image(); hx::fail_offsets = {0x5b3000}; prog_install(); });
    S("P-I9 everything differs", [] { prog_image(); mem[0x64e810] = 0; mem[0x5f094d] = 0; mem[0x5b3000] = 0; prog_install(); });
    S("P-I10 image ends before the board table", [] {
        prog_image();
        hx::set_image_both(mem, 0xd00000);
        prog_install();
    });

    // ---------------- player info ----------------
    S("P-1 NET_SM 1 state 0", [] { prog_setup(); use_store_path(); put<int32_t>(NETSM, 1); player_info(); });
    S("P-2 NET_SM 2 no store file", [] { prog_setup(); use_store_path(); put<int32_t>(NETSM, 2); player_info(); });
    S("P-3 version-10 blob", [] {
        prog_setup();
        write_store("progression.bin", make_store());
        use_store_path();
        put<int32_t>(NETSM, 2);
        player_info();
    });
    S("P-3b version 10 but no has_blob", [] {
        prog_setup();
        write_store("progression.bin", make_store(10, false));
        use_store_path();
        put<int32_t>(NETSM, 5);
        player_info();
    });
    S("P-4 blob version 9", [] {
        prog_setup();
        write_store("progression.bin", make_store(9));
        use_store_path();
        put<int32_t>(NETSM, 2);
        player_info();
    });
    S("P-5 unreadable store", [] {
        prog_setup();
        write_file("progression.bin", {1, 2, 3, 4, 5});
        use_store_path();
        put<int32_t>(NETSM, 2);
        player_info();
    });
    S("P-5b garbage file, good .bak", [] {
        prog_setup();
        write_file("progression.bin", {1, 2, 3});
        write_store("progression.bin.bak", make_store(10, true, 777, 3));
        use_store_path();
        put<int32_t>(NETSM, 2);
        player_info();
    });
    S("P-5c only a bad .bak", [] {
        prog_setup();
        write_file("progression.bin.bak", {1, 2, 3});
        use_store_path();
        put<int32_t>(NETSM, 2);
        player_info();
    });
    S("P-5d store path is a directory", [] {
        prog_setup();
        fs::create_directories("progression.bin");
        use_store_path();
        put<int32_t>(NETSM, 2);
        player_info();
    });
    S("P-6 state 3, Data changed", [] {
        prog_setup();
        use_store_path();
        connected_state();
        put<uint32_t>(CASH, 50);
        player_info();
    });
    S("P-7 state 3, Data unchanged", [] {
        prog_setup();
        use_store_path();
        put<int32_t>(NETSM, 2);
        player_info();   // connects: Data = last published
        player_info();
    });
    S("P-8 state 3, NET_SM 1, Data changed", [] {
        prog_setup();
        use_store_path();
        connected_state();
        put<int32_t>(NETSM, 1);
        put<uint32_t>(KILLS, 9);
        player_info();
    });
    S("P-8b state 3, NET_SM 1, Data unchanged", [] {
        prog_setup();
        use_store_path();
        put<int32_t>(NETSM, 2);
        player_info();
        put<int32_t>(NETSM, 0);
        player_info();
        player_info();
    });
    S("P-9 save fails", [] {
        prog_setup();
        write_file("blocker", {1});
        use_store_path("blocker/sub/progression.bin");
        connected_state();
        put<uint32_t>(CASH, 5);
        player_info();
    });
    S("P-10 store load only once", [] {
        prog_setup();
        write_file("progression.bin", {9, 9, 9});
        use_store_path();
        put<int32_t>(NETSM, 2);
        player_info();
        write_store("progression.bin", make_store(10, true, 4444, 8));
        put<int32_t>(STATE, 0);
        player_info();
    });
    S("P-11 user dir path", [] {
        prog_setup();
        write_store("ud/u2mp/progression.bin", make_store(10, true, 99, 1));
        put<int32_t>(NETSM, 2);
        player_info();
        put<uint32_t>(CASH, 100);
        player_info();
    });
    S("P-12 user dir NULL", [] {
        prog_setup();
        st.user_dir_null = true;
        put<int32_t>(NETSM, 2);
        player_info();
        put<uint32_t>(CASH, 100);
        player_info();
    });
    S("P-13 user dir empty", [] {
        prog_setup();
        st.user_dir = "";
        put<int32_t>(NETSM, 2);
        player_info();
        put<uint32_t>(CASH, 100);
        player_info();
    });
    S("P-15 publish sequence and .bak", [] {
        prog_setup();
        write_store("progression.bin", make_store());
        use_store_path();
        put<int32_t>(NETSM, 2);
        player_info();
        put<uint32_t>(CASH, 2000);
        player_info();
        player_info();
        hx::fake_time_s = 1700000500;
        put<uint32_t>(WINS, 3);
        player_info();
    });
    S("P-16 publish counter wraps", [] {
        prog_setup();
        use_store_path();
        connected_state();
        put<uint32_t>(PUBS, 0xffffffffu);
        player_info();
    });
    S("P-17 offline guard after connect, then reconnect", [] {
        prog_setup();
        write_store("progression.bin", make_store());
        use_store_path();
        put<int32_t>(NETSM, 2);
        player_info();
        put<uint32_t>(CASH, 3000);
        put<int32_t>(NETSM, 1);
        player_info();
        put<int32_t>(NETSM, 2);
        player_info();
    });

    // ---------------- leave ----------------
    S("P-L1a count 0 state 0", [] { prog_setup(); use_store_path(); match(two(), 5, 3, 1, 0, 0); leave(); });
    S("P-L1b count 11 state 3 changed", [] {
        prog_setup();
        use_store_path();
        connected_state();
        put<uint32_t>(CASH, 1);
        match(two(), 5, 3, 1, 0, 11);
        leave();
    });
    S("P-L1c other id, lookup NULL", [] { prog_setup(); use_store_path(); leave(0x1234); });
    S("P-L2 decided match", [] {
        prog_setup();
        use_store_path();
        connected_state();
        match(two(), 5, 3, 1, 0);
        leave();
    });
    S("P-L3 same match twice", [] {
        prog_setup();
        use_store_path();
        connected_state();
        match(two(), 5, 3, 1, 0);
        leave();
        leave();
    });
    S("P-L3b two different matches", [] {
        prog_setup();
        use_store_path();
        connected_state();
        match(two(), 5, 3, 1, 0);
        leave();
        auto m = two();
        m[1].kills = 9;
        match(m, 5, 3, 1, 0);
        hx::fake_time_s += 60;
        leave();
    });
    S("P-L3c same tallies, only rank/coop differ", [] {
        prog_setup();
        use_store_path();
        connected_state();
        match(two(), 5, 3, 1, 0);
        leave();
        auto m = two();
        m[1].rank = 9;
        m[1].coop = 77;
        m[1].team = 0;
        match(m, 5, 3, 1, 0);
        leave();
    });
    S("P-L4a one team empty", [] {
        prog_setup();
        use_store_path();
        connected_state();
        auto m = two();
        m[1].team = 0;
        match(m, 5, 3, 1, 0);
        leave();
    });
    S("P-L4b equal scores", [] { prog_setup(); use_store_path(); connected_state(); match(two(), 4, 4, 1, 0); leave(); });
    S("P-L4c markers 2/1", [] { prog_setup(); use_store_path(); connected_state(); match(two(), 1, 3, 2, 1); leave(); });
    S("P-L5 player team 2", [] {
        prog_setup();
        use_store_path();
        connected_state();
        auto m = two();
        m.push_back({"Third", 2, 0, 1, 50, 1, 1, 0, 10});
        m.push_back({"Fourth", 1, 0, 1, 60, 2, 2, 0, 20});
        match(m, 5, 3, 1, 0);
        leave();
    });
    S("P-L6 name 40 bytes, empty name, name with NUL", [] {
        prog_setup();
        use_store_path();
        connected_state();
        auto m = two();
        m[1].name = std::string(40, 'N');
        m.push_back({"", 1, 0, 1, 50, 1, 1, 0, 10});
        m.push_back({std::string("Ab\0cd", 5), 0, 0, 1, 60, 2, 2, 0, 20});
        match(m, 5, 3, 1, 0);
        leave();
    });
    S("P-L6b 10 players", [] {
        prog_setup();
        use_store_path();
        connected_state();
        std::vector<MP> m;
        for (int i = 0; i < 10; ++i)
            m.push_back({"Pl" + std::to_string(i), uint8_t(i % 2), uint8_t(i == 0), uint8_t(i), uint32_t(100 + i), uint16_t(i),
                         uint16_t(10 - i), uint32_t(i * 3), uint32_t(1000 * i)});
        match(m, 9, 3, 0, 1);
        leave();
    });
    for (int type : {4, 8, 9, 12})
        S("P-L7 linked coop type " + std::to_string(type), [type] {
            prog_setup();
            use_store_path();
            connected_state();
            st.linked = true;
            career(6000, 11, 12, 13, 14, 1500, 77, 900);
            net_games(3, type);
            put<int16_t>(0x113d400, 1);
            match(two(), 5, 3, 1, 0);
            leave();
        });
    S("P-L8 linked competitive matchmaking", [] {
        prog_setup();
        use_store_path();
        connected_state();
        st.linked = true;
        career(25000, 11, 12, 13, 14, 0xfffffff0u, 77, 0x20);
        net_games(3, 5);
        put<int16_t>(0x113d400, 1);
        put<int32_t>(0x104f1fc, 1);
        match(two(), 5, 3, 1, 0);
        leave();
    });
    S("P-L9 linked competitive custom", [] {
        prog_setup();
        use_store_path();
        connected_state();
        st.linked = true;
        net_games(3, 5);
        put<int16_t>(0x113d400, 1);
        leave();
    });
    const char *l10[] = {"net-games NULL", "records NULL", "index -1", "index = count", "index count-1 coop"};
    for (int c = 0; c < 5; ++c)
        S(std::string("P-L10 ") + l10[c], [c] {
            prog_setup();
            use_store_path();
            st.linked = true;
            put<int32_t>(0x104f1fc, c == 4 ? 0 : 7);
            career(500, 1, 2, 3, 4, 5, 6, 7);
            if (c != 0) net_games(c == 4 ? 2 : 3, 9, c == 1);
            put<int16_t>(0x113d400, int16_t(c == 2 ? -1 : c == 3 ? 3 : 1));
            leave();
        });
    struct RankCase {
        const char *name;
        int32_t count;
        bool null_table, null_thr;
        uint32_t money;
    };
    const RankCase rc[] = {{"table NULL", 5, true, false, 500},       {"count 0", 0, false, false, 500},
                           {"count 1", 1, false, false, 500},          {"thresholds NULL count 5", 5, false, true, 500},
                           {"money below thr0", 5, false, false, 0},   {"money between thr2 thr3", 5, false, false, 6000},
                           {"money above all", 5, false, false, 999999}, {"money >= 0x80000000", 5, false, false, 0x80000005u},
                           {"money equals thr1", 5, false, false, 1000}, {"count INT_MIN thresholds NULL", INT32_MIN, false, true, 1},
                           {"count negative", -3, false, false, 1}};
    for (const auto &c : rc)
        S(std::string("P-L11 rank ") + c.name, [c] {
            prog_setup();
            use_store_path();
            st.linked = true;
            put<int32_t>(0x104f1fc, 1);
            if (c.null_table) g_lookup.erase(0xc85e199d);
            else rank_table(c.count, {-5, 1000, 5000, 20000, 100000}, c.null_thr);
            if (c.money == 0) rank_table(c.count, {10, 1000, 5000, 20000, 100000}, c.null_thr);
            career(c.money, 1, 2, 3, 4, 5, 6, 7);
            leave(0x77);
        });
    S("P-L12 not linked", [] { prog_setup(); use_store_path(); connected_state(); match(two(), 5, 3, 1, 0); leave(); });
    S("P-L13 state 2 at leave", [] {
        prog_setup();
        use_store_path();
        put<int32_t>(NETSM, 2);
        put<int32_t>(STATE, 2);
        put<uint32_t>(CASH, 9);
        match(two(), 5, 3, 1, 0);
        leave();
    });
    S("P-L14 leave with store from file, then player info", [] {
        prog_setup();
        ps::Store s = make_store();
        add_recent(s, "Rival", 1, 1, 1, 1, 5);
        write_store("progression.bin", s);
        use_store_path();
        put<int32_t>(NETSM, 2);
        player_info();
        match(two(), 5, 3, 1, 0);
        leave();
        player_info();
    });
    // Headshots (475fba8, parity-d-flow1.md 2.10), new only: the local player's kill records of kind 1 (killer = its
    // net id, victim another player) of a competitive match go into the store's career count at Leave.
    auto kills = [](const std::vector<std::array<uint16_t, 3>> &recs) {   // {kind, killer, victim}
        put<uint64_t>(0x113b760, addr(0x1655000));   // NetGameManager +0: the local tracker, net id at +0x120
        put<uint32_t>(0x1655000 + 0x120, 7);
        put<uint32_t>(0x1147bd0 + 0xe8, uint32_t(recs.size()));
        for (size_t i = 0; i < recs.size(); ++i) {
            const uint64_t r = 0x1147bd0 + 0xec + 0x1c * i;
            put<uint8_t>(r, uint8_t(recs[i][0]));
            put<uint16_t>(r + 2, recs[i][1]);
            put<uint16_t>(r + 4, recs[i][2]);
        }
    };
    auto stored_headshots = [] {
        std::ifstream f(g_root / "new" / "progression.bin", std::ios::binary);
        std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        ps::Store s;
        return ps::decode(b.data(), b.size(), s) ? int64_t(s.headshots) : -1;
    };
    for (int type : {5, 4})
        N(type == 5 ? "HS-1 competitive match: headshots counted" : "HS-2 co-op match: no headshots", [kills, type] {
            prog_setup();
            use_store_path();
            connected_state();
            net_games(3, type);
            put<int16_t>(0x113d400, 1);
            kills({{1, 7, 9}, {1, 7, 7}, {0, 7, 9}, {1, 8, 7}, {1, 7, 3}});   // headshots 1 and 5 are the player's
            match(two(), 5, 3, 1, 0);
            leave();
            leave();   // the same match again: not counted twice
        }, [type, stored_headshots](const Result &r) {
            std::vector<size_t> after;   // what follows each Leave call
            for (size_t i = 0; i + 1 < r.rec.size(); ++i)
                if (r.rec[i] == "-> leave 0xc85e199d") after.push_back(i + 1);
            EXPECT(after.size() == 2 && r.rec[after[0]] == "DcLookup 0x3b9a067d -> img+0x1651000" &&
                   r.rec[after[1]] != r.rec[after[0]]);
            EXPECT(count_of(r, "log progression: 2 headshot(s) this match, 2 in all") == (type == 5 ? 1u : 0u));
            EXPECT(stored_headshots() == (type == 5 ? 2 : 0));
        });

    // ---------------- leaderboard list ----------------
    auto lb_setup = [] {
        prog_setup();
        store_with_players(25);
        put<int32_t>(NETSM, 2);
        career(2500, 15, 3, 2, 1, 100, 5, 50);
    };
    S("P-B1 busy, no online fetch", [lb_setup] {
        lb_setup();
        list_state(1, 0, 0, 0, 0, 0, 0);
        put<uint64_t>(LB + 0x58, 1);
        list_update();
    });
    S("P-B2 reset", [lb_setup] {
        lb_setup();
        list_state(1, 3, 5, 1, 1, 2, 30);
        put<uint8_t>(LB + 0x78, 1);
        list_update();
    });
    S("P-B3 same board, nothing changed", [lb_setup] {
        lb_setup();
        list_state(1, 1, 0, 0, 0, 2, 30);
        list_update();
    });
    S("P-B4 same board, friends differ", [lb_setup] {
        lb_setup();
        list_state(1, 1, 0, 0, 1, 2, 30);
        list_update();
    });
    S("P-B5 same board, refresh", [lb_setup] {
        lb_setup();
        list_state(1, 1, 0, 0, 0, 2, 30);
        put<uint8_t>(LB + 0x85, 1);
        list_update();
    });
    struct Scroll {
        const char *name;
        int32_t first, delta;
        uint32_t total;
        int32_t shown;
        uint8_t refresh;
    };
    const Scroll sc[] = {{"delta +1", 0, 1, 30, 1, 0},           {"delta -5 clamps to first 0", 0, -5, 30, 1, 0},
                         {"delta -5 from 2", 2, -5, 30, 1, 0},     {"delta past last", 1, 10, 30, 1, 0},
                         {"total 0", 0, 3, 0, 1, 0},               {"total 0 from huge first", 390451572, 5, 0, 1, 0},
                         {"total 1 delta 1", 0, 1, 1, 1, 0},       {"total 11 delta 1", 0, 1, 11, 1, 0},
                         {"total 12 delta 1", 0, 1, 12, 1, 0},     {"overflow 0x7fffffff + 1", 0x7fffffff, 1, 30, 1, 0},
                         {"overflow INT_MIN + -1", INT32_MIN, -1, 30, 1, 0},
                         {"delta with refresh", 0, 1, 30, 1, 1},   {"P-B7 delta and shown != b", 0, 1, 30, 2, 0},
                         {"P-B7 delta, shown != b, p == first", 0, -1, 30, 2, 0},
                         {"page -1 first, delta 1", -1, 1, 30, 1, 0}};
    for (const auto &s : sc)
        S(std::string("P-B6 scroll ") + s.name, [lb_setup, s] {
            lb_setup();
            list_state(1, s.shown, s.delta, 0, 0, s.first, s.total);
            put<uint8_t>(LB + 0x85, s.refresh);
            list_update();
        });
    for (uint32_t b : {4u, 0xffffffffu, 100u})
        S("P-B8 board index " + std::to_string(b), [lb_setup, b] {
            lb_setup();
            st.linked = true;
            list_state(b, 0, 0, 0, 0, 0, 30);
            list_update();
        });
    for (int b = 0; b < 4; ++b)
        S("P-B9 not linked local rows board " + std::to_string(b), [lb_setup, b] {
            lb_setup();
            list_state(uint32_t(b), -1, 0, 0, 0, 0, 0);
            list_update();
            // and the next page by scrolling
            put<int32_t>(LIST + 0x44, 1);
            list_update();
            put<int32_t>(LIST + 0x44, 1);
            list_update();
        });
    S("P-B9b local rows, NET_SM 1 (no connect)", [lb_setup] {
        lb_setup();
        put<int32_t>(NETSM, 1);
        list_state(0, -1, 0, 0, 0, 0, 0);
        list_update();
    });
    S("P-B9c local rows, store unreadable", [] {
        prog_setup();
        write_file("progression.bin", {7});
        use_store_path();
        put<int32_t>(NETSM, 2);
        list_state(2, -1, 0, 0, 0, 0, 0);
        list_update();
    });
    S("P-O4 name env 40 chars in local rows", [lb_setup] {
        lb_setup();
        _putenv_s("UNCHARTED_U2MP_NAME", std::string(40, 'Z').c_str());
        list_state(0, -1, 0, 0, 0, 0, 0);
        list_update();
    });
    S("P-O4b stored local name in local rows", [] {
        prog_setup();
        ps::Store s = make_store();
        s.local_name = "LocalHero";
        add_recent(s, "LocalHero", 999, 999, 9, 9, 1);
        add_recent(s, "Other", 5, 5, 0, 0, 2);
        write_store("progression.bin", s);
        use_store_path();
        put<int32_t>(NETSM, 2);
        list_state(0, -1, 0, 0, 0, 0, 0);
        list_update();
    });
    // online fetch start
    struct Start {
        const char *name;
        uint8_t friends;
        int32_t delta;
        bool ok;
    };
    const Start ss[] = {{"friends ok", 1, 0, true},  {"r<0 ok", 0, 0, true},   {"r>=0 ok", 0, 1, true},
                        {"friends false", 1, 0, false}, {"r<0 false", 0, 0, false}, {"r>=0 false", 0, 1, false}};
    for (const auto &s : ss) {
        auto body = [lb_setup, s] {
            lb_setup();
            st.linked = true;
            st.request_default = s.ok;
            st.board_state = 1;
            if (s.delta) list_state(2, 2, 1, s.friends, s.friends, 0, 50);
            else list_state(2, 1, 0, s.friends, s.friends ^ 1, 0, 50);
            put<uint32_t>(LB + 0x70, 9);
            list_update();
            list_update();   // waits when started
        };
        if (s.ok) {
            S(std::string("P-B10 start ") + s.name, body);
            continue;
        }
        // c91d21d: a fetch that cannot start shows an empty list (PS3), no longer the local board; a "my page" lookup
        // that cannot start asks page 0 of the global range instead (parity-a-leaderboards.md 2.3). New only.
        N(std::string("P-B10 start ") + s.name, body, [s](const Result &r) {
            std::vector<std::string> want;
            if (s.friends) want = {"online::board_request 302 1 0 100 -> 0"};   // 2.2: at most 100 friends
            else if (!s.delta) want = {"online::board_request 302 2 0 1 -> 0", "online::board_request 302 0 1 11 -> 0"};
            else want = {"online::board_request 302 0 12 11 -> 0"};          // page 1 from rank 12
            EXPECT(requests(r) == want);
            EXPECT(count_of(r, "log rankings: online board 302 failed: empty list") == 1);
            EXPECT(last_lb(r) == lb_line(0, 0, s.delta ? 1 : 0, 0, 2, s.friends));
        });
    }
    S("P-B11a progress wait", [lb_setup] {
        lb_setup();
        st.linked = true;
        st.board_state = 1;
        list_state(0, 1, 0, 0, 1, 0, 50);
        list_update();
        list_update();
        list_update();
    });
    // c91d21d (parity-a-leaderboards.md 2.3): a failed "my page" lookup asks page 0 of the global range, and a failed
    // range shows an empty list (no local board). New only.
    for (uint32_t b : {1u, 7u})
        N("P-B11b failure, LB board " + std::to_string(b), [lb_setup, b] {
            lb_setup();
            st.linked = true;
            st.board_state = 1;
            list_state(1, 1, 0, 0, 1, 0, 50);
            list_update();
            st.board_state = -1;
            put<uint32_t>(LB + 0x80, b);
            list_update();
            list_update();
        }, [b](const Result &r) {
            EXPECT(requests(r) == std::vector<std::string>({"online::board_request 301 2 0 1 -> 1",
                                                            "online::board_request 301 0 1 11 -> 1"}));
            EXPECT(count_of(r, "log rankings: online board 301 failed: empty list") == 1);
            EXPECT(last_lb(r) == lb_line(0, 0, 0, 0, b, 0));
        });
    N("P-B11b2 failure with refresh pending", [lb_setup] {
        lb_setup();
        st.linked = true;
        st.board_state = 1;
        list_state(1, 2, 1, 0, 0, 0, 50);
        list_update();
        st.board_state = -3;
        put<uint8_t>(LB + 0x85, 1);
        list_update();
    }, [](const Result &r) {   // the failed lookup goes on with page 0 of the range: still busy, refresh kept
        EXPECT(requests(r) == std::vector<std::string>({"online::board_request 301 2 0 1 -> 1",
                                                        "online::board_request 301 0 1 11 -> 1"}));
        EXPECT(starting(r, "log rankings:").empty());
        EXPECT(last_lb(r) == lb_line(50, 0, 0, 1, 1, 0, 1));
    });
    struct Stage1 {
        const char *name;
        bool null_row;
        uint32_t rank;
        bool request_ok;
    };
    const Stage1 s1[] = {{"row NULL", true, 0, true}, {"rank 0", false, 0, true}, {"rank 23", false, 23, true},
                         {"rank 11", false, 11, true}, {"rank 12", false, 12, true}, {"rank 23 request false", false, 23, false},
                         {"rank 0xffffffff", false, 0xffffffffu, true}};
    for (const auto &s : s1) {
        auto body = [lb_setup, s] {
            lb_setup();
            st.linked = true;
            st.board_state = 1;
            list_state(3, 1, 0, 0, 1, 0, 50);
            list_update();   // stage 1 started
            st.board_state = 0;
            if (!s.null_row) st.scores = {score_row("me", s.rank, 5, 6, {1, 2, 3, 4}, 4)};
            st.request_default = s.request_ok;
            list_update();   // stage 1 -> 2
            online_rows(13);
            st.rows = 13;
            st.total = 400;
            list_update();   // stage 2 fill (or idle)
            list_update();
        };
        if (s.request_ok && s.rank != 0xffffffffu) {
            S(std::string("P-B11c stage 1 ") + s.name, body, read_whole_answer);
            continue;
        }
        // c91d21d (2.3), new only: the range of rank 23's page (2) cannot start: empty list on page 2. Rank 0xffffffff:
        // page (rank - 1) / 11 = 390451572, so rows on page = 400 - page * 11 clamped to 0..11 = 0.
        N(std::string("P-B11c stage 1 ") + s.name, body, [s](const Result &r) {
            if (!s.request_ok) {
                EXPECT(requests(r) == std::vector<std::string>({"online::board_request 303 2 0 1 -> 1",
                                                                "online::board_request 303 0 23 11 -> 0"}));
                EXPECT(count_of(r, "log rankings: online board 303 failed: empty list") == 1);
                EXPECT(last_lb(r) == lb_line(0, 0, 2, 0, 3, 0));
            } else {
                EXPECT(requests(r) == std::vector<std::string>({"online::board_request 303 2 0 1 -> 1",
                                                                "online::board_request 303 0 4294967293 11 -> 1"}));
                EXPECT(starting(r, "log rankings:").empty());
                EXPECT(last_lb(r) == lb_line(400, 0, 390451572, 0, 3, 0));
            }
        });
    }
    // c91d21d (2.3): rows on page = total - page * 11 clamped to 0..11 (77 on page 1: 11), not the 5 rows the answer
    // held; the rows themselves are filled as before.
    S("P-B11d stage 2 fill details", [lb_setup] {
        lb_setup();
        st.linked = true;
        st.board_state = 1;
        list_state(0, 0, 1, 0, 0, 0, 50);
        list_update();   // stage 2, page 1
        st.board_state = 0;
        st.scores = {score_row("a", 1, 100, 7, seq(64), 0), score_row("b", 2, -5, 8, seq(64, 0x40), 8),
                     score_row("c", 3, 1LL << 40, 9, seq(64, 0x80), 64),
                     score_row("ABCDEFGHIJKLMNOPQRST", 4, 3, 10, seq(64), 3, 20), score_row("e", 5, 1, 11, seq(64), 17)};
        st.rows = 5;
        st.total = 77;
        list_update();
    }, [](Result &r) {
        const std::string from = "lb total=77 count=5 page=1 ", to = "lb total=77 count=11 page=1 ";
        for (auto &l : r.rec)
            if (l.rfind(from, 0) == 0) l = to + l.substr(from.size());
        if (r.img[LB + 0x70] == 5) r.img[LB + 0x70] = 11;
    });
    // c91d21d (2.3), new only: total 13 on page 1 is 2 rows on the page, the answer's first two (a missing row is
    // left out of the answer, not the end of it).
    for (bool null3 : {true, false})
        N(null3 ? "P-B11d2 stage 2, 13 rows, NULL at 3" : "P-B11d3 stage 2, 13 rows, 13 present", [lb_setup, null3] {
            lb_setup();
            st.linked = true;
            st.board_state = 1;
            list_state(0, 0, 1, 0, 0, 0, 50);
            list_update();
            st.board_state = 0;
            online_rows(13);
            if (null3) st.null_row = {false, false, false, true};
            st.rows = 13;
            st.total = 13;
            list_update();
        }, [](const Result &r) {
            EXPECT(requests(r) == std::vector<std::string>({"online::board_request 300 0 12 11 -> 1"}));
            EXPECT(last_lb(r) == lb_line(13, 2, 1, 0, 0, 0));
            EXPECT(row_id(0) == "user00" && row_rank(0) == 1 && row_id(1) == "user01" && row_rank(1) == 2);
            EXPECT(row_id(2).empty() && row_rank(2) == 0);
        });
    struct Stage3 {
        const char *name;
        int32_t delta;   // 0: page -1 (friends changed); else scroll to page 2
        std::vector<int> mine;
        int n;
    };
    const Stage3 s3[] = {{"page -1, me at 3 and 14", 0, {3, 14}, 20}, {"page -1, me not found", 0, {}, 20},
                         {"page -1, me at 14, n 12 (scan limited)", 0, {14}, 12}, {"page 2", 2, {3}, 30},
                         {"page -1, n 0", 0, {}, 0}};
    // Friends board (c91d21d, parity-a-leaderboards.md 2.2), new only: the player and the first 100 friends of the
    // friend list with their records, unscored ones dropped, by rank; page -1: the player's index / 11; page p:
    // min(p, (kept - 1) / 11); total twice the kept count. No friends here: the player's first row only.
    for (const auto &s : s3)
        N(std::string("P-B11e stage 3 ") + s.name, [lb_setup, s] {
            lb_setup();
            st.linked = true;
            st.board_state = 1;
            if (s.delta) list_state(1, 1, s.delta, 1, 1, 0, 50);
            else list_state(1, 1, 0, 1, 0, 0, 50);
            list_update();
            st.board_state = 0;
            online_rows(30);
            for (int i : s.mine) std::snprintf(st.scores[size_t(i)].npid, 20, "Me");
            st.my_npid = "Me";
            st.rows = s.n;
            list_update();
        }, [s](const Result &r) {
            EXPECT(requests(r) == std::vector<std::string>({"online::board_request 301 1 0 100 -> 1"}));
            EXPECT(count_of(r, "online::my_npid_text -> [Me]") == 1 && count_of(r, "online::friend_npids 100 -> 0") == 1);
            const bool found = !s.mine.empty() && s.mine[0] < s.n;   // the player's first row of the answer
            EXPECT(last_lb(r) == lb_line(found ? 2 : 0, found ? 1 : 0, 0, 0, 1, 1));
            if (found) EXPECT(row_id(0) == "Me" && row_rank(0) == uint32_t(s.mine[0] + 1));
            EXPECT(row_id(found ? 1 : 0).empty());
        });
    N("P-B11g stage 3 npid of 20 bytes without NUL", [lb_setup] {
        lb_setup();
        st.linked = true;
        st.board_state = 1;
        list_state(1, 1, 0, 1, 0, 0, 50);
        list_update();
        st.board_state = 0;
        online_rows(30);
        st.scores[13] = score_row("ABCDEFGHIJKLMNOPQRST", 14, 1, 1, {}, 0, 20);
        st.scores[13].name[0] = 0;
        st.scores[25] = score_row("ABCDEFGHIJKLMNOPQRST", 26, 1, 1, {}, 0, 20);   // name "nm26" follows: no match
        st.my_npid = "ABCDEFGHIJKLMNOPQRST";
        st.rows = 30;
        list_update();
        st.null_row.assign(30, false);
    }, [](const Result &r) {   // the 20-byte id is the player's (rows 13 and 25); the first one is kept
        EXPECT(last_lb(r) == lb_line(2, 1, 0, 0, 1, 1));
        EXPECT(row_id(0) == "ABCDEFGHIJKLMNOPQRST" && row_rank(0) == 14 && row_id(1).empty());
    });
    N("P-B11h stage 3 NULL rows in scan", [lb_setup] {
        lb_setup();
        st.linked = true;
        st.board_state = 1;
        list_state(1, 1, 0, 1, 0, 0, 50);
        list_update();
        st.board_state = 0;
        online_rows(20);
        std::snprintf(st.scores[12].npid, 20, "Me");
        st.null_row.assign(20, false);
        st.null_row[12] = true;
        st.null_row[13] = true;
        st.my_npid = "Me";
        st.rows = 20;
        list_update();
    }, [](const Result &r) {   // the player's row is missing from the answer: no entry, empty page 0
        EXPECT(last_lb(r) == lb_line(0, 0, 0, 0, 1, 1));
        EXPECT(row_id(0).empty());
    });
    // parity-a-leaderboards.md 3, A-F1..A-F4 and A-G3 (c91d21d), new only.
    auto friends_board = [lb_setup](int32_t delta, uint32_t lb_total, std::vector<U2oScore> rows, std::vector<std::string> friends) {
        lb_setup();
        st.linked = true;
        st.board_state = 1;
        if (delta) list_state(1, 1, delta, 1, 1, 0, lb_total);
        else list_state(1, 1, 0, 1, 0, 0, lb_total);   // friends filter changed: page -1
        list_update();
        st.board_state = 0;
        st.scores = std::move(rows);
        st.rows = int(st.scores.size());
        st.friends = std::move(friends);
        st.my_npid = "Me";
        list_update();
    };
    auto srow = [](const char *id, uint32_t rank) { return score_row(id, rank, 1000 - rank, 7, {}, 0); };
    N("A-F1 friends board: ranks 40, 7, unscored, player 12", [friends_board, srow] {
        friends_board(0, 50, {srow("X", 1), srow("F40", 40), srow("Me", 12), srow("F0", 0), srow("F7", 7)}, {"F40", "F0", "F7"});
    }, [](const Result &r) {   // X is no friend; F0 has no score; by rank; total 2 x 3
        EXPECT(last_lb(r) == lb_line(6, 3, 0, 0, 1, 1));
        EXPECT(row_id(0) == "F7" && row_rank(0) == 7 && row_id(1) == "Me" && row_rank(1) == 12 && row_id(2) == "F40" &&
               row_rank(2) == 40 && row_id(3).empty());
    });
    N("A-F2 friends board: 120 friends, 100 used", [friends_board, srow] {
        std::vector<std::string> f;
        for (int i = 0; i < 120; ++i) f.push_back("f" + std::to_string(100 + i));
        friends_board(0, 50, {srow("f105", 5), srow("f210", 3), srow("Me", 9)}, f);   // f210 is the 111th friend
    }, [](const Result &r) {
        EXPECT(requests(r) == std::vector<std::string>({"online::board_request 301 1 0 100 -> 1"}));
        EXPECT(count_of(r, "online::friend_npids 100 -> 120") == 1);
        EXPECT(last_lb(r) == lb_line(4, 2, 0, 0, 1, 1));
        EXPECT(row_id(0) == "f105" && row_id(1) == "Me" && row_id(2).empty());
    });
    auto fifteen = [srow](bool with_me) {   // friends ranked 1..14, the player 15th
        std::vector<U2oScore> rows;
        std::vector<std::string> f;
        for (int i = 1; i <= 14; ++i) {
            f.push_back("g" + std::to_string(i));
            rows.push_back(srow(f.back().c_str(), uint32_t(i)));
        }
        if (with_me) rows.push_back(srow("Me", 15));
        return std::make_pair(rows, f);
    };
    N("A-F3 friends board: page -1, the player 15th", [friends_board, fifteen] {
        auto [rows, f] = fifteen(true);
        friends_board(0, 50, rows, f);
    }, [](const Result &r) {   // page 14 / 11 = 1: 4 rows
        EXPECT(last_lb(r) == lb_line(30, 4, 1, 0, 1, 1));
        EXPECT(row_id(0) == "g12" && row_id(3) == "Me" && row_rank(3) == 15 && row_id(4).empty());
    });
    N("A-F4 friends board: page 5 with 14 kept", [friends_board, fifteen] {
        auto [rows, f] = fifteen(false);
        friends_board(5, 200, rows, f);
    }, [](const Result &r) {   // min(5, 13 / 11) = page 1: 3 rows
        EXPECT(last_lb(r) == lb_line(28, 3, 1, 0, 1, 1));
        EXPECT(row_id(0) == "g12" && row_id(2) == "g14" && row_id(3).empty());
    });
    N("A-F4b friends board: page 5 with 0 kept", [friends_board, srow] {
        friends_board(5, 200, {srow("X", 3)}, {});
    }, [](const Result &r) {   // the page stays 5, empty
        EXPECT(last_lb(r) == lb_line(0, 0, 5, 0, 1, 1));
        EXPECT(row_id(0).empty());
    });
    N("A-G3 global board: total 25 on page 2", [lb_setup] {
        lb_setup();
        st.linked = true;
        st.board_state = 1;
        list_state(0, 0, 2, 0, 0, 0, 50);
        list_update();
        st.board_state = 0;
        online_rows(11);
        st.rows = 11;
        st.total = 25;
        list_update();
    }, [](const Result &r) {   // range from rank 23; rows on page 25 - 22 = 3
        EXPECT(requests(r) == std::vector<std::string>({"online::board_request 300 0 23 11 -> 1"}));
        EXPECT(last_lb(r) == lb_line(25, 3, 2, 0, 0, 0));
        EXPECT(row_id(2) == "user02" && row_id(3).empty());
    });
    S("P-B12 list overlapping LB", [lb_setup] {
        lb_setup();
        put<uint32_t>(LB + 0x80, 1);
        put<uint8_t>(LB + 0x85, 1);
        put<uint8_t>(LB + 0x84, 1);
        put<uint32_t>(LB + 0x24, 30);
        put<int32_t>(LB + 0x74, 1);
        list_update(LB + 0x39);
        put<uint8_t>(LB + 0x78, 1);
        put<uint32_t>(LB + 0x80, 2);
        list_update(LB + 0x39);
    });
    S("P-B13 full online cycle then local after unlink", [lb_setup] {
        lb_setup();
        st.linked = true;
        st.board_state = 1;
        list_state(0, 0, 1, 0, 0, 0, 50);
        list_update();
        st.board_state = 0;
        online_rows(11);
        st.rows = 11;
        st.total = 90;
        list_update();
        st.linked = false;
        put<int32_t>(LIST + 0x44, 1);
        list_update();
    });

    // ---------------- others ----------------
    S("P-O1 publish with image base NULL", [] {
        prog_setup();
        use_store_path();
        connected_state();
        put<uint32_t>(CASH, 5);
        hx::set_image_both(nullptr, 0);
        rec("-> publish");
        A->publish();
        hx::bind_both();
        npd();
    });
    S("P-O1b publish changed / unchanged / not connected", [] {
        prog_setup();
        use_store_path();
        connected_state();
        put<uint32_t>(CASH, 5);
        A->publish();
        npd();
        A->publish();
        npd();
        put<int32_t>(STATE, 2);
        put<uint32_t>(CASH, 6);
        A->publish();
        npd();
    });
    auto recent = [](size_t max) {
        auto v = A->recent(max);
        std::string s;
        for (auto &n : v) s += "[" + n + "]";
        rec("recent(%zu) -> %s", max, s.c_str());
    };
    S("P-O2 recent players", [recent] {
        ps::Store s = make_store();
        add_recent(s, "T5a", 1, 1, 1, 1, 5);
        add_recent(s, "T9", 1, 1, 1, 1, 9);
        add_recent(s, "T5b", 1, 1, 1, 1, 5);
        write_store("progression.bin", s);
        use_store_path();
        recent(2);
        recent(0);
        recent(10);
    });
    S("P-O2b recent players, unreadable store", [recent] {
        write_file("progression.bin", {1});
        use_store_path();
        recent(5);
    });
    S("P-O3 summary", [] {
        ps::Store s = make_store();
        add_recent(s, "Known", 3, 4, 5, 6, 5);
        add_recent(s, std::string(31, 'L'), 3, 4, 5, 6, 5);
        add_recent(s, "Known", 9, 9, 9, 9, 9);   // a second one of the same name
        write_store("progression.bin", s);
        use_store_path();
        for (const std::string n : {std::string("Known"), std::string("Unknown"), std::string(31, 'L'), std::string("known"),
                                    std::string("")})
            rec("summary [%s] -> [%s]", n.c_str(), A->summary(n).c_str());
    });

    // ---------------- Players Met (parity-g-friends.md G1, f11e480), new only ----------------
    auto met = [] {
        std::string s;
        for (auto &n : u2mp::progression_players_met()) s += "[" + n + "]";
        rec("players met -> %s", s.c_str());
    };
    auto met_store = [] {   // local "Stored"; A, C listed; B known from a match's end only; a stale local row
        ps::Store s = make_store();
        add_recent(s, "A", 1, 1, 1, 1, 5);
        add_recent(s, "B", 1, 1, 1, 1, 9);
        s.recent.back().unlisted = true;
        add_recent(s, "Stored", 1, 1, 1, 1, 7);
        add_recent(s, "C", 1, 1, 1, 1, 8);
        write_store("progression.bin", s);
        use_store_path();
    };
    N("G1-1 LAN: listed rows, newest first", [met, met_store] {
        prog_setup();
        met_store();
        met();
    }, [](const Result &r) {
        EXPECT(count_of(r, "players met -> [C][A]") == 1);
        EXPECT(starting(r, "online::is_friend").empty() && starting(r, "online::friend_list_ready").empty());
    });
    N("G1-2 online, friend list not known yet: empty", [met, met_store] {
        prog_setup();
        met_store();
        st.active = true;
        st.friend_list_ready = false;
        met();
    }, [](const Result &r) {
        EXPECT(count_of(r, "players met -> ") == 1);
        EXPECT(count_of(r, "online::friend_list_ready -> 0") == 1);
    });
    N("G1-3 online: a friend is no Players Met row", [met, met_store] {
        prog_setup();
        met_store();
        st.active = true;
        st.friends = {"A"};
        met();
    }, [](const Result &r) {
        EXPECT(count_of(r, "players met -> [C]") == 1);
        EXPECT(count_of(r, "online::is_friend [A] -> 1") == 1 && count_of(r, "online::is_friend [C] -> 0") == 1);
    });
    N("G1-4 at most 50 rows", [] {
        prog_setup();
        ps::Store s = make_store();
        for (int i = 0; i < 60; ++i) add_recent(s, "M" + std::to_string(10 + i), 1, 1, 1, 1, uint64_t(100 + i));
        write_store("progression.bin", s);
        use_store_path();
        const auto v = u2mp::progression_players_met();
        rec("players met %zu first [%s] last [%s]", v.size(), v.empty() ? "" : v.front().c_str(), v.empty() ? "" : v.back().c_str());
    }, [](const Result &r) { EXPECT(count_of(r, "players met 50 first [M69] last [M20]") == 1); });
    N("G1-5 match start: strangers added, store written", [met, met_store] {
        prog_setup();
        met_store();
        st.active = true;
        st.friends = {"F"};
        rec("-> note_met");
        u2mp::progression_note_met({"X", "Stored", "A", "", "F", "Y", "X", "B"});
        met();
    }, [](const Result &r) {   // not the player, a listed row, an empty name, a friend or a name twice; B listed again
        EXPECT(starting(r, "log progression: adding player") ==
               std::vector<std::string>({"log progression: adding player X to Players Met",
                                         "log progression: adding player Y to Players Met",
                                         "log progression: adding player B to Players Met"}));
        EXPECT(count_of(r, "players met -> [B][X][Y][C][A]") == 1);   // seen now; equal times keep the store's order
        EXPECT(count_of(r, "  recent [X] k0 d0 w0 l0 s0 c0 m0 n0 r0 seen1700000000") == 1);
    });
    N("G1-6 a match's end lists nobody; the next match start does", [met] {
        prog_setup();
        use_store_path();
        connected_state();
        match(two(), 5, 3, 1, 0);
        leave();
        met();
        rec("-> note_met");
        u2mp::progression_note_met({"Rival"});
        met();
    }, [](const Result &r) {
        EXPECT(count_of(r, "players met -> ") == 1 && count_of(r, "players met -> [Rival]") == 1);
        EXPECT(count_of(r, "log progression: adding player Rival to Players Met") == 1);
    });
    N("G1-7 at most 16 per match start", [met] {
        prog_setup();
        use_store_path();
        std::vector<std::string> names;
        for (int i = 0; i < 17; ++i) names.push_back("N" + std::to_string(10 + i));
        u2mp::progression_note_met(names);
    }, [](const Result &r) {
        EXPECT(starting(r, "log progression: adding player").size() == 16);
        EXPECT(count_of(r, "log progression: adding player N26 to Players Met") == 0);
    });

    // ---------------- netinit ----------------
    S("N-1 all checks, empty state", [] {
        ni_image();
        boosters(3, {0, 5, 0});
        prepare();
    });
    for (size_t c = 0; c < kNiChecks.size(); ++c)
        S("N-2 check " + std::to_string(c) + " differs", [c] {
            ni_image();
            boosters(3, {0, 5, 0});
            mem[kNiChecks[c].at + kNiChecks[c].b.size() - 1] ^= 0x10;
            if (c == kNiChecks.size() - 1) mem[0xa15300 + 15] = 0x01;
            prepare();
            nmatch();
        });
    S("N-3 two checks differ", [] {
        ni_image();
        mem[0x5f6750] = 0;
        mem[0x641012] = 0;
        prepare();
        prepare();
    });
    S("N-4 second call", [] {
        ni_image();
        boosters(3, {0, 5, 0});
        prepare();
        put<uint8_t>(0x1065bd0, 0);
        put<uint8_t>(0x105f080, 0);
        nmatch();
    });
    S("N-5 table A buckets set, ready 0", [] {
        ni_image();
        put<uint64_t>(TA, addr(0x1500000));
        put<uint64_t>(TA + 0x48, 0x1234);
        put<uint32_t>(TA + 0x38, 0x5678);
        prepare();
    });
    S("N-5b table A buckets 0, ready 1", [] {
        ni_image();
        put<uint8_t>(TA + 0x40, 1);
        prepare();
    });
    S("N-5c table B already set up", [] {
        ni_image();
        put<uint64_t>(TB, addr(0x1500000));
        put<uint8_t>(TB + 0x40, 7);
        prepare();
    });
    S("N-6 alloc NULL for table A", [] {
        ni_image();
        st.heap_fail_mask = 1;
        prepare();
        prepare();
    });
    S("N-6b alloc NULL for both, then retried", [] {
        ni_image();
        st.heap_fail_mask = 3;
        prepare();
        prepare();
    });
    S("N-7 booster count already non-zero", [] {
        ni_image();
        boosters(3, {1, 1, 1});
        put<uint32_t>(BOOST, 4);
        prepare();
        put<uint32_t>(BOOST, 0);
        prepare();
    });
    S("N-8 DcFind NULL then found", [] {
        ni_image();
        prepare();
        boosters(2, {0, 0});
        prepare();
        prepare();
    });
    S("N-9 slot holds NULL", [] {
        ni_image();
        g_find[0x916e03b0] = BSLOT;
        prepare();
        prepare();
    });
    S("N-10a n 0", [] { ni_image(); boosters(0, {1}); prepare(); prepare(); });
    S("N-10b recs NULL", [] { ni_image(); boosters(3, {1}, true); prepare(); prepare(); });
    S("N-10c n negative", [] { ni_image(); boosters(-2, {1}); prepare(); boosters(1, {0}); prepare(); });
    S("N-11a all masks 0", [] { ni_image(); boosters(3, {0, 0, 0}); prepare(); });
    S("N-11b 70 masked records", [] { ni_image(); boosters(70, std::vector<uint32_t>(70, 2)); prepare(); });
    S("N-11c 70 records masks 0", [] { ni_image(); boosters(70, std::vector<uint32_t>(70, 0)); prepare(); });
    S("N-11d 70 records alternating", [] {
        ni_image();
        std::vector<uint32_t> m(70);
        for (size_t i = 0; i < m.size(); ++i) m[i] = i % 2;
        boosters(70, m);
        prepare();
    });
    S("N-11e 130 records, last masked", [] {
        ni_image();
        std::vector<uint32_t> m(130, 0);
        m[129] = 1;
        m[0] = 0;
        boosters(130, m);
        prepare();
    });
    S("N-12 history once", [] {
        ni_image();
        prepare();
        put<uint64_t>(0x113d402, 0);
        nmatch();
    });
    S("N-13 dump", [] {
        ni_image();
        rec("-> dump");
        A->ni_dump("before");
        prepare();
        A->ni_dump("built");
        put<uint64_t>(TA + 0x30, get<uint64_t>(TA + 0x30) + 8);
        put<int32_t>(TB + 0x3c, 5000);
        A->ni_dump("broken");
    });
    S("N-13b dump custom tables", [] {
        ni_image();
        std::memset(mem + 0x1500000, 0xff, 0x40);
        put<uint64_t>(0x1500008, 0);
        put<uint64_t>(0x1500010, 5);
        put<uint64_t>(TA, addr(0x1500000));
        put<int32_t>(TA + 0x3c, 6);
        put<uint64_t>(TA + 0x30, addr(0x1500030));
        put<uint64_t>(TA + 8, 0x11);
        put<uint64_t>(TA + 0x18, 0x22);
        put<uint64_t>(TA + 0x20, 0x33);
        put<uint8_t>(TA + 0x40, 9);
        put<uint64_t>(TB, addr(0x1500000));
        put<int32_t>(TB + 0x3c, 0);
        put<uint64_t>(TB + 0x30, addr(0x1500000));
        A->ni_dump("x");
        put<int32_t>(TB + 0x3c, -1);
        put<uint64_t>(TB + 0x30, addr(0x1500000) - 8);
        put<int32_t>(TA + 0x3c, 4096);
        A->ni_dump("y");
        put<int32_t>(TA + 0x3c, 4097);
        put<uint64_t>(TB, 0);
        put<uint64_t>(TB + 0x30, 0);
        A->ni_dump("z");
    });
    S("N-14 match first", [] { ni_image(); boosters(2, {3, 0}); nmatch(); prepare(); });
    // ---------------- netinit additions of 934a6ca (see ni_expected), new only ----------------
    N("N-G1 player-setup slot reset made NOPs, once", [] { ni_image(); boosters(3, {0, 5, 0}); prepare(); prepare(); },
      [](const Result &r) {
          EXPECT(count_of(r, "log netinit: the player-setup event keeps the joiner's slots") == 1);
          EXPECT(std::memcmp(mem + 0x632148, "\x90\x90\x90\x90\x90", 5) == 0);
      });
    N("N-G2 player-setup check differs: nothing done", [] { ni_image(); mem[0x632148] = 0xe9; prepare(); },
      [](const Result &r) {
          EXPECT(count_of(r, "log netinit: bytes at 0x632148 (player-setup slot reset) differ; nothing done") == 1);
          EXPECT(starting(r, "netheap_alloc").empty() && starting(r, "log netinit: 0 net bit writer").empty());
          EXPECT(mem[0x632148] == 0xe9);
      });
    // a net bit writer site of 1.02 (0x5e1434: mov eax,-1; mov cl,r8b; shl eax,cl) and its fix (or rax,-1; shl rax,cl)
    static const uint8_t kSite[10] = {0xb8, 0xff, 0xff, 0xff, 0xff, 0x44, 0x88, 0xc1, 0xd3, 0xe0};
    static const uint8_t kFixed[10] = {0x44, 0x88, 0xc1, 0x48, 0x83, 0xc8, 0xff, 0x48, 0xd3, 0xe0};
    for (int sites : {342, 1})
        N("N-G3 net bit writers, " + std::to_string(sites) + " site(s)", [sites] {
            ni_image();
            for (int i = 0; i < sites; ++i) putb(0x5e2000 + 16 * uint64_t(i), kSite, sizeof kSite);
            prepare();
            nmatch();
        }, [sites](const Result &r) {
            const char *want = sites == 342 ? "log netinit: net bit writer word masks fixed (342 of 342)"
                                            : "log netinit: 1 net bit writer sites, not the 342 of 1.02: masks left alone";
            EXPECT(count_of(r, want) == 1 && starting(r, "log netinit: ").size() >= 1);
            int fixed = 0;
            for (int i = 0; i < sites; ++i) fixed += std::memcmp(mem + 0x5e2000 + 16 * i, kFixed, 10) == 0;
            EXPECT(fixed == (sites == 342 ? 342 : 0));
        });
    for (uint32_t heap : {0x20000u, 0x40000u})
        N(heap == 0x20000 ? "N-G4 NetCharacter heap 128 -> 512 KiB" : "N-G4b NetCharacter heap not 128 KiB: kept", [heap] {
            ni_image();
            put<uint32_t>(0x14cee28 + 0x10, 0xf2c86c22);   // NetCharacter's process type node: id, heap size
            put<uint32_t>(0x14cee28 + 0x18, heap);
            prepare();
        }, [heap](const Result &r) {
            EXPECT(count_of(r, "log netinit: NetCharacter process heap 128 -> 512 KiB") == (heap == 0x20000 ? 1u : 0u));
            EXPECT(get<uint32_t>(0x14cee28 + 0x18) == (heap == 0x20000 ? 0x80000u : heap));
        });
    N("N-G5 foreground meshes shown", [] {
        ni_image();
        putb(0x5f228d, reinterpret_cast<const uint8_t *>("\x41\xc6\x87\xbf\x00\x00\x00\x01"), 8);   // 1.02 bytes
        prepare();
    }, [](const Result &r) {
        EXPECT(count_of(r, "log netinit: foreground meshes shown (0x5f228d stores 0)") == 1);
        EXPECT(std::memcmp(mem + 0x5f228d, "\x41\xc6\x87\xbf\x00\x00\x00\x00", 8) == 0);
    });

    // ---------------- netpatch ----------------
    S("X-I1 install all match", [] { np_image(); np_install(); });
    S("X-I2 BindSymbol differs, then load", [] {
        np_image();
        mem[0xa16d70 + 23] = 1;
        np_install();
        np_load();
        np_load();
    });
    S("X-I2b RelocateModule differs", [] { np_image(); mem[0xa19a00] = 0; np_install(); np_load(); });
    S("X-I2c DcFind differs", [] { np_image(); mem[0xa15300 + 9] = 0; np_install(); });
    for (uint64_t a : {0xa19a00ull, 0xa16d70ull, 0xa15300ull})
        S("X-I2d not executable " + std::to_string(a), [a] {
            np_image();
            hx::noexec = {{a + 3, 1}};
            np_install();
        });
    S("X-0 load before install", [] { np_image(); np_load(); });
    S("X-1 off", [] {
        np_image();
        np_install();
        write_module(module({0x1000}));
        _putenv_s("UNCHARTED_U2MP_NETPATCH", "off");
        np_load();
        _putenv_s("UNCHARTED_U2MP_NETPATCH", "");
        np_load();
    });
    S("X-1b ALL is not all", [] {
        np_image();
        np_install();
        g_find[0x2000] = 0x16a0000;
        write_module(module({0x1000, 0x2000}));
        _putenv_s("UNCHARTED_U2MP_NETPATCH", "ALL");
        np_load();
    });
    S("X-2 hash buckets NULL, then set", [] {
        np_image();
        np_install();
        write_module(module({0x1000, 0x8cf8e1cc}));
        put<uint64_t>(0x15340d0, 0);
        np_load();
        put<uint64_t>(0x15340d0, addr(0x1680000));
        np_load();
        np_load();
    });
    S("X-2b bucket count 0 / negative", [] {
        np_image();
        np_install();
        write_module(module({0x1000}));
        put<int32_t>(0x153410c, 0);
        np_load();
        put<int32_t>(0x153410c, -4);
        np_load();
        put<int32_t>(0x153410c, 1);
        np_load();
    });
    S("X-2c not set up, then env off", [] {
        np_image();
        np_install();
        write_module(module({0x1000}));
        put<uint64_t>(0x15340d0, 0);
        np_load();
        _putenv_s("UNCHARTED_U2MP_NETPATCH", "off");
        np_load();
        np_load();
    });
    S("X-2d off with table NULL", [] {
        np_image();
        np_install();
        put<uint64_t>(0x15340d0, 0);
        _putenv_s("UNCHARTED_U2MP_NETPATCH", "off");
        np_load();
        np_load();
    });
    S("X-3a translate fails", [] {
        np_image();
        np_install();
        write_module(module({0x1000}));
        st.translate_result = -1;
        np_load();
        st.translate_result = 0;
        np_load();
    });
    S("X-3b file missing", [] { np_image(); np_install(); np_load(); np_load(); });
    struct Bad {
        const char *name;
        std::function<void(std::vector<uint8_t> &)> f;
    };
    auto set64 = [](std::vector<uint8_t> &v, size_t o, uint64_t x) { std::memcpy(v.data() + o, &x, 8); };
    auto set32 = [](std::vector<uint8_t> &v, size_t o, uint32_t x) { std::memcpy(v.data() + o, &x, 4); };
    auto R = [](const std::vector<uint8_t> &v) {
        uint64_t r;
        std::memcpy(&r, v.data() + 8, 8);
        return r;
    };
    const std::vector<Bad> bad = {
        {"Z 0x3f", [](auto &v) { v.resize(0x3f); }},
        {"Z 0x40 minimal", [](auto &v) { v.resize(0x40); }},
        {"wrong magic", [](auto &v) { v[3] = 'E'; }},
        {"R 0x20", [set64](auto &v) { set64(v, 8, 0x20); }},
        {"R 0x28", [set64, set32](auto &v) { set64(v, 8, 0x28); set32(v, 0x28, 1); }},
        {"R 0x2c", [set64](auto &v) { set64(v, 8, 0x2c); }},
        {"R + 4 > Z", [set64](auto &v) { set64(v, 8, (v.size() + 7) & ~size_t(7)); }},
        {"R huge", [set64](auto &v) { set64(v, 8, 0xfffffffffffffff8ull); }},
        {"L too large", [set32, R](auto &v) { set32(v, R(v), 1000); }},
        {"L*64 < R", [set32, R](auto &v) { set32(v, R(v), 0); }},
        {"root 0x28", [set64](auto &v) { set64(v, 0x10, 0x28); }},
        {"wrong root type", [set32](auto &v) { set32(v, 0x1c, 0x916aa604); }},
        {"C too large", [set32](auto &v) { set32(v, 0x24, 1000); }},
        {"C max that fits", [set32, R](auto &v) { set32(v, 0x24, uint32_t((R(v) - 0x28) / 16)); }},
        {"C huge", [set32](auto &v) { set32(v, 0x24, 0xffffffffu); }},
        {"marked slot value >= R", [set64, R](auto &v) { set64(v, 0x30, R(v)); }},
        {"marked slot value R-1", [set64, R](auto &v) { set64(v, 0x30, R(v) - 1); }},
        {"marked header slot value", [R](auto &v) { v[R(v) + 4] |= 1; }},
        {"marked slot 1 (R) is >= R", [R](auto &v) { v[R(v) + 4] |= 2; }},
    };
    for (const auto &b : bad)
        S(std::string("X-4 validation ") + b.name, [b] {
            np_image();
            np_install();
            auto v = module({0x1000, 0x1001, 0x1002});
            b.f(v);
            write_module(v);
            np_load();
            np_load();
        });
    S("X-4b marked slot at R", [] {
        np_image();
        np_install();
        auto v = module({0x1000, 0x1001, 0x1002}, 1);
        uint64_t r;
        std::memcpy(&r, v.data() + 8, 8);
        v[r + 4 + (r / 8) / 8] |= uint8_t(1u << ((r / 8) % 8));
        write_module(v);
        np_load();
    });
    S("X-4c bitmap byte past the slots (unmarked)", [] {
        np_image();
        np_install();
        write_module(module({0x1000}, 3));
        np_load();
    });
    S("X-5a relocate returns another slot", [] {
        np_image();
        np_install();
        write_module(module({0x1000}));
        g_reloc_mode = 1;
        np_load();
        np_load();
    });
    S("X-5b relocate leaves the root slot", [] {
        np_image();
        np_install();
        write_module(module({0x1000}));
        g_reloc_mode = 2;
        np_load();
    });
    S("X-6 entries", [] {
        np_image();
        np_install();
        g_find[0x2000] = 0x16a0000;
        put<uint64_t>(0x16a0000, 0x9999);
        write_module(module({0x87b03989, 0x2000, 0x3000, 0x3001, 0x8cf8e1cc}));
        np_load();
        np_load();
    });
    S("X-6b weapon set stays missing (kept count, only script)", [] {
        np_image();
        np_install();
        write_module(module({0x87b03989}));
        np_load();
    });
    S("X-7 online weapon settings to offline map", [] {
        np_image();
        np_install();
        g_find[0x974cf651] = 0x16a0100;
        put<uint64_t>(0x16a0100, addr(0x16a0200));
        write_module(module({0x837a26b6, 0x3000}));
        np_load();
    });
    S("X-7b offline map also undefined", [] {
        np_image();
        np_install();
        write_module(module({0x837a26b6, 0x3000}));
        np_load();
    });
    S("X-7c online weapon settings defined", [] {
        np_image();
        np_install();
        g_find[0x837a26b6] = 0x16a0300;
        g_find[0x974cf651] = 0x16a0100;
        write_module(module({0x837a26b6}));
        np_load();
    });
    S("X-7d offline map with pool full", [] {
        np_image(10);
        np_install();
        g_find[0x974cf651] = 0x16a0100;
        put<uint64_t>(0x16a0100, 0x4242);
        write_module(module({0x3000, 0x837a26b6}));
        np_load();
    });
    S("X-8 all mode", [] {
        np_image();
        np_install();
        g_find[0x2000] = 0x16a0000;
        g_find[0x837a26b6] = 0x16a0300;
        write_module(module({0x87b03989, 0x2000, 0x3000, 0x837a26b6}));
        _putenv_s("UNCHARTED_U2MP_NETPATCH", "all");
        np_load();
    });
    S("X-8b all mode, pool tiny: defined bound, undefined not", [] {
        np_image(3);
        np_install();
        g_find[0x2000] = 0x16a0000;
        write_module(module({0x2000, 0x3000, 0x3001}));
        _putenv_s("UNCHARTED_U2MP_NETPATCH", "all");
        np_load();
    });
    for (int nodes : {50, 0, 48, 49, 200})
        S("X-9 free list " + std::to_string(nodes) + ", 10 undefined", [nodes] {
            np_image(nodes);
            np_install();
            std::vector<uint32_t> sids;
            for (uint32_t i = 0; i < 10; ++i) sids.push_back(0x3000 + i);
            write_module(module(sids));
            np_load();
        });
    S("X-9b cyclic free list", [] {
        np_image(5);
        np_install();
        free_list(5, true);
        write_module(module({0x3000, 0x3001}));
        np_load();
    });
    S("X-9c empty module", [] {
        np_image();
        np_install();
        write_module(module({}));
        np_load();
    });
    S("X-10 after success", [] {
        np_image();
        np_install();
        write_module(module({0x3000, 0x3001}));
        np_load();
        hx::calls.push_back("-- second");
        np_load();
        put<uint64_t>(0x15340d0, 0);
        np_load();
    });
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    define();
    g_base = fs::absolute(fs::path(argv[0])).parent_path();
    if (argc >= 2) {
        hx::init();
        return run_one(argv[1], argc >= 3);
    }
    int fails = 0, n = 0;
    for (const auto &sc : scenarios) {
        ++n;
        const intptr_t rc = _spawnl(_P_WAIT, argv[0], argv[0], ("\"" + sc.name + "\"").c_str(), nullptr);
        if (rc != 0) {
            ++fails;
            if (rc != 1) std::printf("CRASH %s (exit %lld)\n", sc.name.c_str(), (long long)rc);
        }
    }
    std::printf("progression: %d scenarios, %d failed\n", n, fails);
    return fails ? 1 : 0;
}
