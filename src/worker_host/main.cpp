#include "host.hpp"

#include <cstdio>
#include <iostream>
#include <string>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif

int main(int argc, char**) {
    if (argc != 1) {
        std::cerr << "luban-worker accepts private supervisor JSONL on stdin only\n";
        return 2;
    }
    // Reserve the actual parent pipe before suppressing native SDK diagnostics.
    // Tool/provider libraries must not interleave output or credential-bearing
    // error bodies with JSONL, or copy those bodies into supervisor logs.
#ifdef _WIN32
    FILE* protocol = _fdopen(_dup(_fileno(stdout)), "wb");
    const char* null_device = "NUL";
#else
    std::signal(SIGPIPE, SIG_IGN);
    FILE* protocol = fdopen(dup(fileno(stdout)), "wb");
    const char* null_device = "/dev/null";
#endif
    if (!protocol) return 2;
#ifdef _WIN32
    const bool private_pipe = SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(protocol))),
                                                  HANDLE_FLAG_INHERIT, 0) != 0;
#else
    const bool private_pipe = fcntl(fileno(protocol), F_SETFD, FD_CLOEXEC) != -1;
#endif
    if (!private_pipe) { std::fclose(protocol); return 2; }
    if (!std::freopen(null_device, "w", stdout) || !std::freopen(null_device, "w", stderr)) {
        std::fclose(protocol);
        return 2;
    }
    lubancode::worker_host::Host host;
    constexpr std::size_t limit = 1024 * 1024;
    int exit_code = 0;
    while (!host.stopping()) {
        std::string line;
        bool oversized = false;
        char ch = 0;
        while (std::cin.get(ch) && ch != '\n') {
            if (line.size() < limit) line.push_back(ch);
            else oversized = true;
        }
        if (!std::cin && line.empty() && !oversized) break;
        nlohmann::json response;
        if (!std::cin) response = {{"id", nullptr}, {"error", {{"code", "worker.incomplete_frame"}}}};
        else if (oversized) response = {{"id", nullptr}, {"error", {{"code", "worker.frame_too_large"}}}};
        else {
            auto request = nlohmann::json::parse(line, nullptr, false);
            response = request.is_discarded() ? nlohmann::json{{"id", nullptr}, {"error", {{"code", "worker.invalid_json"}}}}
                                             : host.Handle(request);
        }
        auto bytes = response.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
        if (host.stopping() && response.contains("error")) exit_code = 1;
        if (std::fwrite(bytes.data(), 1, bytes.size(), protocol) != bytes.size() || std::fflush(protocol) != 0) break;
    }
    if (!host.stopping()) {
        auto closed = host.Handle({{"id", "parent-close"}, {"method", "worker.shutdown"}});
        if (closed.contains("error")) exit_code = 1;
    }
    std::fclose(protocol);
    return exit_code;
}
