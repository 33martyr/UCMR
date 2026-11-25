// u2mp online link: starts u2_online.exe and speaks u2_online_rpc.h with it (online_link.h).
//
// One reader thread takes the program's frames: replies go to the call waiting for them (one call at a time),
// datagrams and events go to queues that recv / poll read. The program ends with this process (Windows: a job
// object; Linux: its parent-death signal); closing our end of its stdin ends it too.
#include "online_link.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
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
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif

#include "u2_online_rpc.h"

namespace u2mp::online_link {
namespace {

using namespace u2o_rpc;

constexpr auto kCallTimeout = std::chrono::seconds(10);
constexpr auto kHelloTimeout = std::chrono::seconds(10);
constexpr size_t kMaxQueued = 4096;   // datagrams kept for the game (the oldest go first), as the link itself does
constexpr size_t kPipeBytes = 1u << 20;

// ---------------------------------------------------------------------------------------------
// The OS part: starting the program, its stdin / stdout, ending it.

#ifdef _WIN32

struct Program {
    HANDLE to = nullptr, from = nullptr, process = nullptr, job = nullptr;
};

bool read_all(Program &p, void *data, size_t n) {
    auto *b = static_cast<uint8_t *>(data);
    while (n) {
        DWORD got = 0;
        if (!ReadFile(p.from, b, DWORD(std::min<size_t>(n, 1u << 20)), &got, nullptr) || !got) return false;
        b += got;
        n -= got;
    }
    return true;
}

bool write_all(Program &p, const void *data, size_t n) {
    auto *b = static_cast<const uint8_t *>(data);
    while (n) {
        DWORD put = 0;
        if (!WriteFile(p.to, b, DWORD(std::min<size_t>(n, 1u << 20)), &put, nullptr) || !put) return false;
        b += put;
        n -= put;
    }
    return true;
}

void end_program(Program &p) { TerminateProcess(p.process, 1); }

std::wstring quoted(const wchar_t *s) {
    std::wstring q = L"\"";
    q += s;
    q += L"\"";
    return q;
}

bool launch(const Path &exe, Program &p, std::string &why) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr;
    if (!CreatePipe(&in_r, &in_w, &sa, DWORD(kPipeBytes)) || !CreatePipe(&out_r, &out_w, &sa, DWORD(kPipeBytes))) {
        why = "no pipe (Windows error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    // the program inherits exactly its two pipe ends
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<uint8_t> attr_buf(attr_size);
    auto *attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    HANDLE inherit[2] = {in_r, out_w};
    const bool attrs_ok = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
                          UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit,
                                                    nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_r;
    si.StartupInfo.hStdOutput = out_w;
    si.StartupInfo.hStdError = nullptr;
    si.lpAttributeList = attrs_ok ? attrs : nullptr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = quoted(exe.c_str());
    const BOOL made = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                     CREATE_NO_WINDOW | CREATE_SUSPENDED | (attrs_ok ? EXTENDED_STARTUPINFO_PRESENT : 0),
                                     nullptr, nullptr, &si.StartupInfo, &pi);
    const DWORD err = GetLastError();
    if (attrs_ok) DeleteProcThreadAttributeList(attrs);
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (!made) {
        CloseHandle(in_w);
        CloseHandle(out_r);
        why = "u2_online.exe did not start (Windows error " + std::to_string(err) + ")";
        return false;
    }
    // ends the program with this process, however it ends
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
        AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    p.to = in_w;
    p.from = out_r;
    p.process = pi.hProcess;
    p.job = job;
    return true;
}

#else

// The program's process as its keeper thread sees it (never freed, like the client).
struct Child {
    std::mutex m;
    std::condition_variable cv;
    pid_t pid = 0;           // 0 until fork answered, -1 when it failed
    int fork_error = 0;
    bool exited = false;     // ended and about to be reaped: its pid may name another process from now on
};

// One stream socket pair stands in for the two pipes: the program gets its end as stdin and stdout, ours is
// read for its frames and written with MSG_NOSIGNAL (a pipe write after the program ended would raise SIGPIPE
// and end the game; WriteFile only fails).
struct Program {
    int fd = -1;
    Child *child = nullptr;
};

bool read_all(Program &p, void *data, size_t n) {
    auto *b = static_cast<uint8_t *>(data);
    while (n) {
        const ssize_t got = ::read(p.fd, b, std::min<size_t>(n, 1u << 20));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        b += got;
        n -= size_t(got);
    }
    return true;
}

bool write_all(Program &p, const void *data, size_t n) {
    auto *b = static_cast<const uint8_t *>(data);
    while (n) {
        const ssize_t put = ::send(p.fd, b, std::min<size_t>(n, 1u << 20), MSG_NOSIGNAL);
        if (put < 0 && errno == EINTR) continue;
        if (put <= 0) return false;
        b += put;
        n -= size_t(put);
    }
    return true;
}

void end_program(Program &p) {
    std::lock_guard<std::mutex> l(p.child->m);
    if (p.child->pid > 0 && !p.child->exited) kill(p.child->pid, SIGKILL);
}

// The forked child up to exec: async-signal-safe calls only. Never returns.
[[noreturn]] void run_child(char *const argv[], int stdio, int devnull, int status, pid_t parent, long max_fd) {
#ifdef __linux__
    // ends the program with this process, however it ends (the signal follows the forking thread, the keeper,
    // which lives until the program ended)
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != parent) _exit(127);   // this process ended before the line above
#else
    (void)parent;
#endif
    dup2(stdio, 0);
    dup2(stdio, 1);
    if (devnull >= 0) dup2(devnull, 2); else ::close(2);   // no stderr, as on Windows
    // the program inherits exactly its stdin / stdout (and stderr); `status` closes itself on exec
    bool closed = false;
#ifdef SYS_close_range
    closed = (status == 3 || syscall(SYS_close_range, 3u, unsigned(status - 1), 0u) == 0) &&
             syscall(SYS_close_range, unsigned(status + 1), ~0u, 0u) == 0;
#endif
    for (int fd = 3; !closed && fd < max_fd; ++fd)
        if (fd != status) ::close(fd);
    execv(argv[0], argv);
    const int err = errno;
    ssize_t ignored = write(status, &err, sizeof err);
    (void)ignored;
    _exit(127);
}

