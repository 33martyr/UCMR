// port.h - the few operating-system calls the launcher makes that the C++ library does not give directly. Windows and
// Linux bodies sit side by side here, behind the same name, so the code that uses them has no #ifdef. Everything else
// uses <filesystem>, <fstream>, <chrono> and <thread> directly.
//
// Text is UTF-8 everywhere inside the launcher. Wide strings exist only at the exported C API (native.h) and at the
// Windows API boundary; port::utf8 / port::wide are the only conversions.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <cstdlib>
#include <dlfcn.h>
#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace port {

namespace fs = std::filesystem;

// ------------------------------------------------------------------------------------------------ text

#ifdef _WIN32
inline std::string utf8(const std::wstring &w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::wstring wide(const std::string &s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
#else
inline std::string utf8(const std::wstring &w) {   // wchar_t is UTF-32 here
    std::string out;
    out.reserve(w.size() * 2);
    for (size_t i = 0; i < w.size(); ++i) {
        uint32_t c = (uint32_t)w[i];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < w.size()) {   // a UTF-16 surrogate pair that reached a wstring
            const uint32_t lo = (uint32_t)w[i + 1];
            if (lo >= 0xdc00 && lo <= 0xdfff) {
                c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
                ++i;
            }
        }
        if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) c = 0xfffd;
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) {
            out += (char)(0xc0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            out += (char)(0xe0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3f));
            out += (char)(0x80 | (c & 0x3f));
        } else {
            out += (char)(0xf0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3f));
            out += (char)(0x80 | ((c >> 6) & 0x3f));
            out += (char)(0x80 | (c & 0x3f));
        }
    }
    return out;
}

inline std::wstring wide(const std::string &s) {   // UTF-8 to UTF-32
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        uint32_t cp = 0xfffd;
        size_t n = 1;
        if (c < 0x80) cp = c;
        else if ((c & 0xe0) == 0xc0 && i + 1 < s.size()) { cp = (uint32_t)(c & 0x1f) << 6 | ((unsigned char)s[i + 1] & 0x3f); n = 2; }
        else if ((c & 0xf0) == 0xe0 && i + 2 < s.size()) {
            cp = (uint32_t)(c & 0x0f) << 12 | ((unsigned char)s[i + 1] & 0x3f) << 6 | ((unsigned char)s[i + 2] & 0x3f);
            n = 3;
        } else if ((c & 0xf8) == 0xf0 && i + 3 < s.size()) {
            cp = (uint32_t)(c & 0x07) << 18 | ((unsigned char)s[i + 1] & 0x3f) << 12 | ((unsigned char)s[i + 2] & 0x3f) << 6 |
                 ((unsigned char)s[i + 3] & 0x3f);
            n = 4;
        }
        out += (wchar_t)cp;
        i += n;
    }
    return out;
}
#endif

// the launcher's wide path text as a path. Linux: UTF-8, and a backslash (the Windows separator some joins and
// constant names still use) becomes "/"; no file name of ours or of the game contains one.
inline fs::path path(const std::wstring &w) {
#ifdef _WIN32
    return fs::path(w);
#else
    std::string s = utf8(w);
    for (char &c : s)
        if (c == '\\') c = '/';
    return fs::path(s);
#endif
}

#ifndef _WIN32
// an exported function's UTF-8 path argument (native.h un_path_char) as a path
inline fs::path path(const char *s) { return fs::path(s ? s : ""); }
#endif

// an exported function's path argument (native.h un_path_char: wchar_t on Windows, UTF-8 char on Linux) as the
// launcher's wide text; `fallback` when it is null
inline std::wstring wtext(const wchar_t *p, const wchar_t *fallback = L"") { return p ? std::wstring(p) : std::wstring(fallback); }
inline std::wstring wtext(const char *p, const wchar_t *fallback = L"") { return p ? wide(p) : std::wstring(fallback); }

// the other way: UTF-8 text as an exported function's path argument (wide on Windows, UTF-8 on Linux)
#ifdef _WIN32
using arg_string = std::wstring;
inline std::wstring arg(const std::string &s) { return wide(s); }
inline std::wstring arg(const std::wstring &w) { return w; }
#else
using arg_string = std::string;
inline std::string arg(const std::string &s) { return s; }
inline std::string arg(const std::wstring &w) { return utf8(w); }
#endif

// the native separator for wide joins
#ifdef _WIN32
inline constexpr wchar_t sep = L'\\';
#else
inline constexpr wchar_t sep = L'/';
#endif

