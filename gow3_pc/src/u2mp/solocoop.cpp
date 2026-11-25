// u2mp solo matches (owner rule 2026-11-25: custom games start with 1 player in every game mode; the minimum of 2
// is for matchmaking only). Two parts in the PS4 NetGameManager host update (0x63e6a0):
// - co-op (types 4/8/9/12) requires two players in the PS4 build; lower the check to one (solo co-op).
// - Elimination (type 10) outside matchmaking ends the match when only one team has players, and ends a round
//   5 s after it starts when only one team is alive. A custom game begun by one player has one team only, so it
//   ended at once. In such a solo custom game both checks are skipped: the rounds run to their time limit (or end
//   when the player dies) and the match ends by its round count, as with two teams. Rules: start_rules.h.
// Spec: u2mp/docs/spec/parity-n-inlined.md 4 (NI2).
#include <cstring>
#include <initializer_list>

#include "start_rules.h"
#include "u2mp_internal.h"

namespace u2mp {
namespace {

constexpr uint64_t kCoopMinimum = 0x63f60b;
constexpr unsigned char kTwoPlayers[] = {0xb9, 0x02, 0x00, 0x00, 0x00};
constexpr unsigned char kOnePlayer[] = {0xb9, 0x01, 0x00, 0x00, 0x00};

// Game-over test "one team left": cmp r8d, 10 (the game type); jne past it. Outside matchmaking only type 10 runs it.
constexpr uint64_t kOneTeamGameOver = 0x63f48b;
constexpr uint64_t kOneTeamLoop = 0x63f491;     // the team test itself (ends the match with reason 2)
constexpr uint64_t kAfterOneTeam = 0x63f4e7;    // the checks after it
constexpr unsigned char kOneTeamGameOverBytes[] = {0x41, 0x83, 0xf8, 0x0a, 0x75, 0x56};
// Elimination round test after its alive count: mov edi, 2; test dl, 1 (two teams alive); je round-over call.
// esi = the team of the living players (-1 none).
constexpr uint64_t kRoundOneTeam = 0x63f837;
constexpr uint64_t kRoundTimeTest = 0x63f841;   // both teams alive: the round time limit test
constexpr uint64_t kRoundOverCall = 0x63f8a4;   // call to the round-over sender (reason edi, winner esi)
constexpr unsigned char kRoundOneTeamBytes[] = {0xbf, 0x02, 0x00, 0x00, 0x00, 0xf6, 0xc2, 0x01, 0x74, 0x63};

constexpr uint64_t kNetMode = 0x104f1c8 + 0x2c4;   // netInfo page mode: 1 matchmaking, 2 custom game
constexpr uint64_t kNgm = 0x113b760;
constexpr uint64_t kStartPlayers = kNgm + 0x1b90;   // players in the match's start event
constexpr uint64_t kMatchStartMark = kNgm + 0x1d5c; // clock mark of the match start
constexpr uint64_t kRoundStartMark = kNgm + 0x1d60; // clock mark of the round start
constexpr uint64_t kRoundsPlayed = kNgm + 0x1d74;

template <typename T> T peek(uint64_t offset) {
    T v;
    std::memcpy(&v, image.base + offset, sizeof v);
    return v;
}

U2MP_GUEST bool one_team_ends_game(int32_t type) {
    const int32_t mode = peek<int32_t>(kNetMode), players = peek<int32_t>(kStartPlayers);
    const bool ends = start::one_team_ends_game(mode, type, players);
    static uint32_t logged = 0;
    if (!ends && type == start::kElimination && peek<uint32_t>(kMatchStartMark) != logged) {
        logged = peek<uint32_t>(kMatchStartMark);
        log("solo custom game: Elimination with %d player(s) at the start, one team does not end the match",
            int(players));
    }
    return ends;
}

U2MP_GUEST bool round_runs_to_time(int32_t alive_team) {
    const bool runs = start::round_runs_to_time(peek<int32_t>(kNetMode), peek<int32_t>(kStartPlayers), alive_team);
    static uint32_t logged = 0;
    if (runs && peek<uint32_t>(kRoundStartMark) != logged) {
        logged = peek<uint32_t>(kRoundStartMark);
        log("solo custom game: Elimination round %d runs to its time limit", int(peek<int32_t>(kRoundsPlayed)) + 1);
    }
    return runs;
}

// Small code writer for the two detours (machine code next to the image, rel32 jumps back into it).
struct Code {
    unsigned char *p;
    size_t n = 0;
    bool ok = true;
    void put(std::initializer_list<unsigned char> b) {
        for (const unsigned char c : b) p[n++] = c;
    }
    void call(void *fn) {   // movabs rax, fn; call rax
        const uint64_t a = uint64_t(uintptr_t(fn));
        put({0x48, 0xb8});
        std::memcpy(p + n, &a, 8);
        n += 8;
        put({0xff, 0xd0});
    }
    void jump(uint64_t image_offset) {   // jmp rel32 into the image
        const int64_t rel = int64_t(uintptr_t(image.base + image_offset)) - int64_t(uintptr_t(p + n + 5));
        ok = ok && rel >= INT32_MIN && rel <= INT32_MAX;
        const int32_t r = int32_t(rel);
        put({0xe9});
        std::memcpy(p + n, &r, 4);
        n += 4;
    }
};

// Site -> jmp stub, the rest of the replaced bytes nop.
bool detour(uint64_t site, size_t length, const unsigned char *stub) {
    const int64_t rel = int64_t(uintptr_t(stub)) - int64_t(uintptr_t(image.base + site + 5));
    if (rel < INT32_MIN || rel > INT32_MAX) return false;
    const int32_t r = int32_t(rel);
    image.base[site] = 0xe9;
    std::memcpy(image.base + site + 1, &r, 4);
    std::memset(image.base + site + 5, 0x90, length - 5);
    return true;
}

bool install_solo_elimination() {
    if (!image.executable(kOneTeamGameOver, sizeof kOneTeamGameOverBytes) ||
        !image.matches(kOneTeamGameOver, kOneTeamGameOverBytes, sizeof kOneTeamGameOverBytes) ||
        !image.executable(kRoundOneTeam, sizeof kRoundOneTeamBytes) ||
        !image.matches(kRoundOneTeam, kRoundOneTeamBytes, sizeof kRoundOneTeamBytes)) {
        log("solo custom game: Elimination one-team checks at 0x%llx / 0x%llx differ, left alone",
            (unsigned long long)kOneTeamGameOver, (unsigned long long)kRoundOneTeam);
        return false;
    }
    unsigned char *a = near_stub(48), *b = near_stub(64);
    if (!a || !b) {
        log("solo custom game: no stub memory near the image");
        return false;
    }
    // Game over: the team test runs when one_team_ends_game(type) says so.
    Code ca{a};
    ca.put({0x44, 0x89, 0xc7});   // mov edi, r8d
    ca.call(reinterpret_cast<void *>(&one_team_ends_game));
    ca.put({0x84, 0xc0, 0x75, 0x05});   // test al, al; jne +5
    ca.jump(kAfterOneTeam);
    ca.jump(kOneTeamLoop);
    // Round: one team alive -> round over, unless round_runs_to_time(team); eax (alive balance) and esi are kept.
    Code cb{b};
    cb.put({0xbf, 0x02, 0x00, 0x00, 0x00, 0xf6, 0xc2, 0x01, 0x75, 0x22});   // mov edi, 2; test dl, 1; jne time
    cb.put({0x50, 0x56, 0x89, 0xf7});                                       // push rax; push rsi; mov edi, esi
    cb.call(reinterpret_cast<void *>(&round_runs_to_time));
    cb.put({0x89, 0xc1, 0x5e, 0x58});                                       // mov ecx, eax; pop rsi; pop rax
    cb.put({0xbf, 0x02, 0x00, 0x00, 0x00, 0x84, 0xc9, 0x75, 0x05});         // mov edi, 2; test cl, cl; jne time
    cb.jump(kRoundOverCall);
    const size_t time = cb.n;
    cb.jump(kRoundTimeTest);
    if (time != 10 + 0x22 || !ca.ok || !cb.ok) {
        log("solo custom game: stub out of jump range");
        return false;
    }
    if (!detour(kOneTeamGameOver, sizeof kOneTeamGameOverBytes, a) ||
        !detour(kRoundOneTeam, sizeof kRoundOneTeamBytes, b)) {
        log("solo custom game: stub out of jump range");
        return false;
    }
    log("solo custom game: Elimination one-team game over / round end skipped in a custom game begun alone");
    return true;
}

}  // namespace

void install_solocoop(int &installed) {
    if (!image.base) return;
    if (!image.executable(kCoopMinimum, sizeof kTwoPlayers) ||
        !image.matches(kCoopMinimum, kTwoPlayers, sizeof kTwoPlayers)) {
        log("solo co-op: minimum-player check at 0x%llx not found, left alone", (unsigned long long)kCoopMinimum);
    } else {
        std::memcpy(image.base + kCoopMinimum, kOnePlayer, sizeof kOnePlayer);
        log("solo co-op: co-op minimum players 2 -> 1");
        installed += 1;
    }
    if (install_solo_elimination()) installed += 1;
}

}  // namespace u2mp