bool launch(const Path &exe, Program &p, std::string &why) {
    int pair[2], status[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
        why = "no pipe (error " + std::to_string(errno) + ")";
        return false;
    }
    if (pipe2(status, O_CLOEXEC) != 0) {
        why = "no pipe (error " + std::to_string(errno) + ")";
        ::close(pair[0]);
        ::close(pair[1]);
        return false;
    }
    const int bytes = int(kPipeBytes);   // as much as the Windows pipes hold (the kernel may give less)
    for (int fd : pair) setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof bytes);
    const int devnull = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    const long max_fd = sysconf(_SC_OPEN_MAX) > 0 ? sysconf(_SC_OPEN_MAX) : 1024;
    auto *child = new Child;
    // The keeper thread forks the program and then waits for its end (and reaps it).
    std::thread([child, exe, stdio = pair[1], devnull, status_w = status[1], parent = getpid(), max_fd] {
        char *const argv[] = {const_cast<char *>(exe.c_str()), nullptr};
        const pid_t pid = fork();
        if (pid == 0) run_child(argv, stdio, devnull, status_w, parent, max_fd);
        const int err = errno;
        ::close(stdio);
        if (devnull >= 0) ::close(devnull);
        ::close(status_w);
        {
            std::lock_guard<std::mutex> l(child->m);
            child->pid = pid < 0 ? -1 : pid;
            child->fork_error = err;
        }
        child->cv.notify_all();
        if (pid < 0) return;
        siginfo_t info{};
        while (waitid(P_PID, id_t(pid), &info, WEXITED | WNOWAIT) != 0 && errno == EINTR) {
        }
        {
            std::lock_guard<std::mutex> l(child->m);
            child->exited = true;
        }
        waitpid(pid, nullptr, 0);
    }).detach();
    int err = 0;
    {
        std::unique_lock<std::mutex> l(child->m);
        child->cv.wait(l, [child] { return child->pid != 0; });
        if (child->pid < 0) err = child->fork_error;
    }
    if (!err) {   // the status pipe closes at exec, or brings exec's error
        ssize_t got;
        while ((got = ::read(status[0], &err, sizeof err)) < 0 && errno == EINTR) {
        }
        if (got <= 0) err = 0;
    }
    ::close(status[0]);
    if (err) {
        ::close(pair[0]);
        why = "u2_online.exe did not start (error " + std::to_string(err) + ")";
        return false;
    }
    p.fd = pair[0];
    p.child = child;
    return true;
}