// UTF-8 path text as a path (Linux: a backslash becomes "/", as in path() above)
inline fs::path path_utf8(const std::string &s) {
#ifdef _WIN32
    return fs::u8path(s);
#else
    std::string t = s;
    for (char &c : t)
        if (c == '\\') c = '/';
    return fs::path(t);
#endif
}

// opens a folder, file or web address with the desktop's default program and does not wait (Windows: ShellExecuteW
// "open"; Linux: xdg-open)
inline void open_path(const fs::path &p) {
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    const pid_t pid = ::fork();
    if (pid == 0) {   // a grandchild runs xdg-open, so nobody has to wait for it
        if (::fork() == 0) {
            ::execlp("xdg-open", "xdg-open", p.c_str(), (char *)nullptr);
            ::_exit(127);
        }
        ::_exit(0);
    }
    if (pid > 0) ::waitpid(pid, nullptr, 0);
#endif
}

// the same file path (Windows folds case, Linux does not)
inline bool same_path(const fs::path &a, const fs::path &b) {
#ifdef _WIN32
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
#else
    return a == b;
#endif
}

inline std::string utf8(const fs::path &p) {
#ifdef _WIN32
    return utf8(p.wstring());
#else
    return p.string();
#endif
}

// ------------------------------------------------------------------------------------------------ files

// false when the file is missing, not a regular file, or larger than `max`; the file is read whole otherwise
inline bool read(const fs::path &p, std::string &out, uint64_t max) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
    const uint64_t size = fs::file_size(p, ec);
    if (ec || size > max) return false;
    out.assign((size_t)size, '\0');
    if (!size) return true;
    std::FILE *f = nullptr;
#ifdef _WIN32
    f = _wfopen(p.c_str(), L"rb");
#else
    f = std::fopen(p.c_str(), "rb");
#endif
    if (!f) return false;
    const size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    out.resize(got);
    return got == size;
}

// up to `count` bytes from `at` (a negative `at` counts from the end of the file). `out` holds the bytes read, also
// when the file ended early; false when the file is missing, can't be opened or can't be seeked.
inline bool read_part(const fs::path &p, long long at, long long count, std::vector<uint8_t> &out) {
    out.clear();
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
    const long long size = (long long)fs::file_size(p, ec);
    if (ec) return false;
    if (at < 0) at = size + at < 0 ? 0 : size + at;
    if (at >= size || count <= 0) return true;
    if (count > size - at) count = size - at;
    std::FILE *f = nullptr;
#ifdef _WIN32
    f = _wfopen(p.c_str(), L"rb");
#else
    f = std::fopen(p.c_str(), "rb");
#endif
    if (!f) return false;
    bool ok = false;
#ifdef _WIN32
    const int moved = _fseeki64(f, at, SEEK_SET);
#else
    const int moved = fseeko(f, (off_t)at, SEEK_SET);
#endif
    if (moved == 0) {
        out.resize((size_t)count);
        const size_t got = std::fread(out.data(), 1, out.size(), f);
        out.resize(got);
        ok = got == (size_t)count;
    }
    std::fclose(f);
    return ok;
}

// Windows: MoveFileExW(MOVEFILE_REPLACE_EXISTING); Linux: rename(), which replaces atomically
inline bool replace(const fs::path &from, const fs::path &to) {
#ifdef _WIN32
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return ::rename(from.c_str(), to.c_str()) == 0;
#endif
}

// writes the whole file, truncating it (creates it when missing); same sharing as before on Windows
inline bool write(const fs::path &p, const void *data, size_t n) {
    std::FILE *f = nullptr;
#ifdef _WIN32
    f = _wfopen(p.c_str(), L"wb");
#else
    f = std::fopen(p.c_str(), "wb");
#endif
    if (!f) return false;
    const bool written = n == 0 || std::fwrite(data, 1, n, f) == n;
    const bool closed = std::fclose(f) == 0;
    return written && closed;
}

// a temporary file next to the target, then replace it
inline bool write_atomic(const fs::path &p, const void *data, size_t n) {
    if (p.empty()) return false;
    fs::path tmp = p;
    tmp += ".tmp";
#ifdef _WIN32
    std::FILE *f = _wfopen(tmp.c_str(), L"wb");
#else
    std::FILE *f = std::fopen(tmp.c_str(), "wb");
#endif
    if (!f) return false;
    const bool written = n == 0 || std::fwrite(data, 1, n, f) == n;
    const bool closed = std::fclose(f) == 0;
    if (!written || !closed || !replace(tmp, p)) {
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

inline bool ensure_dir(const fs::path &p) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) return true;
    fs::create_directories(p, ec);
    return fs::is_directory(p, ec);
}

