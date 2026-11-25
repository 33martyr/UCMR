// u2mp host_memory: can the host read a range of its own address space, without touching it.
//
// The game image, the guest heaps and the host's own memory are all host memory here; a pointer read out of a
// guest record (possibly debug fill such as 0xaaaaaaaa...) is checked before it is followed. Header only, so every
// module (and the u2mp rewrite tests) gets it without a link dependency.
#pragma once

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
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

}  // namespace u2mp
