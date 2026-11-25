// gamefiles.cpp (in uncharted_native.dll) backs the "Check game files" button under Advanced -> FOLDERS. It compares
// what is in the player's game folder with tools/game_files_102.json.gz, a listing of the base game of the Collection
// that maps each relative path to its byte size. A size of -1 means "only check that it exists": the regions disagree
// on it, or update 1.02 ships its own copy. The listing carries file names and sizes, never game data, and skips
// sce_sys. Background: players whose dump was short one script file got stuck in chapter 8 of Uncharted 3 (#247, #296).

#include "native.h"
#include "json.h"
#include "inflate.h"
#include "port.h"

#include <cstdint>
#include <cstring>

#include <string>
#include <vector>

namespace {

using json::Value;
namespace fs = port::fs;

int give(const std::string &s, char *out, int cap) {
    if ((int)s.size() + 1 > cap) return -(int)(s.size() + 1);
    std::memcpy(out, s.c_str(), s.size() + 1);
    return 0;
}

bool read_file(const fs::path &path, std::string &data) { return port::read(path, data, ~(uint64_t)0); }

// Size of the file behind `path`, following links like Path.stat. Returns false if it can't be opened (e.g. absent).
bool size_of(const fs::path &path, long long &size) {
    std::error_code ec;
    const fs::file_status st = fs::status(path, ec);   // follows links
    if (ec || st.type() == fs::file_type::not_found) return false;
    if (fs::is_directory(st)) {
        size = 0;
        return true;
    }
    const uintmax_t n = fs::file_size(path, ec);
    if (ec) return false;
    size = (long long)n;
    return true;
}

}  // namespace

extern "C" {

// Runs the comparison and writes a report to out. Its first line reads "<folder>: N files compared, M not found, K with
// a different size"; after it come one "missing <file>" line per absent file and one "size    <file>" line per size
// mismatch. If the game folder has a uroot subfolder, that subfolder is what gets checked. Returns 0 when everything
// matches, 1 when something is absent or the wrong size, and 2 when the listing itself is unusable (out says why).
UN_API int un_check_game_files(const wchar_t *root, const wchar_t *game, char *out, int cap) {
    const fs::path list_path = port::path(root ? root : L".") / "tools" / "game_files_102.json.gz";
    std::string gz, text;
    Value wanted;
    if (!read_file(list_path, gz))
        return give("The game file list is missing: " + port::utf8(list_path), out, cap) < 0 ? -1 : 2;
    try {
        text = inflate::gunzip(gz);
    } catch (const inflate::InflateError &e) {
        return give("The game file list can't be read: " + e.message, out, cap) < 0 ? -1 : 2;
    }
    if (!json::parse(text, wanted) || wanted.kind != Value::Object)
        return give("The game file list can't be read.", out, cap) < 0 ? -1 : 2;
    fs::path folder = port::path(game ? game : L"");
    long long ignored = 0;
    if (size_of(folder / "uroot" / "eboot.bin", ignored)) folder /= "uroot";
    std::string missing, wrong;
    size_t missing_count = 0, wrong_count = 0;
    for (const auto &entry : wanted.fields) {
        long long actual = 0;
        if (!size_of(folder / port::path_utf8(entry.first), actual)) {
            missing += "missing " + entry.first + "\n";
            ++missing_count;
            continue;
        }
        const long long size = entry.second.kind == Value::Int ? entry.second.i : -1;
        if (size >= 0 && actual != size) {
            wrong += "size    " + entry.first + "\n";
            ++wrong_count;
        }
    }
    const std::string report = port::utf8(folder) + ": " + std::to_string(wanted.fields.size()) + " files compared, " +
                               std::to_string(missing_count) + " not found, " + std::to_string(wrong_count) +
                               " with a different size\n" + missing + wrong;
    const int status = give(report, out, cap);
    if (status < 0) return status;
    return missing_count || wrong_count ? 1 : 0;
}

}  // extern "C"
