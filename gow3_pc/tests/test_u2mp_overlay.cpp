// u2mp overlay rules test (no game needed): ninja -C out/gpu u2mp-overlay-test
// The scoreboard / result entry fade, priority and slide rules, the match-state entries, the automatic flag edges
// and the hint fade (spec parity-m-features.md M1 / M2), and the ticker placement / finished / visible rules on a
// stand-in ticker object (spec parity-ticker-respawn.md 1.2 - 1.6).
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../src/u2mp/overlay_rules.h"

using namespace u2mp::overlay;

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++failures;                                                  \
        }                                                                \
    } while (0)

static bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

static void test_fade() {
    // Speed 1.0: a full fade in takes 1 s of frames; speed 2.0 half that; out the same way; clamped.
    float f = 0.0f;
    for (int i = 0; i < 10; ++i) f = fade_step(f, true, 1.0f, 0.05f);
    CHECK(near(f, 0.5f));
    for (int i = 0; i < 20; ++i) f = fade_step(f, true, 1.0f, 0.05f);
    CHECK(f == 1.0f);
    for (int i = 0; i < 5; ++i) f = fade_step(f, false, 2.0f, 0.05f);
    CHECK(near(f, 0.5f));
    for (int i = 0; i < 10; ++i) f = fade_step(f, false, 2.0f, 0.05f);
    CHECK(f == 0.0f);
    CHECK(fade_step(0.0f, true, 1.0f, 0.0f) == 0.0f);   // first frame (dt 0) moves nothing
}

static void test_priority() {
    float f[kEntries] = {0.0f, 0.0f, 0.0f, 0.0f};
    CHECK(top_entry(f) == -1);
    f[0] = 1.0f;
    CHECK(top_entry(f) == 0);
    f[1] = 0.01f;
    CHECK(top_entry(f) == 1);
    f[3] = 0.2f;
    CHECK(top_entry(f) == 3);
    f[3] = 0.0f;
    f[2] = 0.5f;
    CHECK(top_entry(f) == 2);
}

static void test_slide() {
    // Fading in: starts 20 above (y -20), ends in place; fading out: in place, then on downward to +20.
    CHECK(near(slide_y(0.0f, true), -20.0f));
    CHECK(near(slide_y(1.0f, true), 0.0f));
    CHECK(near(slide_y(0.5f, true), -20.0f * 0.25f));   // ease 0.75
    CHECK(near(slide_y(1.0f, false), 0.0f));
    CHECK(near(slide_y(0.0f, false), 20.0f));
    CHECK(near(slide_y(0.5f, false), 20.0f * 0.75f));   // ease 0.25
    CHECK(near(slide_ease(0.5f, true), 0.75f));
    CHECK(near(slide_ease(0.5f, false), 0.25f));
}

static void test_states() {
    CHECK(state_entry(2, -1, false) == -1);
    CHECK(state_entry(3, -1, false) == 2);
    CHECK(state_entry(4, -1, false) == 3);
    CHECK(state_entry(5, -1, false) == 0);
    CHECK(state_entry(5, 3, false) == 3);
    CHECK(state_entry(5, 1, false) == 0);
    CHECK(state_entry(3, -1, true) == 2);
    CHECK(state_entry(4, -1, true) == 3);
    CHECK(state_entry(5, -1, true) == -1);
    CHECK(state_entry(0, -1, false) == -1);
}

static void test_auto_edge() {
    bool last = false;
    CHECK(auto_edge(last, false) == Edge::None);
    CHECK(auto_edge(last, true) == Edge::Opened && last);
    CHECK(auto_edge(last, true) == Edge::None);
    CHECK(auto_edge(last, false) == Edge::Closed && !last);
    CHECK(auto_edge(last, false) == Edge::None);
}

static void test_hint() {
    // In at 1/s, out at 2/s, clamped.
    float h = 0.0f;
    for (int i = 0; i < 10; ++i) h = hint_step(h, true, 0.05f);
    CHECK(near(h, 0.5f));
    h = hint_step(h, false, 0.1f);
    CHECK(near(h, 0.3f));
    h = hint_step(h, false, 0.5f);
    CHECK(h == 0.0f);
    for (int i = 0; i < 30; ++i) h = hint_step(h, true, 0.05f);
    CHECK(h == 1.0f);
}

// ---- ticker ----

struct Ticker {
    unsigned char b[kTickerSize] = {};
    void put(uint32_t off, float v) { std::memcpy(b + off, &v, sizeof v); }
    float get(uint32_t off) const {
        float v;
        std::memcpy(&v, b + off, sizeof v);
        return v;
    }
    uint32_t entry(int k) const { return kTickerEntry + uint32_t(k) * kTickerStride; }
    void add(int k, float start, float width, const char *text) {
        put(entry(k), start);
        put(entry(k) + 4, width);
        std::strcpy(reinterpret_cast<char *>(b + entry(k) + kTickerTextOff), text);
    }
    void free_entry(int k) { b[entry(k) + kTickerTextOff] = 0; }
};

static void test_ticker() {
    Ticker t;
    t.put(kTickerWidth, 900.0f);
    // Empty: the next text enters 10 right of the strip's right edge, into entry 0; finished.
    CHECK(near(ticker_next_start(t.b), 910.0f));
    CHECK(ticker_free_entry(t.b) == 0);
    CHECK(ticker_finished(t.b));

    // One text right after its add: not finished; a second one queues 150 behind it.
    t.add(0, 910.0f, 400.0f, "first");
    CHECK(!ticker_finished(t.b));
    CHECK(near(ticker_next_start(t.b), 910.0f + 400.0f + 150.0f));
    CHECK(ticker_free_entry(t.b) == 1);

    // Scrolled until start + 150 + width == strip width: finished (not greater); one unit earlier: not.
    t.put(t.entry(0), 900.0f - 150.0f - 400.0f + 1.0f);
    CHECK(!ticker_finished(t.b));
    t.put(t.entry(0), 900.0f - 150.0f - 400.0f);
    CHECK(ticker_finished(t.b));
    // Once fully in, a new text enters at the edge again, not behind the old one.
    CHECK(near(ticker_next_start(t.b), 910.0f));

    // A free entry in the middle is taken first; free entries count neither for placement nor for finished.
    t.add(1, 2000.0f, 50.0f, "second");
    t.free_entry(1);
    CHECK(ticker_free_entry(t.b) == 1);
    CHECK(ticker_finished(t.b));
    for (int k = 0; k < kTickerSlots; ++k) t.add(k, 0.0f, 10.0f, "x");
    CHECK(ticker_free_entry(t.b) == -1);

    // Visible: the last draw time within 0.1 s of the frame clock / 300.
    Ticker v;
    v.put(kTickerDrawn, 100.0f);
    CHECK(ticker_visible(v.b, 30000));        // 100.0 s
    CHECK(ticker_visible(v.b, 30000 + 10));   // 1/30 s later: the strip draws after the test in a frame
    CHECK(ticker_visible(v.b, 30000 - 29));
    CHECK(!ticker_visible(v.b, 30000 + 31));  // past 0.1 s: no longer
    CHECK(!ticker_visible(v.b, 60000));
    Ticker never;   // the strip never drew (+0x10 zero) and the clock is past 0.1 s
    CHECK(!ticker_visible(never.b, 300));
}

int main() {
    test_fade();
    test_priority();
    test_slide();
    test_states();
    test_auto_edge();
    test_hint();
    test_ticker();
    std::printf("%s\n", failures ? "u2mp overlay test FAILED" : "u2mp overlay test passed");
    return failures ? 1 : 0;
}