#endif

// ---------------------------------------------------------------------------------------------

struct Datagram {
    char npid[20];
    std::vector<uint8_t> data;
};

struct Client {
    Program program;
    std::mutex call_m;     // one call at a time
    std::mutex write_m;    // whole frames
    std::mutex m;          // everything below
    std::condition_variable cv;
    bool reply_ready = false;
    std::vector<uint8_t> reply;
    int hello = -1;
    std::atomic<bool> dead{false};
    std::map<uint16_t, std::deque<Datagram>> inbox;
    size_t queued = 0;
    std::deque<std::vector<uint8_t>> events;
    // only the game side
    Event current;
    bool opened = false, lost_reported = false;
};
Client *g = nullptr;   // never freed: the reader thread may run until the process ends

void mark_dead() {
    {
        std::lock_guard<std::mutex> l(g->m);
        g->dead = true;
    }
    g->cv.notify_all();
}

void reader() {
    std::vector<uint8_t> f;
    for (;;) {
        uint32_t size = 0;
        if (!read_all(g->program, &size, 4) || !size || size > kMaxFrame) break;
        f.resize(size);
        if (!read_all(g->program, f.data(), size)) break;
        const uint8_t op = f[0];
        In in(f.data() + 1, size - 1);
        std::unique_lock<std::mutex> l(g->m);
        if (op == R_REPLY) {
            g->reply.assign(f.begin() + 1, f.end());
            g->reply_ready = true;
            l.unlock();
            g->cv.notify_all();
        } else if (op == P_DATAGRAM) {
            const uint16_t vport = in.get<uint16_t>();
            const uint8_t *npid = in.raw(20);
            if (!in.ok()) continue;
            Datagram d;
            std::memcpy(d.npid, npid, 20);
            d.npid[19] = 0;
            d.data.assign(in.p, in.end);
            if (g->queued >= kMaxQueued) {   // drop the oldest of the fullest port
                auto fullest = std::max_element(g->inbox.begin(), g->inbox.end(),
                                                 [](const auto &a, const auto &b) { return a.second.size() < b.second.size(); });
                fullest->second.pop_front();
                --g->queued;
            }
            g->inbox[vport].push_back(std::move(d));
            ++g->queued;
        } else if (op == P_EVENT) {
            g->events.emplace_back(f.begin() + 1, f.end());
        } else if (op == P_HELLO) {
            g->hello = in.get<int32_t>();
            l.unlock();
            g->cv.notify_all();
        }
    }
    mark_dead();
}

bool write_frame(Out &o) {
    if (!g || g->dead) return false;
    const std::vector<uint8_t> &f = o.frame();
    std::lock_guard<std::mutex> l(g->write_m);
    if (write_all(g->program, f.data(), f.size())) return true;
    mark_dead();
    return false;
}

// A call with a reply: sends the frame and waits. False: no program, or it did not answer (then it is ended).
bool call(Out &o, std::vector<uint8_t> &reply) {
    if (!g || g->dead) return false;
    std::lock_guard<std::mutex> cl(g->call_m);
    {
        std::lock_guard<std::mutex> l(g->m);
        g->reply_ready = false;
    }
    if (!write_frame(o)) return false;
    std::unique_lock<std::mutex> l(g->m);
    if (!g->cv.wait_for(l, kCallTimeout, [] { return g->reply_ready || g->dead.load(); }) || !g->reply_ready) {
        if (!g->dead) {   // hung: end it, the game goes on without online
            g->dead = true;
            end_program(g->program);
        }
        return false;
    }
    reply.swap(g->reply);
    g->reply_ready = false;
    return true;
}

