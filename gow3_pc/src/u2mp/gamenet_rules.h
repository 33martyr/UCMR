// u2mp game transport: the send window, send cadence and snapshot expiry rules of the PS3 packet and
// message layers (u2mp/docs/spec/parity-i-connection.md I1..I3), kept free of the game so that
// tests/test_u2mp_gamenet.cpp can check them. gamenet.cpp applies them to the PS4 message layers.
// List layouts: u2mp/docs/TRANSPORT-INTERFACE.md 1.1 (event), 1.3 (queue node), 1.4 (M, packet record).
#pragma once
#include <cstdint>

namespace u2mp::gamenet_rules {

template <typename T> T &at(uint64_t object, uint64_t offset) { return *reinterpret_cast<T *>(object + offset); }

// Message layer M: in-flight packet records, unreliable queue (head / tail).
constexpr uint64_t kInflightHead = 0x100, kUnreliableHead = 0x120, kUnreliableTail = 0x128;
// Packet record: next record, its message list. Queue node: event, next node.
constexpr uint64_t kRecordNext = 0x08, kRecordMessages = 0x10;
constexpr uint64_t kNodeEvent = 0x00, kNodeNext = 0x20;
// Event: vtable, type, delivery mode (0 reliable, 1 re-queued on loss, 2 fire-and-forget), reference
// count, first payload word.
constexpr uint64_t kEventType = 0x08, kEventMode = 0x09, kEventRefs = 0x0a, kEventStamp = 0x0c;

// ---- I1: send window ------------------------------------------------------------------------
// More than 30 unacknowledged packets: the packet goes out header-only (sequence not advanced), so at
// most 31 are ever in flight.
constexpr int kMaxUnacked = 30;
constexpr bool window_full(uint32_t next_seq, uint32_t acked_seq) {
    return int32_t(next_seq - acked_seq) > kMaxUnacked;
}

// ---- I2: send cadence -----------------------------------------------------------------------
constexpr double kTick = 0.05, kKeepalive = 2.0;
struct Cadence {
    double next_tick = 0, keepalive = 0;
};
// A fixed 50 ms tick: true when it has come; the next one is due 50 ms later whether or not a packet
// goes out. Between ticks nothing is sent.
inline bool tick(Cadence &c, double now) {
    if (now < c.next_tick) return false;
    c.next_tick = now + kTick;
    return true;
}
// On a tick: a packet goes out when something is pending (see pending()) or the 2 s keepalive has
// passed; each packet moves the keepalive 2 s on.
inline bool send(Cadence &c, double now, bool pending) {
    if (!pending && now < c.keepalive) return false;
    c.keepalive = now + kKeepalive;
    return true;
}
// An in-flight packet still holds a mode 0 or 1 message (packets holding only mode-2 snapshots do not
// keep the connection sending).
inline bool inflight_needs_ack(uint64_t M) {
    for (uint64_t r = at<uint64_t>(M, kInflightHead); r; r = at<uint64_t>(r, kRecordNext))
        for (uint64_t n = at<uint64_t>(r, kRecordMessages); n; n = at<uint64_t>(n, kNodeNext))
            if (at<uint8_t>(at<uint64_t>(n, kNodeEvent), kEventMode) < 2) return true;
    return false;
}

// ---- I3: snapshot expiry --------------------------------------------------------------------
// Only the player state snapshot (type 12, CharacterMove, mode 2) expires: when the net clock is more
// than 1.0 s past the net clock time it was filled with (its first payload word, f32 seconds).
constexpr uint8_t kCharacterMove = 12;
constexpr double kSnapshotLife = 1.0;
inline bool expired(uint64_t ev, double net_now) {
    return at<uint8_t>(ev, kEventType) == kCharacterMove && at<uint8_t>(ev, kEventMode) == 2 &&
           net_now - double(at<float>(ev, kEventStamp)) > kSnapshotLife;
}
// Unlinks every expired message from M's unreliable queue; the others keep their order and the tail
// follows the last one left. Returns the unlinked nodes, in queue order, linked through their next.
inline uint64_t unlink_expired(uint64_t M, double net_now) {
    uint64_t out = 0, out_tail = 0, prev = 0;
    for (uint64_t n = at<uint64_t>(M, kUnreliableHead); n;) {
        const uint64_t next = at<uint64_t>(n, kNodeNext);
        if (expired(at<uint64_t>(n, kNodeEvent), net_now)) {
            (prev ? at<uint64_t>(prev, kNodeNext) : at<uint64_t>(M, kUnreliableHead)) = next;
            if (!next) at<uint64_t>(M, kUnreliableTail) = prev;
            at<uint64_t>(n, kNodeNext) = 0;
            (out_tail ? at<uint64_t>(out_tail, kNodeNext) : out) = n;
            out_tail = n;
        } else {
            prev = n;
        }
        n = next;
    }
    return out;
}

// ---- run-time layout checks -----------------------------------------------------------------
// A message list (linked through node next) whose every node holds an event that looks like one: a
// vtable inside [lo, hi), type 0..79, mode 0..2, at least one reference. Stops after 512 nodes.
inline bool plausible_messages(uint64_t list, uint64_t lo, uint64_t hi) {
    int count = 0;
    for (uint64_t n = list; n; n = at<uint64_t>(n, kNodeNext)) {
        if (++count > 512) return false;
        const uint64_t ev = at<uint64_t>(n, kNodeEvent);
        if (!ev) return false;
        const uint64_t vt = at<uint64_t>(ev, 0);
        if (vt < lo || vt >= hi || at<uint8_t>(ev, kEventType) >= 80 || at<uint8_t>(ev, kEventMode) > 2 ||
            at<uint8_t>(ev, kEventRefs) == 0)
            return false;
    }
    return true;
}

}  // namespace u2mp::gamenet_rules
