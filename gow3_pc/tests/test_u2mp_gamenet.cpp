// u2mp game transport rules (gow3_pc/src/u2mp/gamenet_rules.h, spec parity-i-connection.md I1..I3).
// Build: ninja -C gow3_pc/out/gpu u2mp-gamenet-test. Message layers, packet records, queue nodes and
// events are host memory laid out like the PS4 ones (TRANSPORT-INTERFACE 1.1, 1.3, 1.4).
#include "../src/u2mp/gamenet_rules.h"

#include <cstdio>
#include <cstring>
#include <deque>

namespace r = u2mp::gamenet_rules;

static int failures = 0;

static void check(bool ok, const char *what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

template <size_t N> struct alignas(16) Block {
    unsigned char bytes[N] = {};
    uint64_t address() { return uint64_t(uintptr_t(bytes)); }
};
using Layer = Block<0x220>;
using Node = Block<0x28>;
using Record = Block<0x18>;
using Event = Block<0x40>;

static std::deque<Event> events;
static std::deque<Node> nodes;
static uint64_t fake_vtable[8];

static uint64_t make_event(uint8_t type, uint8_t mode, float stamp) {
    Event &e = events.emplace_back();
    r::at<uint64_t>(e.address(), 0) = uint64_t(uintptr_t(fake_vtable));
    r::at<uint8_t>(e.address(), r::kEventType) = type;
    r::at<uint8_t>(e.address(), r::kEventMode) = mode;
    r::at<uint8_t>(e.address(), r::kEventRefs) = 1;
    r::at<float>(e.address(), r::kEventStamp) = stamp;
    return e.address();
}
static uint64_t make_node(uint64_t ev) {
    Node &n = nodes.emplace_back();
    r::at<uint64_t>(n.address(), r::kNodeEvent) = ev;
    return n.address();
}
// Appends a node to the unreliable queue of M (head / tail).
static uint64_t enqueue(Layer &M, uint64_t ev) {
    const uint64_t n = make_node(ev);
    if (uint64_t tail = r::at<uint64_t>(M.address(), r::kUnreliableTail)) r::at<uint64_t>(tail, r::kNodeNext) = n;
    else r::at<uint64_t>(M.address(), r::kUnreliableHead) = n;
    r::at<uint64_t>(M.address(), r::kUnreliableTail) = n;
    return n;
}
static int length(uint64_t list) {
    int count = 0;
    for (uint64_t n = list; n; n = r::at<uint64_t>(n, r::kNodeNext)) ++count;
    return count;
}
static uint64_t nth(uint64_t list, int i) {
    while (i-- > 0 && list) list = r::at<uint64_t>(list, r::kNodeNext);
    return list;
}

int main() {
    // I1: header-only only when more than 30 are unacknowledged (31 may be in flight).
    check(!r::window_full(30, 0), "30 in flight: the 31st packet still carries messages");
    check(r::window_full(31, 0), "31 in flight: header-only");
    check(!r::window_full(0, 0), "nothing in flight");
    check(r::window_full(5, 0xffffffe0u), "window across the 32-bit wrap (37 in flight)");
    check(!r::window_full(10, 0xfffffff0u), "26 in flight across the wrap");

    // I2: fixed 50 ms tick that advances without a send; keepalive 2 s after the last packet.
    {
        r::Cadence c;
        check(r::tick(c, 0.0) && r::send(c, 0.0, false), "first tick sends (keepalive due)");
        check(!r::tick(c, 0.03), "no tick before 50 ms");
        check(r::tick(c, 0.05) && !r::send(c, 0.05, false), "idle tick: nothing sent");
        check(!r::tick(c, 0.06), "a message queued at 0.06 waits for the next tick");
        check(r::tick(c, 0.10) && r::send(c, 0.10, true), "it leaves on the tick at 0.10");
        bool sent = false;
        for (int i = 3; i < 40; ++i)   // ticks at 0.15 .. 1.95
            if (r::tick(c, i * 0.05) && r::send(c, i * 0.05, false)) sent = true;
        check(!sent, "idle: no packet before the 2 s keepalive");
        check(r::tick(c, 2.15) && r::send(c, 2.15, false), "keepalive 2 s after the last packet");
        check(c.keepalive > 4.14 && c.keepalive < 4.16, "keepalive moved 2 s on");
        r::Cadence d;
        d.next_tick = 1.0;
        check(r::tick(d, 1.017) && d.next_tick > 1.066 && d.next_tick < 1.068, "next tick = now + 50 ms (frame late)");
    }

    // I2: in-flight packets keep it sending only while they hold a mode 0 or 1 message.
    {
        Layer M;
        Record a, b;
        r::at<uint64_t>(M.address(), r::kInflightHead) = a.address();
        r::at<uint64_t>(a.address(), r::kRecordNext) = b.address();
        const uint64_t s1 = make_node(make_event(12, 2, 1.0f)), s2 = make_node(make_event(12, 2, 1.0f));
        r::at<uint64_t>(s1, r::kNodeNext) = s2;
        r::at<uint64_t>(a.address(), r::kRecordMessages) = s1;
        r::at<uint64_t>(b.address(), r::kRecordMessages) = make_node(make_event(54, 2, 0));
        check(!r::inflight_needs_ack(M.address()), "snapshots only in flight: not pending");
        r::at<uint64_t>(s2, r::kNodeNext) = make_node(make_event(6, 0, 0));
        check(r::inflight_needs_ack(M.address()), "a reliable message in flight: pending");
        r::at<uint64_t>(s2, r::kNodeNext) = 0;
        r::at<uint64_t>(b.address(), r::kRecordMessages) = make_node(make_event(30, 1, 0));
        check(r::inflight_needs_ack(M.address()), "a mode 1 message in the second record: pending");
        Layer empty;
        check(!r::inflight_needs_ack(empty.address()), "nothing in flight: not pending");
    }

    // I3: only type 12 expires, more than 1.0 s after its stamp; order kept, tail fixed up.
    {
        Layer M;
        const double now = 100.0;
        const uint64_t old1 = enqueue(M, make_event(12, 2, 98.5f));
        const uint64_t other = enqueue(M, make_event(54, 2, 10.0f));     // other types never expire
        const uint64_t edge = enqueue(M, make_event(12, 2, 99.0f));      // exactly 1.0 s: kept
        const uint64_t mode1 = enqueue(M, make_event(12, 1, 50.0f));     // not a mode-2 snapshot: kept
        const uint64_t fresh = enqueue(M, make_event(12, 2, 99.9f));
        const uint64_t old2 = enqueue(M, make_event(12, 2, 90.0f));
        const uint64_t gone = r::unlink_expired(M.address(), now);
        const uint64_t head = r::at<uint64_t>(M.address(), r::kUnreliableHead);
        check(length(gone) == 2 && gone == old1 && nth(gone, 1) == old2, "two old snapshots unlinked, in order");
        check(length(head) == 4 && head == other && nth(head, 1) == edge && nth(head, 2) == mode1 && nth(head, 3) == fresh,
              "the others keep their order");
        check(r::at<uint64_t>(M.address(), r::kUnreliableTail) == fresh, "tail follows the last one left");
        check(r::unlink_expired(M.address(), now) == 0, "a second pass finds nothing");

        Layer all;
        enqueue(all, make_event(12, 2, 1.0f));
        enqueue(all, make_event(12, 2, 2.0f));
        check(length(r::unlink_expired(all.address(), 10.0)) == 2, "every message expired");
        check(r::at<uint64_t>(all.address(), r::kUnreliableHead) == 0 && r::at<uint64_t>(all.address(), r::kUnreliableTail) == 0,
              "queue empty: head and tail cleared");
        Layer nan;
        float q;
        const uint32_t bits = 0x7fc00000u;
        std::memcpy(&q, &bits, 4);
        enqueue(nan, make_event(12, 2, q));
        check(r::unlink_expired(nan.address(), 10.0) == 0, "a NaN stamp never expires");
    }

    // Run-time layout check.
    {
        const uint64_t lo = uint64_t(uintptr_t(fake_vtable)), hi = lo + sizeof(fake_vtable);
        const uint64_t a = make_node(make_event(12, 2, 0)), b = make_node(make_event(6, 0, 0));
        r::at<uint64_t>(a, r::kNodeNext) = b;
        check(r::plausible_messages(a, lo, hi), "a real message list is plausible");
        check(!r::plausible_messages(a, lo + 8, hi), "a vtable outside the image is not");
        r::at<uint8_t>(r::at<uint64_t>(b, r::kNodeEvent), r::kEventType) = 80;
        check(!r::plausible_messages(a, lo, hi), "type 80 is not");
    }

    if (failures) return 1;
    std::printf("u2mp gamenet rules: all tests passed\n");
    return 0;
}