// a call whose reply is only the u64 result (0 when the call failed)
uint64_t call_u64(Out &o) {
    std::vector<uint8_t> r;
    if (!call(o, r)) return 0;
    In in(r.data(), r.size());
    const uint64_t v = in.get<uint64_t>();
    return in.ok() ? v : 0;
}

}  // namespace

bool env_path(const char *name, Path &path) {
    constexpr size_t kCap = 1024;
#ifdef _WIN32
    const std::wstring wide(name, name + std::strlen(name));
    wchar_t value[kCap];
    const DWORD n = GetEnvironmentVariableW(wide.c_str(), value, DWORD(kCap));
    if (!n || n >= kCap) return false;
    path.assign(value, n);
#else
    const char *value = std::getenv(name);
    if (!value || !*value || std::strlen(value) >= kCap) return false;
    path = value;
#endif
    return true;
}

bool start(const Path &exe, std::string &why) {
    static bool tried = false;
    if (tried) {
        why = g ? "" : "the online program did not start before";
        return g && !g->dead;
    }
    tried = true;
    Program program;
    if (!launch(exe, program, why)) return false;
    g = new Client;
    g->program = program;
    std::thread(reader).detach();
    std::unique_lock<std::mutex> l(g->m);
    g->cv.wait_for(l, kHelloTimeout, [] { return g->hello != -1 || g->dead.load(); });
    if (g->hello != U2O_ABI) {
        why = g->hello == -1 ? "u2_online.exe did not answer"
                             : "u2_online.exe has ABI " + std::to_string(g->hello) + ", this build needs " +
                                   std::to_string(int(U2O_ABI));
        g->dead = true;
        end_program(g->program);
        return false;
    }
    return true;
}

bool alive() { return g && !g->dead; }

int abi() { return int(call_u64(Out(ABI).put(int32_t(0)))); }

int open(const char *relay, const char *com_id) {
    Out o(OPEN);
    o.str(relay).str(com_id);
    const int r = int(call_u64(o));
    if (g) g->opened = true;
    return r;
}

int state(char *text, int cap) {
    std::vector<uint8_t> r;
    Out o(STATE);
    o.put(int32_t(cap));
    if (!call(o, r)) {
        if (text && cap > 0) std::snprintf(text, size_t(cap), "%s", "the online program stopped");
        return U2O_FAILED;
    }
    In in(r.data(), r.size());
    const int32_t st = int32_t(in.get<uint64_t>());
    std::string t;
    in.str(t);
    if (text && cap > 0) std::snprintf(text, size_t(cap), "%s", t.c_str());
    return in.ok() ? st : U2O_FAILED;
}

int poll(U2oEvent *e) {
    if (!e || !g) return 0;
    for (;;) {
        std::vector<uint8_t> f;
        {
            std::lock_guard<std::mutex> l(g->m);
            if (g->events.empty()) break;
            f.swap(g->events.front());
            g->events.pop_front();
        }
        In in(f.data(), f.size());
        if (!g->current.decode(in)) continue;   // malformed: skip it
        *e = g->current.e;
        return 1;
    }
    if (g->dead && g->opened && !g->lost_reported) {   // the program ended: the link is gone with it
        g->lost_reported = true;
        g->current = Event{};
        g->current.e.kind = U2O_EV_LINK_LOST;
        g->current.text = "the online program stopped";
        g->current.has_text = true;
        g->current.fix();
        *e = g->current.e;
        return 1;
    }
    return 0;
}

void close() {
    Out o(CLOSE);
    call_u64(o);
}

uint64_t find_world() { return call_u64(Out(FIND_WORLD).put(int32_t(0))); }

