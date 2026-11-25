// u2mp peer rules: small, game-free rules about the links to other players (spec parity-i-connection.md I4, I5),
// kept here so tests/test_u2mp_peer_rules.cpp can check them without the game.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "u2_online_abi.h"

namespace u2mp::peer_rules {

// PS3 connection manager's unreachable list (spec 1.4.1): 32 online ids of players whose link died as "peer
// unreachable"; the 33rd entry overwrites the oldest, a player listed twice takes two slots. Emptied only when
// online multiplayer is entered again.
class UnreachableList {
public:
    static constexpr size_t kSlots = 32;
    void clear() { count_ = next_ = 0; }
    void add(const std::string &id) {
        ids_[next_] = id;
        next_ = (next_ + 1) % kSlots;
        if (count_ < kSlots) ++count_;
    }
    bool contains(const std::string &id) const {
        if (id.empty()) return false;
        for (size_t i = 0; i < count_; ++i)
            if (ids_[i] == id) return true;
        return false;
    }
    size_t size() const { return count_; }

private:
    std::string ids_[kSlots];
    size_t count_ = 0, next_ = 0;   // slots in use; the slot the next entry goes to
};

// The online program does not say why a link died (spec question 2). A link that died before it was ever active,
// and that we did not end ourselves, stands for PS3's "peer unreachable".
inline bool dead_means_unreachable(bool was_active, bool ended_by_us) { return !was_active && !ended_by_us; }

// Colour word of a player row's connection bars (PS3 0x4bfac8 from the signaling state 0x4e8f1c, spec I5), by the
// link state of u2_online_abi.h; -1 (no record), dead and anything else are grey. Written as PS3 writes it (the PS4
// byte order is parity-b-menus.md 2's open point).
inline uint32_t bars_colour(int32_t link_state) {
    switch (link_state) {
    case U2O_PEER_INACTIVE: return 0x800000FFu;
    case U2O_PEER_PENDING: return 0x80FF0000u;
    case U2O_PEER_ACTIVE: return 0x8000FF00u;
    default: return 0x80808080u;
    }
}

}  // namespace u2mp::peer_rules
