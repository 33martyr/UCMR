// u2mp clan tag check rules test (no game needed): ninja -C out/gpu u2mp-clantag-test
// The scenarios A-C1 .. A-C7 of u2mp/docs/spec/parity-a-leaderboards.md 3 on the pure rules (clantag_rules.h); the
// NET_SM gate (A-C8) is clantag.cpp's own first line.
#include <cstdio>
#include <cstring>

#include "../src/u2mp/clantag_rules.h"

using namespace u2mp::clantag;

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++failures;                                                  \
        }                                                                \
    } while (0)

struct Tag {
    char b[kTagBytes] = {};
    explicit Tag(const char *s) { std::strncpy(b, s, kTagBytes); }
};

static bool is(const Check &c, const char *s) { return std::memcmp(c.checked, Tag(s).b, kTagBytes) == 0; }

static void test_accepted() {
    // A-C1: a new tag starts a check; accepted: kept as typed.
    Check c;
    CHECK(notice(c, Tag("AB").b) == Change::Check && is(c, "AB"));
    CHECK(on_start(c, Start::Started) == Step::Started && c.running);
    CHECK(on_verdict(c, Verdict::Running) == Step::Waiting && c.running);
    CHECK(on_verdict(c, Verdict::Accepted) == Step::Kept && !c.running && is(c, "AB"));
    CHECK(notice(c, Tag("AB").b) == Change::Same);   // checked once
}

static void test_rejected() {
    // A-C2 refused / A-C3 the check failed: both "rejected", the tag becomes "*" and zeros, not checked again.
    Check c;
    CHECK(notice(c, Tag("BAD1").b) == Change::Check);
    on_start(c, Start::Started);
    CHECK(on_verdict(c, Verdict::Rejected) == Step::Refused && !c.running && is(c, "*"));
    for (size_t i = 1; i < kTagBytes; ++i) CHECK(c.checked[i] == 0);
    CHECK(notice(c, Tag("*").b) == Change::Same);   // "*" equals the last checked tag
}

static void test_no_censor() {
    // A-C4: no censor service: accepted at once, never "*".
    Check c;
    CHECK(notice(c, Tag("XYZ").b) == Change::Check);
    CHECK(on_start(c, Start::Unavailable) == Step::Unavailable && !c.running && is(c, "XYZ"));
    CHECK(notice(c, Tag("XYZ").b) == Change::Same);
}

static void test_cleared() {
    // A-C5: clearing the tag publishes at once, no check; an empty tag at the start is no change.
    Check c;
    CHECK(notice(c, Tag("").b) == Change::Same);
    CHECK(notice(c, Tag("AB").b) == Change::Check);
    on_start(c, Start::Unavailable);
    CHECK(notice(c, Tag("").b) == Change::Cleared && !c.running && is(c, ""));
    CHECK(notice(c, Tag("").b) == Change::Same);
}

static void test_start_failed() {
    // A-C6: the check does not start: nothing more, and no retry next frame while the tag stays.
    Check c;
    CHECK(notice(c, Tag("AB").b) == Change::Check);
    CHECK(on_start(c, Start::Failed) == Step::StartFailed && !c.running);
    CHECK(notice(c, Tag("AB").b) == Change::Same);
    CHECK(notice(c, Tag("ABC").b) == Change::Check);   // a new tag is checked again
}

static void test_profile_tag() {
    // A-C7: a tag loaded from the profile is checked on the first online frame only; only 4 bytes are compared.
    Check c;
    CHECK(notice(c, Tag("XYZ").b) == Change::Check);
    on_start(c, Start::Unavailable);
    CHECK(notice(c, Tag("XYZ").b) == Change::Same);
    Tag longer("XYZ");
    longer.b[5] = 'Q';   // a byte past the compared four
    CHECK(notice(c, longer.b) == Change::Same);
}

int main() {
    test_accepted();
    test_rejected();
    test_no_censor();
    test_cleared();
    test_start_failed();
    test_profile_tag();
    std::printf("%s\n", failures ? "u2mp clan tag test FAILED" : "u2mp clan tag test passed");
    return failures ? 1 : 0;
}
