// u2mp host_memory: the host memory calls of the module, one function per OS call (Windows and POSIX).
//
// host_readable: can the host read a range of its own address space, without touching it. The game image, the
// guest heaps and the host's own memory are all host memory here; a pointer read out of a guest record (possibly
// debug fill such as 0xaaaaaaaa...) is checked before it is followed.
// host_alloc: zeroed read/write memory of our own, never given back.
// HostCodeWrite: makes code writable for a while (a code patch made after the loader protected the image).
// Header only, so every module (and the u2mp rewrite tests) gets it without a link dependency.
#pragma once

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX   // keep std::min / std::max usable in the files that include this
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace u2mp {

// True when every byte of [p, p + n) is committed and readable. Windows: the region holding p is committed, not
// PAGE_NOACCESS / PAGE_GUARD, and the range ends inside it. POSIX: write() copies the range in the kernel, which
// answers EFAULT (instead of a fault here) when a byte is not mapped readable.
inline bool host_readable(const void *p, size_t n = 1) {
    const uintptr_t a = uintptr_t(p);
    if (n == 0 || a + n < a) return false;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION m{};
    if (VirtualQuery(p, &m, sizeof m) != sizeof m) return false;
    if (m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return a + n <= uintptr_t(m.BaseAddress) + m.RegionSize;
#else
    static const int *const pipe_fds = [] {
        static int fds[2] = {-1, -1};
        if (pipe(fds) == 0)
            for (int fd : fds) {
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
                fcntl(fd, F_SETFD, FD_CLOEXEC);
            }
        return fds;
    }();
    if (pipe_fds[1] < 0) return false;
    char sink[512];
    for (size_t done = 0; done < n;) {
        const size_t chunk = n - done < sizeof sink ? n - done : sizeof sink;
        const ssize_t written = write(pipe_fds[1], static_cast<const char *>(p) + done, chunk);
        if (written <= 0) return false;
        // Drain what this call wrote (another thread's bytes are as good: only the count matters).
        for (ssize_t left = written; left > 0;) {
            const ssize_t got = read(pipe_fds[0], sink, size_t(left));
            if (got <= 0) break;
            left -= got;
        }
        done += size_t(written);
    }
    return true;
#endif
}

// `bytes` of zeroed read/write memory, or nullptr. Windows: VirtualAlloc (reserved and committed); POSIX: an
// anonymous private mapping.
inline void *host_alloc(size_t bytes) {
#ifdef _WIN32
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}

// Makes the pages of [p, p + n) readable, writable and executable while it lives. When it ends, it flushes the
// instruction cache for the range and gives every page of it the protection the first page had before (as
// VirtualProtect reports and restores it). ok() false: nothing was changed, error() is the OS error
// (GetLastError / errno).
class HostCodeWrite {
public:
    HostCodeWrite(void *p, size_t n) : p_(p), n_(n) {
#ifdef _WIN32
        ok_ = VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old_) != 0;
        if (!ok_) error_ = GetLastError();
#else
        const uintptr_t page = uintptr_t(sysconf(_SC_PAGESIZE));
        start_ = uintptr_t(p) & ~(page - 1);
        length_ = ((uintptr_t(p) + n + page - 1) & ~(page - 1)) - start_;
        ok_ = old_protection(start_, old_) && mprotect(reinterpret_cast<void *>(start_), length_,
                                                       PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
        if (!ok_) error_ = static_cast<unsigned long>(errno);
#endif
    }
    ~HostCodeWrite() {
        if (!ok_) return;
#ifdef _WIN32
        FlushInstructionCache(GetCurrentProcess(), p_, n_);
        DWORD ignored = 0;
        VirtualProtect(p_, n_, old_, &ignored);
#else
        __builtin___clear_cache(static_cast<char *>(p_), static_cast<char *>(p_) + n_);
        mprotect(reinterpret_cast<void *>(start_), length_, old_);
#endif
    }
    HostCodeWrite(const HostCodeWrite &) = delete;
    HostCodeWrite &operator=(const HostCodeWrite &) = delete;

    bool ok() const { return ok_; }
    unsigned long error() const { return error_; }

private:
#ifndef _WIN32
    // The protection of the mapping holding `address`, from /proc/self/maps (false and errno set when it is not
    // there: no /proc, or the address is not mapped).
    static bool old_protection(uintptr_t address, int &prot) {
        FILE *maps = std::fopen("/proc/self/maps", "r");
        if (!maps) return false;
        bool found = false;
        unsigned long long from = 0, to = 0;
        char perms[5] = {};
        // "from-to perms offset device inode path", one mapping per line (the rest of the line is skipped)
        while (!found && std::fscanf(maps, "%llx-%llx %4s%*[^\n]", &from, &to, perms) == 3) {
            if (address < from || address >= to) continue;
            prot = (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) |
                   (perms[2] == 'x' ? PROT_EXEC : 0);
            found = true;
        }
        std::fclose(maps);
        if (!found) errno = ENOMEM;   // what mprotect answers for an address that is not mapped
        return found;
    }
    uintptr_t start_ = 0;
    size_t length_ = 0;
    int old_ = 0;
#else
    DWORD old_ = 0;
#endif
    void *p_;
    size_t n_;
    bool ok_ = false;
    unsigned long error_ = 0;
};

}  // namespace u2mp
