// u2mp network core tests (gow3_pc/src/u2mp/net.cpp). Build: ninja -C gow3_pc/out/gpu u2mp-net-test
// 1. Two connections over a lossy link: every reliable message arrives once, in order.
// 2. A host and a client over real UDP on 127.0.0.1: join, messages both ways, leave.
#include "../src/u2mp/net.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace u2mp::net;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

static void lossy_link() {
    Connection a, b;
    std::vector<int> got_b, got_a;
    int unreliable_b = 0;
    const int total = 300;
    int sent = 0;
    std::vector<uint8_t> packet;
    for (int tick = 0; tick < 4000 && (got_b.size() < size_t(total) || got_a.size() < size_t(total)); ++tick) {
        if (sent < total) {
            const uint8_t m[4] = {uint8_t(sent), uint8_t(sent >> 8), 0xab, 0xcd};
            a.queue(true, m, sizeof m);
            b.queue(true, m, sizeof m);
            const uint8_t u[2] = {0x55, 0x66};
            a.queue(false, u, sizeof u);
            ++sent;
        }
        if (a.build(packet, true) && tick % 3 != 0)   // a -> b, every third packet lost
            b.receive(packet.data(), packet.size(), [&](const uint8_t *d, size_t n) {
                if (n == 4) got_b.push_back(d[0] | d[1] << 8);
                else ++unreliable_b;
            });
        if (b.build(packet, true) && tick % 5 != 1)   // b -> a, every fifth lost
            a.receive(packet.data(), packet.size(), [&](const uint8_t *d, size_t n) {
                if (n == 4) got_a.push_back(d[0] | d[1] << 8);
            });
    }
    CHECK(got_b.size() == size_t(total));
    CHECK(got_a.size() == size_t(total));
    for (size_t i = 0; i < got_b.size(); ++i) CHECK(got_b[i] == int(i));
    for (size_t i = 0; i < got_a.size(); ++i) CHECK(got_a[i] == int(i));
    CHECK(unreliable_b > 0 && unreliable_b < total);   // some unreliable ones lost, none resent
    CHECK(a.pending_reliable() == 0);
    std::printf("lossy link: %zu/%zu reliable in order, %d unreliable\n", got_b.size(), got_a.size(), unreliable_b);
}

static void loopback() {
    Transport host, client;
    CHECK(host.host(0));
    Address to;
    CHECK(Address::parse("127.0.0.1:" + std::to_string(host.bound_port()), 0, to));
    CHECK(client.join(to));
    int joined = 0, left = 0, host_got = 0, client_got = 0;
    host.on_peer = [&](uint8_t slot, bool in) { (in ? joined : left) += slot == 1; };
    host.on_message = [&](uint8_t slot, const uint8_t *d, size_t n) { host_got += slot == 1 && n == 3 && d[0] == 'c'; };
    client.on_message = [&](uint8_t slot, const uint8_t *d, size_t n) { client_got += slot == 0 && n == 3 && d[0] == 'h'; };
    double now = 0;
    for (int i = 0; i < 200 && !client.connected(); ++i) {
        client.pump(now);
        host.pump(now);
        now += 0.016;
    }
    CHECK(client.connected());
    CHECK(client.local_slot() == 1);
    CHECK(joined == 1);
    for (int i = 0; i < 20; ++i) {
        client.send(0, true, reinterpret_cast<const uint8_t *>("cli"), 3);
        host.broadcast(true, reinterpret_cast<const uint8_t *>("hst"), 3);
    }
    for (int i = 0; i < 200 && (host_got < 20 || client_got < 20); ++i) {
        client.pump(now);
        host.pump(now);
        now += 0.016;
    }
    CHECK(host_got == 20);
    CHECK(client_got == 20);
    client.close();
    for (int i = 0; i < 20 && !left; ++i) {
        host.pump(now);
        now += 0.016;
    }
    CHECK(left == 1);
    std::printf("loopback: joined %d, host got %d, client got %d, left %d\n", joined, host_got, client_got, left);
}

