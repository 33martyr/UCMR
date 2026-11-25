// u2mp teams: the per-player values the team balance of a net game reads (spec parity-e-flow2.md 2.6).
//
// NetGameManager::AssignTeams (PS4 0x6400d0, PS3 0x4a2490) balances the players of the 10 tracker slots (NGM + 0x10 +
// k * 0x2c0). It reads two values per slot whose PS3 sources (the room's slot table and the room members' stats) are
// gone on PS4:
//
// 1. Level (0x6405a1..0x6405b5). PS3 took the slot's room member's level (member stats +8; a skill value for a ranked
//    index). PS4 has no member stats: remaining.cpp points the read at the match session's one block, so every player
//    got the same value and the teams came out in join order. The read now takes the slot's level from a table that
//    session.cpp fills every frame (the member's level, 1 for the lowest rank; 1 for an empty slot, like the block).
//    The skill a ranked index chose on PS3 was kept by Sony's servers: PC has none, so the level stands for it.
// 2. Party group (the store at 0x640884, in the path without team choices). PS3 read each slot's party id from the
//    host's slot reservations (0 for a player without one) and kept a party on one team where it fits. PS4 stores the
//    removed session member list's word there (always 0: one group). The store now writes the slot's party id from a
//    table session.cpp fills from its reservations (0 without one), in every mode that takes this path.
//
// Both are byte-checked; a site whose bytes differ is logged and left alone.
#include <cstdint>
#include <cstring>

#include "u2mp_internal.h"

