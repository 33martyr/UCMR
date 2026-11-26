// core_smoke.cpp - a first check of a built launcher core, for the CI: loads the library and calls functions that
// answer the same on every system (no game, no window, no network).
//   core_smoke <libuncharted_native.so | uncharted_native.dll> <the launcher's folder>
// Prints one line per check; the exit code is the number of checks that failed.
#include "native.h"
#include "json.h"
#include "port.h"

#include <cstdio>
#include <string>

namespace {

int failed = 0;

void check(bool ok, const char *what, const std::string &detail = "") {
    std::printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
    if (!ok) ++failed;
}

// a text call of the core's convention (native.h); "" when it failed
template <typename Call> std::string text(Call call, int &status) {
    std::string out(1 << 16, '\0');
    status = call(&out[0], (int)out.size());
    if (status < 0) {
        out.assign((size_t)-status, '\0');
        status = call(&out[0], (int)out.size());
    }
    out.resize(std::char_traits<char>::length(out.c_str()));
    return out;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: core_smoke <library> <launcher folder>\n");
        return 2;
    }
    void *lib = port::load_library(port::path_utf8(argv[1]));
    if (!lib) {
        std::fprintf(stderr, "core_smoke: %s could not be loaded\n", argv[1]);
        return 2;
    }
    auto version = (int (*)())port::symbol(lib, "un_version");
    auto tables = (int (*)(char *, int))port::symbol(lib, "un_system_tables");
    auto settings = (int (*)(const char *, char *, int))port::symbol(lib, "un_settings_table");
    auto system = (int (*)(const char *, const un_path_char *, char *, int))port::symbol(lib, "un_system");
    auto steam = (int (*)())port::symbol(lib, "un_steam_running");
    auto core_open = port::symbol(lib, "un_core_open");
    check(version && tables && settings && system && steam && core_open, "exports");
    if (failed) return failed;

    check(version() == UN_VERSION, "un_version", std::to_string(version()));
    int status = 0;
    json::Value t;
    const std::string table = text([&](char *o, int c) { return tables(o, c); }, status);
    check(status == 0 && json::parse(table, t) && t.is_object() && t.get("GAMES"), "un_system_tables");
    const std::string defaults = text([&](char *o, int c) { return settings("ini_defaults", o, c); }, status);
    check(status == 0 && defaults.find('\t') != std::string::npos, "un_settings_table ini_defaults");
    std::string expected;
    port::read(port::path_utf8(std::string(argv[2])) / "tools" / "VERSION", expected, 1 << 16);
    while (!expected.empty() && (expected.back() == '\n' || expected.back() == '\r')) expected.pop_back();
    const port::arg_string root = port::arg(std::string(argv[2]));
    const std::string got = text([&](char *o, int c) { return system("launcher_version", root.c_str(), o, c); }, status);
    check(status == 0 && !expected.empty() && got == expected, "un_system launcher_version", got);
    check(steam() == 0 || steam() == 1, "un_steam_running");
    return failed;
}
