// probe.hpp 的实现。语义对齐 scripts/updater.py 的 probe_exe(文件头注释
// 见 probe.hpp)。
#include "updater/probe.hpp"

#include <string>
#include <system_error>
#include <vector>

#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "platform/wall_clock.hpp"

namespace lubancode::updater {

namespace {

// python tempfile.mkdtemp(prefix="lubancode-health-")的等价件:临时目录下
// 唯一名(进程号 + 稳定钟搅开同进程并发探针)。
std::filesystem::path MakeProbeHome() {
    const std::filesystem::path home = std::filesystem::temp_directory_path() /
                                       ("lubancode-health-" +
                                        std::to_string(lubancode::platform::CurrentProcessId()) + "-" +
                                        std::to_string(lubancode::platform::WallClockNowMs()));
    std::error_code ec;
    std::filesystem::create_directories(home, ec);
    return home;
}

// 首行:strip 后取到第一个换行(python stdout.strip().splitlines()[0])。
std::string FirstLine(const std::string& bytes) {
    std::string_view text(bytes);
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' ||
                             text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ||
                             text.back() == '\n')) {
        text.remove_suffix(1);
    }
    const std::size_t newline = text.find('\n');
    if (newline != std::string_view::npos) text = text.substr(0, newline);
    return std::string(text);
}

}  // namespace

ProbeOutcome ProbeExe(const std::filesystem::path& exe_path,
                      std::optional<std::string_view> expected_version, int timeout_secs) {
    const std::filesystem::path exe = std::filesystem::absolute(exe_path).lexically_normal();
    const std::filesystem::path probe_home = MakeProbeHome();

    const std::vector<std::string> argv = {lubancode::platform::PathToUtf8(exe), "--version"};
    const platform::EnvPairs env = {
        {"LUBANCODE_HOME", lubancode::platform::PathToUtf8(probe_home)},
        {"LUBANCODE_DATA_HOME", lubancode::platform::PathToUtf8(probe_home / "data")},
        {"LUBANCODE_LANG", "en"},
    };
    const platform::ProcessResult result =
        platform::RunProcessWithStdin(argv, std::string(), timeout_secs * 1000, env);

    std::error_code ec;
    std::filesystem::remove_all(probe_home, ec);  // 用后即清(python rmtree 同款,失败不较真)

    if (result.spawn_failed || result.timed_out) {
        const std::string why = result.timed_out ? ("超时 " + std::to_string(timeout_secs) + " 秒")
                                                 : result.spawn_error;
        return ProbeOutcome{false, "探针跑不起来(" + why + ")"};
    }
    const std::string first = FirstLine(result.stdout_bytes);
    if (result.exit_code != 0 || first.rfind("lubancode ", 0) != 0) {
        return ProbeOutcome{false, "探针输出不合: rc=" + std::to_string(result.exit_code) +
                                       " stdout='" + first + "'"};
    }
    if (expected_version.has_value() && first != "lubancode " + std::string(*expected_version)) {
        return ProbeOutcome{false, "探针版本不合:期望 'lubancode " + std::string(*expected_version) +
                                       "',得 '" + first + "'"};
    }
    return ProbeOutcome{true, first};
}

}  // namespace lubancode::updater