uint64_t create_room(uint32_t world, const U2oCreate *c) {
    if (!c) return 0;
    Out o(CREATE_ROOM);
    U2oCreate h = *c;
    h.attrs = nullptr;
    h.member_bin = nullptr;
    o.put(world).put(h);
    put_attrs(o, c->attrs, c->attr_count);
    o.bytes(c->member_bin, c->member_bin_len);
    return call_u64(o);
}

uint64_t join_room(uint64_t room_id, const uint8_t *member_bin, uint32_t member_bin_len, uint8_t team) {
    Out o(JOIN_ROOM);
    o.put(room_id).put(team).bytes(member_bin, member_bin_len);
    return call_u64(o);
}

uint64_t leave_room(uint64_t room_id) { return call_u64(Out(LEAVE_ROOM).put(room_id)); }

uint64_t search_rooms(uint32_t world, const U2oSearch *s) {
    if (!s) return 0;
    Out o(SEARCH_ROOMS);
    U2oSearch h = *s;
    h.bin_data = nullptr;
    o.put(world).put(h).bytes(s->bin_data, s->bin_len);
    return call_u64(o);
}

uint64_t set_room_external(uint64_t room_id, const U2oAttr *attrs, uint32_t count) {
    Out o(SET_ROOM_EXTERNAL);
    o.put(room_id).put(uint8_t(attrs ? 1 : 0));
    put_attrs(o, attrs, count);
    return call_u64(o);
}

uint64_t set_room_internal(uint64_t room_id, uint32_t flag_filter, uint32_t flag_attr, const U2oAttr *attrs,
                           uint32_t count) {
    Out o(SET_ROOM_INTERNAL);
    o.put(room_id).put(flag_filter).put(flag_attr).put(uint8_t(attrs ? 1 : 0));
    put_attrs(o, attrs, count);
    return call_u64(o);
}

uint64_t set_member_internal(uint64_t room_id, uint16_t member_id, uint8_t team, const uint8_t *bin, uint32_t len) {
    Out o(SET_MEMBER_INTERNAL);
    o.put(room_id).put(member_id).put(team).bytes(bin, len);
    return call_u64(o);
}

uint64_t members_external(uint64_t room_id) { return call_u64(Out(MEMBERS_EXTERNAL).put(room_id)); }

uint64_t peer_address(const char *npid) {
    Out o(PEER_ADDRESS);
    o.str(npid);
    return call_u64(o);
}

int peer_connect(const char *npid, const uint8_t ip[4], uint16_t port) {
    Out o(PEER_CONNECT);
    o.str(npid).bytes(ip, ip ? 4 : 0).put(port);
    return int(call_u64(o));
}

void peer_disconnect(const char *npid) {   // no reply
    Out o(PEER_DISCONNECT);
    o.str(npid);
    write_frame(o);
}

int peer_state(const char *npid, uint32_t *rtt_us) {
    std::vector<uint8_t> r;
    Out o(PEER_STATE);
    o.str(npid);
    if (!call(o, r)) return U2O_PEER_INACTIVE;
    In in(r.data(), r.size());
    const int st = int(in.get<uint64_t>());
    const uint32_t rtt = in.get<uint32_t>();
    if (!in.ok()) return U2O_PEER_INACTIVE;
    if (rtt_us) *rtt_us = rtt;
    return st;
}

int send(const char *npid, uint16_t dst_vport, uint16_t src_vport, const uint8_t *data, uint32_t len) {
    if (!npid || (!data && len)) return 0;   // no reply: the frame is the packet
    Out o(SEND);
    o.str(npid).put(dst_vport).put(src_vport).raw(data, len);
    return write_frame(o) ? 1 : 0;
}

