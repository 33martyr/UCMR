// u2mp peer rule tests (gow3_pc/src/u2mp/peer_rules.h). Build: ninja -C gow3_pc/out/gpu u2mp-peer-rules-test
// The unreachable list (spec parity-i-connection.md I4: 32 slots, oldest overwritten, duplicates kept, cleared on a
// new online session), the "dead before active" rule and the four bar colours of the matchmaking rows (I5).
#include "../src/u2mp/peer_rules.h"

#include <cstdio>
#include <string>

using u2mp::peer_rules::UnreachableList;

static int failures = 0;

static void check(bool ok, const char *what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

int main() {
    UnreachableList list;
    check(list.size() == 0 && !list.contains("a"), "a new list is empty");
    check(!list.contains(""), "an empty id is never listed");
    list.add("a");
    list.add("b");
    check(list.contains("a") && list.contains("b") && !list.contains("c"), "listed ids are found, others not");
    list.add("a");
    check(list.size() == 3, "a duplicate takes its own slot");

    // 32 slots: entries 33 and 34 overwrite the two oldest ("a", "b"); the second "a" (entry 3) stays.
    list.clear();
    for (int i = 0; i < 32; ++i) list.add("p" + std::to_string(i));
    check(list.size() == 32 && list.contains("p0") && list.contains("p31"), "32 entries all kept");
    list.add("x");
    check(list.size() == 32 && !list.contains("p0") && list.contains("p1") && list.contains("x"),
          "the 33rd entry overwrites the oldest");
    list.add("y");
    check(!list.contains("p1") && list.contains("p2") && list.contains("y"), "the 34th overwrites the next oldest");
    for (int i = 0; i < 30; ++i) list.add("z");
    check(list.contains("x") && list.contains("y") && !list.contains("p31"), "the ring wraps in order");
    list.clear();
    check(list.size() == 0 && !list.contains("x") && !list.contains("z"), "clear empties the list");

    using u2mp::peer_rules::dead_means_unreachable;
    check(dead_means_unreachable(false, false), "dead before ever active: unreachable");
    check(!dead_means_unreachable(true, false), "dead after being active: not unreachable");
    check(!dead_means_unreachable(false, true), "ended by us: not unreachable");

    using u2mp::peer_rules::bars_colour;
    check(bars_colour(U2O_PEER_INACTIVE) == 0x800000FFu, "inactive colour");
    check(bars_colour(U2O_PEER_PENDING) == 0x80FF0000u, "pending colour");
    check(bars_colour(U2O_PEER_ACTIVE) == 0x8000FF00u, "active colour");
    check(bars_colour(U2O_PEER_DEAD) == 0x80808080u, "dead is grey");
    check(bars_colour(-1) == 0x80808080u, "no record is grey");

    if (failures) return 1;
    std::printf("u2mp peer rules: all tests passed\n");
    return 0;
}
