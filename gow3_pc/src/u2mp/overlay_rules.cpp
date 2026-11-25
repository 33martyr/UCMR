// u2mp: the pure overlay and ticker rules (overlay_rules.h).
//
// Reimplemented from the behaviour descriptions u2mp/docs/spec/parity-m-features.md (M1, M2) and
// u2mp/docs/spec/parity-ticker-respawn.md (1.2 - 1.6).
#include "overlay_rules.h"

#include <cmath>
#include <cstring>

namespace u2mp::overlay {
namespace {

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float f32_at(const unsigned char *p, uint32_t off) {
    float v;
    std::memcpy(&v, p + off, sizeof v);
    return v;
}
uint32_t entry_at(int k) { return kTickerEntry + uint32_t(k) * kTickerStride; }
bool in_use(const unsigned char *ticker, int k) { return ticker[entry_at(k) + kTickerTextOff] != 0; }
float start_of(const unsigned char *ticker, int k) { return f32_at(ticker, entry_at(k)); }
float width_of(const unsigned char *ticker, int k) { return f32_at(ticker, entry_at(k) + 4); }

}  // namespace

float fade_step(float fade, bool shown, float speed, float dt) {
    return clamp01(shown ? fade + speed * dt : fade - speed * dt);
}

int top_entry(const float (&fade)[kEntries]) {
    for (int k = kEntries - 1; k >= 0; --k)
        if (fade[k] > 0.0f) return k;
    return -1;
}

float slide_ease(float fade, bool shown) {
    if (shown) return 1.0f - (1.0f - fade) * (1.0f - fade);
    return fade * fade;
}

float slide_y(float fade, bool shown) {
    const float rest = kSlide * (1.0f - slide_ease(fade, shown));
    return shown ? -rest : rest;
}

int state_entry(int32_t ngm_state, int32_t round_substate, bool cinema) {
    if (ngm_state == 3) return 2;
    if (ngm_state == 4) return 3;
    if (ngm_state == 5 && !cinema) return round_substate == 3 ? 3 : 0;
    return -1;
}

Edge auto_edge(bool &last, bool now) {
    if (now == last) return Edge::None;
    last = now;
    return now ? Edge::Opened : Edge::Closed;
}

float hint_step(float h, bool on, float dt) { return clamp01(on ? h + kHintIn * dt : h - kHintOut * dt); }

float ticker_next_start(const unsigned char *ticker) {
    float start = f32_at(ticker, kTickerWidth) + kTickerEdgeGap;
    for (int k = 0; k < kTickerSlots; ++k) {
        if (!in_use(ticker, k)) continue;
        const float behind = start_of(ticker, k) + width_of(ticker, k) + kTickerQueueGap;
        if (behind > start) start = behind;
    }
    return start;
}

int ticker_free_entry(const unsigned char *ticker) {
    for (int k = 0; k < kTickerSlots; ++k)
        if (!in_use(ticker, k)) return k;
    return -1;
}

bool ticker_finished(const unsigned char *ticker) {
    const float width = f32_at(ticker, kTickerWidth);
    for (int k = 0; k < kTickerSlots; ++k)
        if (in_use(ticker, k) && start_of(ticker, k) + kTickerQueueGap + width_of(ticker, k) > width) return false;
    return true;
}

bool ticker_visible(const unsigned char *ticker, int32_t frame_clock) {
    const float now = float(frame_clock) * (1.0f / 300.0f);
    return std::fabs(double(now) - double(f32_at(ticker, kTickerDrawn))) < kTickerVisibleSeconds;
}

}  // namespace u2mp::overlay