// 3. A client times out on the host (no Bye reaches it) and joins again from the same address before the
//    host's own timeout: the host must drop the old connection and announce a new one, so the game layer
//    starts fresh (online play: the online link names a peer by its NpId, so the address stays the same).
static void rejoin_same_address() {
    Transport host;
    CHECK(host.host(0));
    Address to;
    CHECK(Address::parse("127.0.0.1:" + std::to_string(host.bound_port()), 0, to));
    int joined = 0, left = 0, client_got = 0;
    host.on_peer = [&](uint8_t slot, bool in) { (in ? joined : left) += slot == 1; };
    double now = 0;
    uint16_t port = 0;
    {
        Transport first;
        CHECK(first.join(to));
        port = first.bound_port();
        for (int i = 0; i < 200 && !first.connected(); ++i) {
            first.pump(now);
            host.pump(now);
            now += 0.016;
        }
        CHECK(first.connected());
        for (int i = 0; i < 10; ++i) first.send(0, true, reinterpret_cast<const uint8_t *>("cli"), 3);
        for (int i = 0; i < 30; ++i) {
            first.pump(now);
            host.pump(now);
            now += 0.016;
        }
        // The host goes quiet for the client (only the client pumps): it times out without a Bye.
        for (int i = 0; i < 12 && first.connected(); ++i) {
            now += 1.0;
            first.pump(now);
        }
        CHECK(!first.connected());
        first.close();   // not connected any more: no Bye goes out
    }
    CHECK(joined == 1 && left == 0);
    Transport second;
    CHECK(second.join(to, port));   // the same address as before
    second.on_message = [&](uint8_t slot, const uint8_t *d, size_t n) { client_got += slot == 0 && n == 3 && d[0] == 'h'; };
    for (int i = 0; i < 200 && !second.connected(); ++i) {
        second.pump(now);
        host.pump(now);
        now += 0.016;
    }
    CHECK(second.connected());
    CHECK(left == 1);
    CHECK(joined == 2);
    for (int i = 0; i < 5; ++i) host.send(1, true, reinterpret_cast<const uint8_t *>("hst"), 3);
    for (int i = 0; i < 200 && client_got < 5; ++i) {
        second.pump(now);
        host.pump(now);
        now += 0.016;
    }
    CHECK(client_got == 5);
    std::printf("rejoin: joined %d, left %d, client got %d after the rejoin\n", joined, left, client_got);
}

// 4. The host's admit hook turns a Hello away (a griefer-kick block): no slot, no Welcome, no peer event,
//    exactly as when the party is full. Once the hook lets it in, the same client joins.
static void refused_join() {
    Transport host, client;
    CHECK(host.host(0));
    Address to;
    CHECK(Address::parse("127.0.0.1:" + std::to_string(host.bound_port()), 0, to));
    CHECK(client.join(to));
    int joined = 0, asked = 0;
    bool allow = false;
    host.on_peer = [&](uint8_t, bool in) { joined += in; };
    host.admit = [&](const Address &) { ++asked; return allow; };
    double now = 0;
    for (int i = 0; i < 100; ++i) {
        client.pump(now);
        host.pump(now);
        now += 0.016;
    }
    CHECK(!client.connected() && joined == 0 && asked >= 2);
    allow = true;
    for (int i = 0; i < 100 && !client.connected(); ++i) {
        client.pump(now);
        host.pump(now);
        now += 0.016;
    }
    CHECK(client.connected() && joined == 1);
    std::printf("refused join: %d Hellos refused, then joined %d\n", asked - 1, joined);
}

int main() {
    lossy_link();
    loopback();
    rejoin_same_address();
    refused_join();
    std::printf(failures ? "u2mp net: %d FAILED\n" : "u2mp net: all passed\n", failures);
    return failures ? 1 : 0;
}