// secrets: a compiler may not elide a write through a volatile pointer
inline void zero(void *p, size_t n) {
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

// ------------------------------------------------------------------------------------------------ environment

inline bool env(const char *name, std::string &out) {
#ifdef _WIN32
    const std::wstring wname = wide(name);   // names are ASCII, values may not be
    wchar_t buf[32768];
    const DWORD n = GetEnvironmentVariableW(wname.c_str(), buf, (DWORD)(sizeof buf / sizeof *buf));
    if (!n || n >= sizeof buf / sizeof *buf) return false;
    out = utf8(std::wstring(buf, n));
    return true;
#else
    const char *v = std::getenv(name);
    if (!v) return false;
    out = v;
    return true;
#endif
}

// ------------------------------------------------------------------------------------------------ the program and its libraries

inline fs::path executable_path() {
#ifdef _WIN32
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
    buf.resize(n);
    return fs::path(buf);
#else
    std::vector<char> buf(4096);
    for (;;) {
        const ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size());
        if (n < 0) return fs::path();
        if ((size_t)n < buf.size()) return fs::path(std::string(buf.data(), (size_t)n));
        buf.resize(buf.size() * 2);
    }
#endif
}

inline void *load_library(const fs::path &p) {
#ifdef _WIN32
    return (void *)LoadLibraryExW(p.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
    return ::dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

// a library the system's loader finds by name (SDL3 on Linux: "libSDL3.so.0")
inline void *load_library_name(const std::string &name) {
#ifdef _WIN32
    return (void *)LoadLibraryW(wide(name).c_str());
#else
    return ::dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

inline void *symbol(void *library, const char *name) {
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)library, name);
#else
    return ::dlsym(library, name);
#endif
}

// ------------------------------------------------------------------------------------------------ the machine

// this process's id, used in temporary file names
inline unsigned long process_id() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return (unsigned long)::getpid();
#endif
}

// logical CPUs this process may run on, like GetProcessAffinityMask / sched_getaffinity; 8 when unknown
inline int hardware_threads() {
#ifdef _WIN32
    DWORD_PTR process = 0, system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
        int n = 0;
        for (DWORD_PTR m = process; m; m &= m - 1) ++n;
        return n;
    }
    const DWORD all = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return all ? (int)all : 8;
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        int n = 0;
        for (int i = 0; i < CPU_SETSIZE; ++i)
            if (CPU_ISSET(i, &set)) ++n;
        if (n) return n;
    }
    const unsigned n = std::thread::hardware_concurrency();
    return n ? (int)n : 8;
#endif
}

// the OS's own description of an error code (Windows: FormatMessage; Linux: strerror), without a trailing newline
inline std::string error_text(unsigned long code) {
#ifdef _WIN32
    wchar_t *text = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, (DWORD)code, 0, (LPWSTR)&text, 0, nullptr);
    std::string out;
    if (n && text) out = utf8(std::wstring(text, n));
    if (text) LocalFree(text);
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' ' || out.back() == '\t'))
        out.pop_back();
    return out.empty() ? "error" : out;
#else
    const char *text = std::strerror((int)code);
    return text ? text : "error";
#endif
}

// ------------------------------------------------------------------------------------------------ time

// milliseconds on a monotonic clock; the epoch differs from GetTickCount64's, only differences are used
inline long long now_ms() {
    using namespace std::chrono;
    return (long long)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ------------------------------------------------------------------------------------------------ later phases

// a feature whose Linux body comes in a later phase (docs/WINDOWS-DEPENDENCIES.md §13): one log line on stderr, and
// the text the caller hands back as its reason
inline const char *not_yet(const char *what) {
    std::fprintf(stderr, "uncharted_native: %s: not on Linux yet\n", what);
    return "not on Linux yet";
}

// the core's text answer convention (native.h): 0 when `s` fits in `out`, else -(the size needed)
inline int give(const std::string &s, char *out, int cap) {
    if ((int)s.size() + 1 > cap) return -(int)(s.size() + 1);
    std::memcpy(out, s.c_str(), s.size() + 1);
    return 0;
}

// a stub's answer: "<what>: not on Linux yet" in `out`, then `status` (or the size needed)
inline int not_yet_answer(const char *what, int status, char *out, int cap) {
    const int r = give(std::string(what) + ": " + not_yet(what), out, cap);
    return r < 0 ? r : status;
}

}   // namespace port
