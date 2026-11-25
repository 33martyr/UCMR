// u2mp: the pure rules of the clan tag check (u2mp/docs/spec/parity-a-leaderboards.md 2.1). No game memory, no
// online program: clantag.cpp feeds the current tag and the censor's answers in, and writes, publishes and logs what
// a step says. Tested by tests/test_u2mp_clantag.cpp.
//
// Reimplemented from the behaviour description u2mp/docs/spec/parity-a-leaderboards.md (2.1).
#pragma once

#include <cstddef>
#include <cstring>

namespace u2mp::clantag {

constexpr size_t kTagBytes = 8;      // NetPlayerData +0x1a8: the tag, NUL-padded
constexpr size_t kTagCompared = 4;   // PS3 compared the first 4 bytes

// The check's state: the last tag checked (all zero at the start, kept for the process) and whether a check runs.
struct Check {
    char checked[kTagBytes] = {};
    bool running = false;
};

// A frame without a running check, the tag compared with the last one checked.
enum class Change {
    Same,      // nothing to do
    Cleared,   // the tag became empty: the career and the member data are published at once, nothing is checked
    Check,     // a new non-empty tag: start a censor check of it
};
// What starting a censor check gave.
enum class Start { Started, Failed, Unavailable };
// What a running check answers.
enum class Verdict { Running, Accepted, Rejected };   // Rejected: refused, or the check failed

// What the frame ends with.
enum class Step {
    Waiting,       // the check still runs
    Started,       // a check runs now
    StartFailed,   // could not start: the tag stays as typed and is not checked again until it changes
    Unavailable,   // no censor: accepted at once (never "failed", which would turn every tag into "*")
    Kept,          // accepted: the checked tag is written, then the career and the member data are published
    Refused,       // rejected: the checked tag is now "*", written, then the career and the member data are published
};

// The tag changed (first kTagCompared bytes) since the last check: it becomes the last checked tag (all 8 bytes).
inline Change notice(Check &c, const char *tag) {
    if (std::memcmp(tag, c.checked, kTagCompared) == 0) return Change::Same;
    std::memcpy(c.checked, tag, kTagBytes);
    return c.checked[0] ? Change::Check : Change::Cleared;
}

// After Change::Check: what the censor start gave.
inline Step on_start(Check &c, Start s) {
    switch (s) {
    case Start::Started: c.running = true; return Step::Started;
    case Start::Failed: return Step::StartFailed;
    case Start::Unavailable: return Step::Unavailable;
    }
    return Step::StartFailed;
}

// While a check runs: its answer. A rejected non-empty tag becomes "*" followed by zeros.
inline Step on_verdict(Check &c, Verdict v) {
    if (v == Verdict::Running) return Step::Waiting;
    c.running = false;
    if (v == Verdict::Accepted || !c.checked[0]) return Step::Kept;
    std::memset(c.checked, 0, kTagBytes);
    c.checked[0] = '*';
    return Step::Refused;
}

}  // namespace u2mp::clantag