int recv(uint16_t vport, uint8_t *data, uint32_t cap, char npid[20]) {
    if (!g || !data) return 0;
    std::lock_guard<std::mutex> l(g->m);
    auto it = g->inbox.find(vport);
    if (it == g->inbox.end() || it->second.empty()) return 0;
    Datagram &d = it->second.front();
    int n = -1;   // too large for the buffer: dropped (it would block the port otherwise)
    if (d.data.size() <= cap) {
        n = int(d.data.size());
        if (n) std::memcpy(data, d.data.data(), d.data.size());
        if (npid) std::memcpy(npid, d.npid, 20);
    }
    it->second.pop_front();
    --g->queued;
    return n;
}

int identity(char npid[20], char name[52]) {
    std::vector<uint8_t> r;
    if (!call(Out(IDENTITY).put(int32_t(0)), r)) return 0;
    In in(r.data(), r.size());
    const int ok = int(in.get<uint64_t>());
    const uint8_t *n = in.raw(20), *nm = in.raw(52);
    if (!in.ok()) return 0;
    if (npid) { std::memcpy(npid, n, 20); npid[19] = 0; }
    if (name) { std::memcpy(name, nm, 52); name[51] = 0; }
    return ok;
}

int friends(U2oFriend *out, int cap) {
    std::vector<uint8_t> r;
    if (!out || cap < 0) cap = 0;
    if (!call(Out(FRIENDS).put(int32_t(cap)), r)) return 0;
    In in(r.data(), r.size());
    const int total = int(in.get<uint64_t>());
    const uint32_t n = in.get<uint32_t>();
    if (!in.ok() || n > uint32_t(cap)) return 0;
    const uint8_t *d = in.raw(sizeof(U2oFriend) * n);
    if (!d) return 0;
    if (n) std::memcpy(static_cast<void *>(out), d, sizeof(U2oFriend) * n);
    return total;
}

uint64_t set_presence(const char *title, const char *status, const char *comment, const uint8_t *data, uint32_t len) {
    Out o(SET_PRESENCE);
    o.str(title).str(status).str(comment).bytes(data, len);
    return call_u64(o);
}

uint64_t send_message(const char *const *npids, uint32_t count, const U2oMessage *m) {
    if (!m || (count && !npids) || count > 64 || (m->blob_len && !m->blob)) return 0;
    Out o(SEND_MESSAGE);
    o.put(count);
    for (uint32_t i = 0; i < count; ++i) o.str(npids[i]);
    U2oMessage h = *m;
    h.blob = nullptr;
    o.put(h).bytes(m->blob, m->blob ? m->blob_len : 0);
    return call_u64(o);
}

uint64_t set_owner_rank(uint64_t room_id, const uint16_t *member_ids, uint32_t count) {
    Out o(SET_OWNER_RANK);
    o.put(room_id).bytes(member_ids, member_ids ? count * 2 : 0);
    return call_u64(o);
}

int friend_requests(int in_flag, char *out, int cap) {
    std::vector<uint8_t> r;
    if (!out || cap < 0) cap = 0;
    if (!call(Out(FRIEND_REQUESTS).put(int32_t(in_flag)).put(int32_t(cap)), r)) return 0;
    In in(r.data(), r.size());
    const int total = int(in.get<uint64_t>());
    const uint8_t *d;
    uint32_t n;
    in.bytes(d, n);
    if (!in.ok() || n > uint32_t(cap)) return 0;
    if (n) std::memcpy(out, d, n);
    return total;
}

uint64_t add_friend(const char *npid) {
    Out o(ADD_FRIEND);
    o.str(npid);
    return call_u64(o);
}

uint64_t remove_friend(const char *npid) {
    Out o(REMOVE_FRIEND);
    o.str(npid);
    return call_u64(o);
}

uint64_t record_score(uint32_t board, int64_t score, const char *comment, const uint8_t *info, uint32_t info_len) {
    Out o(RECORD_SCORE);
    o.put(board).put(score).str(comment).bytes(info, info_len);
    return call_u64(o);
}

uint64_t get_scores(uint32_t board, int mode, uint32_t start, uint32_t count) {
    return call_u64(Out(GET_SCORES).put(board).put(int32_t(mode)).put(start).put(count));
}

}  // namespace u2mp::online_link