namespace u2mp {
namespace {

constexpr int kTeamSlots = 10;
constexpr uint64_t kTrackerStride = 0x2c0;
// The level read's index register (rdx) is the slot's offset kLevelBase + k * kTrackerStride.
constexpr uint64_t kLevelBase = 0x27c;
constexpr uint64_t kLevelTableSize = kLevelBase + kTeamSlots * kTrackerStride;

constexpr uint64_t cLevelRead = 0x6405a1, cLevelKeep = 0x6405b3, cLevelNext = 0x6405b6;
constexpr uint64_t cGroupStore = 0x640884, cGroupNext = 0x64088c;

alignas(64) uint8_t slot_levels[kLevelTableSize];   // only kLevelBase + k * kTrackerStride is used; 0 reads as 1
alignas(64) int32_t slot_groups[16];                  // slot k's party group (k < 10)

bool jump_from(uint64_t site, size_t length, const unsigned char *stub) {
    const int64_t delta = int64_t(uintptr_t(stub)) - int64_t(uintptr_t(image.base + site + 5));
    if (delta < INT32_MIN || delta > INT32_MAX) return false;
    unsigned char *c = image.base + site;
    const int32_t rel = int32_t(delta);
    c[0] = 0xe9;
    std::memcpy(c + 1, &rel, 4);
    std::memset(c + 5, 0x90, length - 5);
    return true;
}

void put_back_jump(unsigned char *code, size_t at, uint64_t target) {
    static const unsigned char jmp[] = {0xff, 0x25, 0, 0, 0, 0};   // jmp [rip]
    std::memcpy(code + at, jmp, sizeof jmp);
    const uint64_t back = uint64_t(uintptr_t(image.base + target));
    std::memcpy(code + at + sizeof jmp, &back, 8);
}

// 0x6405a1: test r9d,r9d; js +9; vcvttss2si rdi,[rdi+r8*4+0x14]; jmp +4; movzx edi,byte [rdi+8]; mov rbx,rax
bool install_level_read() {
    static const unsigned char site[] = {0x45, 0x85, 0xc9, 0x78, 0x09, 0xc4, 0xa1, 0xfa, 0x2c, 0x7c, 0x87,
                                         0x14, 0xeb, 0x04, 0x0f, 0xb6, 0x7f, 0x08, 0x48, 0x89, 0xc3};
    static_assert(sizeof site == cLevelNext - cLevelRead, "level read site");
    if (!image.executable(cLevelRead, sizeof site) || !image.matches(cLevelRead, site, sizeof site)) {
        log("team balance levels (6405a1): bytes differ, left alone");
        return false;
    }
    unsigned char *stub = near_stub(64);
    if (!stub) {
        log("team balance levels (6405a1): no stub memory in range");
        return false;
    }
    unsigned char code[] = {
        0x48, 0xbf, 0, 0, 0, 0, 0, 0, 0, 0,   // mov rdi, slot_levels
        0x48, 0x81, 0xfa, 0, 0, 0, 0,         // cmp rdx, kLevelTableSize
        0x73, 0x08,                           // jae one
        0x0f, 0xb6, 0x3c, 0x17,               // movzx edi, byte [rdi+rdx]
        0x85, 0xff,                           // test edi, edi
        0x75, 0x05,                           // jnz done
        0xbf, 0x01, 0x00, 0x00, 0x00,         // one: mov edi, 1
        0x45, 0x85, 0xc9,                     // done: test r9d, r9d  (the flags the original leaves)
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};   // jmp [rip] -> 0x6405b3 (mov rbx, rax stays in place)
    static_assert(sizeof code == 49, "level stub layout");
    const uint64_t table = uint64_t(uintptr_t(slot_levels));
    const uint32_t size = uint32_t(kLevelTableSize);
    std::memcpy(code + 2, &table, 8);
    std::memcpy(code + 13, &size, 4);
    put_back_jump(code, 35, cLevelKeep);
    std::memcpy(stub, code, sizeof code);
    if (!jump_from(cLevelRead, cLevelKeep - cLevelRead, stub)) {
        log("team balance levels (6405a1): stub out of jump range");
        return false;
    }
    log("team balance levels (6405a1): each slot's own level (PS3 member stats), not one shared block");
    return true;
}

// 0x640884: mov [rbx+r14+0x134], eax (r13 = the slot index; eax is set again at 0x64089d)
bool install_group_store() {
    static const unsigned char site[] = {0x42, 0x89, 0x84, 0x33, 0x34, 0x01, 0x00, 0x00};
    static_assert(sizeof site == cGroupNext - cGroupStore, "group store site");
    if (!image.executable(cGroupStore, sizeof site) || !image.matches(cGroupStore, site, sizeof site)) {
        log("team balance party groups (640884): bytes differ, left alone");
        return false;
    }
    unsigned char *stub = near_stub(48);
    if (!stub) {
        log("team balance party groups (640884): no stub memory in range");
        return false;
    }
    // No flag-changing instruction: the code after the store may still test what came before it.
    unsigned char code[] = {
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,               // mov rax, slot_groups
        0x42, 0x8b, 0x04, 0xa8,                           // mov eax, [rax+r13*4]
        0x42, 0x89, 0x84, 0x33, 0x34, 0x01, 0x00, 0x00,   // mov [rbx+r14+0x134], eax   (original)
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};        // jmp [rip] -> 0x64088c
    static_assert(sizeof code == 36, "group stub layout");
    const uint64_t table = uint64_t(uintptr_t(slot_groups));
    std::memcpy(code + 2, &table, 8);
    put_back_jump(code, 22, cGroupNext);
    std::memcpy(stub, code, sizeof code);
    if (!jump_from(cGroupStore, sizeof site, stub)) {
        log("team balance party groups (640884): stub out of jump range");
        return false;
    }
    log("team balance party groups (640884): each slot's party id from the host's slot reservations (PS3 2.6)");
    return true;
}

}  // namespace

void teams_set_slot(int slot, uint32_t party, uint32_t level) {
    if (slot < 0 || slot >= kTeamSlots) return;
    slot_groups[slot] = int32_t(party);
    slot_levels[kLevelBase + uint64_t(slot) * kTrackerStride] = uint8_t(level > 255 ? 255 : level);
}

void install_teams(int &installed) {
    if (install_level_read()) ++installed;
    if (install_group_store()) ++installed;
}

}  // namespace u2mp
