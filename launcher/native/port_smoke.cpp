// port_smoke.cpp - checks port.h's Linux process layer (docs/WINDOWS-DEPENDENCIES.md §12): start, pipes, working
// folder, environment, exit codes, a missing program, time-outs, the process list and kill_tree. Linux CI runs it;
// one "ok"/"FAIL" line per check, exit code 1 when one failed.
#include "port.h"

#include <cstdio>
#include <string>

#ifndef _WIN32
namespace {

int failed = 0;

void check(bool ok, const std::string &what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failed;
}

std::string drain(port::Child &c) {
    std::string out;
    char buf[256];
    long n;
    while ((n = port::read_some(c.out, buf, sizeof buf, -1)) > 0) out.append(buf, (size_t)n);
    return out;
}

}   // namespace

int main() {
    std::string error;
    port::Child c;

    port::Spawn s;
    s.argv = {"sh", "-c", "echo out; echo err >&2; exit 3"};
    s.out = port::Spawn::pipe;
    const bool started = port::spawn(s, c, error);
    check(started, "start a program from PATH " + error);
    const std::string both = drain(c);
    check(both == "out\nerr\n", "stdout and stderr on one pipe: [" + both + "]");
    check(port::wait(c) == 3, "its exit code");
    port::close(c);

    s.err = port::Spawn::err_pipe;
    check(port::spawn(s, c, error), "start with stderr apart");
    std::string err;
    char buf[64];
    long n;
    while ((n = port::read_some(c.err, buf, sizeof buf, -1)) > 0) err.append(buf, (size_t)n);
    check(drain(c) == "out\n" && err == "err\n", "stderr on its own pipe");
    port::wait(c);
    port::close(c);

    port::Spawn e;
    e.argv = {"sh", "-c", "pwd; echo \"$UCMR_SMOKE\"; read line; echo \"got $line\""};
    e.cwd = "/tmp";
    e.set_env = true;
    e.env = {"UCMR_SMOKE=value", "PATH=/usr/bin:/bin"};
    e.out = port::Spawn::pipe;
    e.in_pipe = true;
    check(port::spawn(e, c, error), "start with a folder, an environment and a stdin pipe");
    check(port::write_all(c.in, "hello\n", 6), "write its stdin");
    const std::string seen = drain(c);
    check(seen == "/tmp\nvalue\ngot hello\n", "folder, environment and stdin reached it: [" + seen + "]");
    port::wait(c);
    port::close(c);

    port::Spawn missing;
    missing.argv = {"/nonexistent/ucmr-smoke"};
    const bool failed_to_start = !port::spawn(missing, c, error) && !error.empty();
    check(failed_to_start, "a missing program fails to start: " + error);

    port::Spawn sleeper;
    sleeper.argv = {"sh", "-c", "sleep 30 & sleep 30"};
    check(port::spawn(sleeper, c, error), "start a program that starts another");
    check(port::wait(c, 100) == -1 && port::running(c), "a time-out leaves it running");
    bool listed = false;
    for (const port::ProcessInfo &p : port::list_processes())
        if (p.pid == c.pid) listed = p.parent == (long long)::getpid() && !p.name.empty();
    check(listed, "the process list has it, with its parent and name");
    check(port::process_alive(c.pid), "process_alive while it runs");
    long long grandchild = 0;
    for (int i = 0; i < 100 && !grandchild; ++i, std::this_thread::sleep_for(std::chrono::milliseconds(10)))
        for (const port::ProcessInfo &p : port::list_processes())
            if (p.parent == c.pid) grandchild = p.pid;
    check(grandchild > 0, "its child is listed under it");
    port::kill_tree(c.pid);
    check(port::wait(c) == 128 + SIGKILL, "kill_tree ends it (SIGKILL)");
    bool gone = false;
    for (int i = 0; i < 200 && !gone; ++i, std::this_thread::sleep_for(std::chrono::milliseconds(10)))
        gone = !port::process_alive(grandchild) ||
               [&] {   // a killed grandchild stays a zombie until init reaps it: count that as gone
                   std::string stat;
                   std::FILE *f = std::fopen(("/proc/" + std::to_string(grandchild) + "/stat").c_str(), "rb");
                   if (!f) return true;
                   char b[512];
                   stat.assign(b, std::fread(b, 1, sizeof b, f));
                   std::fclose(f);
                   const size_t at = stat.rfind(')');
                   return at != std::string::npos && at + 2 < stat.size() && stat[at + 2] == 'Z';
               }();
    check(gone, "kill_tree ends its child too");

    check(port::program_name("u3fix.exe") == "u3fix" && port::program_name("U3IMPORT.EXE") == "U3IMPORT" &&
              port::program_name("ffmpeg") == "ffmpeg" && port::exe_name("u3fix") == "u3fix",
          "program names without .exe");

    std::printf("%s\n", failed ? "some checks failed" : "all checks passed");
    return failed ? 1 : 0;
}
#else
int main() { return 0; }   // Windows: the callers keep their own CreateProcessW bodies
#endif
