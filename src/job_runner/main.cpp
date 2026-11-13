#include "job_runner/client.hpp"
#include "job_runner/common.hpp"
#include "job_runner/server.hpp"
#include "platform/paths.hpp"
#include <csignal>
#include <iostream>
#include <vector>

namespace {
volatile std::sig_atomic_t stop_requested = 0;
void Stop(int) { stop_requested = 1; }
int Main(const std::vector<std::string>& args) {
    using namespace lubancode::job_runner;
    try {
        if (args.size() < 2) throw Error("runner.usage");
        std::string root, runner_id;
        std::size_t max_running = 16, max_records = 1024;
        for (std::size_t i = 2; i < args.size(); ++i) {
            if (i + 1 == args.size()) throw Error("runner.usage");
            const auto value = args[++i];
            if (args[i - 1] == "--state-root") root = value;
            else if (args[i - 1] == "--runner-id") runner_id = value;
            else if (args[1] == "serve" && args[i - 1] == "--max-running") max_running = std::stoul(value);
            else if (args[1] == "serve" && args[i - 1] == "--max-records") max_records = std::stoul(value);
            else throw Error("runner.usage");
        }
        if (root.empty() || max_running == 0 || max_running > 1024 || max_records == 0 || max_records > 10000) {
            throw Error("runner.usage");
        }
        const auto state_root = lubancode::platform::Utf8ToPath(root);
        if (args[1] == "serve") {
            std::signal(SIGINT, Stop);
            std::signal(SIGTERM, Stop);
#ifndef _WIN32
            std::signal(SIGPIPE, SIG_IGN);
            std::signal(SIGCHLD, SIG_DFL);
#endif
            return Serve(state_root, max_running, max_records, stop_requested);
        }
        Json body;
        if (args[1] == "info") {
            VerifyPrivateDirectory(state_root);
            runner_id = ReadJson(state_root / "identity.json").at("runner_id").get<std::string>();
            body = Json{{"method", "runner.info"}};
        } else if (args[1] == "request") {
            if (!HexId(runner_id)) throw Error("runner.identity_required");
            std::string line;
            char c;
            while (std::cin.get(c) && c != '\n') {
                if (line.size() >= kMaxRequestBytes / 2) throw Error("runner.request_too_large");
                line += c;
            }
            body = Json::parse(line, nullptr, false);
            if (body.is_discarded()) throw Error("runner.invalid_request");
        } else throw Error("runner.usage");
        const auto reply = Request(state_root, runner_id, body);
        std::cout << reply.dump() << std::endl;
        return reply.value("ok", false) ? 0 : 1;
    } catch (const Error& error) {
        std::cout << lubancode::job_runner::Failure(error.what()).dump() << std::endl;
        return 1;
    } catch (...) {
        std::cout << lubancode::job_runner::Failure("runner.invalid_request").dump() << std::endl;
        return 1;
    }
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(lubancode::platform::WideToUtf8(argv[i]));
    return Main(args);
}
#else
int main(int argc, char** argv) { return Main(std::vector<std::string>(argv, argv + argc)); }
#endif
