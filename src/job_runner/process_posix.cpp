#include "job_runner/process.hpp"
#include "job_runner/common.hpp"
#include "platform/paths.hpp"
#include <array>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/proc.h>
#endif

namespace lubancode::job_runner {
std::string RandomHex() {
    std::array<unsigned char, 16> bytes{};
    const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw Error("runner.entropy_unavailable");
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto count = read(fd, bytes.data() + done, bytes.size() - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { close(fd); throw Error("runner.entropy_unavailable"); }
        done += static_cast<std::size_t>(count);
    }
    close(fd);
    std::string out;
    for (auto c : bytes) { out += "0123456789abcdef"[c >> 4]; out += "0123456789abcdef"[c & 15]; }
    return out;
}
void VerifyPrivateDirectory(const fs::path& path) {
    struct stat info {};
    if (lstat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != geteuid() ||
        (info.st_mode & 0077) != 0) throw Error("runner.state_not_private");
}
std::string ProcessScope() { return "posix_process_group"; }

namespace {
#ifdef __APPLE__
bool OnlyExitedGroupMembers(pid_t leader) {
    // XNU killpg1 filters SZOMB before counting permitted targets, so an
    // otherwise empty, zombie-anchored group can return EPERM. Do not confuse
    // that with permission denied for a live descendant: inspect the complete
    // KERN_PROC_PGRP result, which includes both live and zombie processes.
    int query[] = {CTL_KERN, KERN_PROC, KERN_PROC_PGRP, leader};
    for (int attempt = 0; attempt != 3; ++attempt) {
        std::size_t bytes = 0;
        if (sysctl(query, 4, nullptr, &bytes, nullptr, 0) != 0 || bytes == 0 || bytes > 1024 * 1024) return false;
        std::vector<kinfo_proc> members((bytes + sizeof(kinfo_proc) - 1) / sizeof(kinfo_proc));
        bytes = members.size() * sizeof(kinfo_proc);
        if (sysctl(query, 4, members.data(), &bytes, nullptr, 0) != 0) {
            if (errno == ENOMEM) continue;
            return false;
        }
        if (bytes % sizeof(kinfo_proc) != 0 || bytes > members.size() * sizeof(kinfo_proc)) return false;
        bool saw_anchor = false;
        for (std::size_t i = 0; i < bytes / sizeof(kinfo_proc); ++i) {
            if (members[i].kp_eproc.e_pgid != leader || members[i].kp_proc.p_stat != SZOMB) return false;
            if (members[i].kp_proc.p_pid == leader) saw_anchor = true;
        }
        return saw_anchor;
    }
    return false;
}
#endif
void StopOwnedGroup(pid_t leader, bool leader_exited) {
    if (kill(-leader, SIGKILL) == 0) return;
    const int failure = errno;
    if (failure == ESRCH) return;
#ifdef __APPLE__
    if (failure == EPERM && leader_exited && OnlyExitedGroupMembers(leader)) return;
#else
    (void)leader_exited;
#endif
    // Only a fixed operation and errno enter the local supervisor diagnostic.
    std::cerr << "runner.group_signal_failed errno=" << failure << " leader_exited=" << leader_exited << '\n';
    throw Error("runner.cancel_failed");
}
}

struct Process::Impl {
    pid_t child = 0;
    int gate = -1;
    std::optional<std::int64_t> exit;
    ~Impl() { if (gate >= 0) close(gate); }
    bool Check(siginfo_t& info) {
        info = {};
        int result;
        do { result = waitid(P_PID, static_cast<id_t>(child), &info, WEXITED | WNOHANG | WNOWAIT); }
        while (result < 0 && errno == EINTR);
        if (result == 0) return true;
        // Never signal this numeric PID again once the child relation is lost.
        child = 0;
        throw Error("runner.process_identity_lost");
    }
};
Process::Process(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
std::unique_ptr<Process> Process::Create(const Json& spec, const fs::path& dir,
                                        const std::vector<std::string>& environment) {
    std::vector<std::string> args = spec["argv"].get<std::vector<std::string>>();
    std::vector<char*> argv, envp;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    for (const auto& entry : environment) envp.push_back(const_cast<char*>(entry.c_str()));
    envp.push_back(nullptr);
    const auto cwd = platform::Utf8ToPath(spec["cwd"].get<std::string>());
    const int output = open((dir / "stdout.log").c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    const int error = open((dir / "stderr.log").c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    const int input = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int gate[2] = {-1, -1};
    if (output < 0 || error < 0 || input < 0 || pipe(gate) != 0) {
        if (output >= 0) close(output);
        if (error >= 0) close(error);
        if (input >= 0) close(input);
        throw Error("runner.launch_failed");
    }
    fcntl(gate[0], F_SETFD, FD_CLOEXEC);
    fcntl(gate[1], F_SETFD, FD_CLOEXEC);
    const pid_t child = fork();
    if (child == 0) {
        close(gate[1]);
        if (setpgid(0, 0) != 0 || chdir(cwd.c_str()) != 0 || dup2(input, STDIN_FILENO) < 0 ||
            dup2(output, STDOUT_FILENO) < 0 || dup2(error, STDERR_FILENO) < 0) _exit(126);
        close(input); close(output); close(error);
        char release = 0;
        ssize_t count;
        do { count = read(gate[0], &release, 1); } while (count < 0 && errno == EINTR);
        close(gate[0]);
        if (count != 1 || release != 'g') _exit(126);
        execve(argv[0], argv.data(), envp.data());
        _exit(127);
    }
    close(gate[0]); close(input); close(output); close(error);
    if (child < 0) { close(gate[1]); throw Error("runner.launch_failed"); }
    if (setpgid(child, child) != 0 && errno != EACCES) {
        close(gate[1]);
        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        throw Error("runner.launch_failed");
    }
    auto impl = std::make_unique<Impl>();
    impl->child = child;
    impl->gate = gate[1];
    return std::unique_ptr<Process>(new Process(std::move(impl)));
}
Process::~Process() {
    if (!impl_ || impl_->child == 0) return;
    try { Cancel(); } catch (...) {}
    if (impl_->child > 0) {
        int status = 0;
        while (waitpid(impl_->child, &status, 0) < 0 && errno == EINTR) {}
        impl_->child = 0;
    }
}
std::uint64_t Process::pid() const { return static_cast<std::uint64_t>(impl_->child); }
void Process::Release() {
    char release = 'g';
    ssize_t count;
    do { count = write(impl_->gate, &release, 1); } while (count < 0 && errno == EINTR);
    close(impl_->gate);
    impl_->gate = -1;
    if (count != 1) throw Error("runner.launch_failed");
}
void Process::Cancel() {
    if (impl_->exit || impl_->child == 0) return;
    siginfo_t info {};
    if (!impl_->Check(info)) return;
    // The unreaped child anchors the group ID, including after leader exit.
    StopOwnedGroup(impl_->child, info.si_pid != 0);
}
std::optional<std::int64_t> Process::Poll() {
    if (impl_->exit) return impl_->exit;
    if (impl_->child == 0) throw Error("runner.process_identity_lost");
    siginfo_t info {};
    if (!impl_->Check(info) || info.si_pid == 0) return std::nullopt;
    // Do not release/reuse the leader PID until its managed group is signalled.
    StopOwnedGroup(impl_->child, true);
    int status = 0;
    pid_t waited;
    do { waited = waitpid(impl_->child, &status, 0); } while (waited < 0 && errno == EINTR);
    impl_->child = 0;
    if (waited <= 0) throw Error("runner.process_identity_lost");
    impl_->exit = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return impl_->exit;
}
}  // namespace lubancode::job_runner
