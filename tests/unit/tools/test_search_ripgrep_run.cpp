// ripgrep 迁移单 P0-4/P0-5 的流式 runner 测试,三段:
//   1) 纯函数段:base64/路径规整/UTF-8 截断/分帧器/JSONL 事件解析/流式帽
//      钳制——不起进程,处处可跑;
//   2) 假 rg 段:tests/support/fake_ripgrep.cpp 按 @场景名 伪造输出,经
//      exe_override 起,覆盖真 rg 不肯配合的路径(坏 JSON、大帧、拒退、
//      stderr 洪水、尾帧无换行、满额主动收树、取消/超时收树、再生孩子
//      不留孤儿、回调与 Shutdown 并发);
//   3) 真 rg 段:LUBANCODE_BUNDLED_RG_DIR 分期入位后启 用(CTest 运行目录
//      libexec/ 里有 rg),走生产同一条定位路——没设则整段如实跳过
//      (设计单 4.3:不为跑测试开环境变量的洞)。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "platform/process.hpp"  // IsProcessAlive:取消收树不留孤儿/残留的证据
#include "tools/observation_filter.hpp"  // ObservationBoundary:登记目录剪枝的差分
#include "tools/path_utils.hpp"
#include "tools/search.hpp"
#include "tools/search_ripgrep.hpp"

using lubancode::tools::BundledRipgrepRunner;
using lubancode::tools::DecodeBase64;
using lubancode::tools::IRipgrepRunner;
using lubancode::tools::kBundledRipgrepVersion;
using lubancode::tools::MakeRipgrepStreamLimits;
using lubancode::tools::NormalizeRipgrepPath;
using lubancode::tools::ParsedGrepEvent;
using lubancode::tools::ParseGrepEventLine;
using lubancode::tools::PathToUtf8;
using lubancode::tools::RipgrepInvocation;
using lubancode::tools::RipgrepRunResult;
using lubancode::tools::RipgrepSmokeStatus;
using lubancode::tools::RipgrepStreamFramer;
using lubancode::tools::RipgrepStreamLimits;
using lubancode::tools::RipgrepVersionProbe;
using lubancode::tools::SearchBackendError;
using lubancode::tools::SearchBackendErrorInfo;
using lubancode::tools::SearchMode;
using lubancode::tools::SearchPolicy;
using lubancode::tools::SearchRequest;
using lubancode::tools::SearchTool;
using lubancode::tools::Tool;
using lubancode::tools::ToolExecutionContext;
using lubancode::tools::ToString;
using lubancode::tools::TruncateUtf8Boundary;
using lubancode::tools::Utf8ToPath;

namespace {

class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("lubancode_rgrun_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& Path() const { return path_; }

    std::string Utf8Path(const std::string& child = "") const {
        return PathToUtf8(child.empty() ? path_ : path_ / Utf8ToPath(child));
    }

    void WriteFile(const std::string& child, const std::string& content) const {
        const std::filesystem::path full = path_ / Utf8ToPath(child);
        std::filesystem::create_directories(full.parent_path());
        std::ofstream file(full, std::ios::binary);
        file << content;
    }

private:
    std::filesystem::path path_;
};

// 环境变量 RAII(fake rg 的 marker 出入口)。
class EnvGuard {
public:
    EnvGuard(const char* name, const std::string& value) : name_(name) {
        const char* old = std::getenv(name);
        had_old_ = old != nullptr;
        old_value_ = old != nullptr ? std::string(old) : std::string();
#ifdef _WIN32
        _putenv_s(name_, value.c_str());
#else
        setenv(name_, value.c_str(), /*overwrite=*/1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        if (had_old_) {
            _putenv_s(name_, old_value_.c_str());
        } else {
            _putenv_s(name_, "");
        }
#else
        if (had_old_) {
            setenv(name_, old_value_.c_str(), 1);
        } else {
            unsetenv(name_);
        }
#endif
    }

private:
    const char* name_;
    bool had_old_ = false;
    std::string old_value_;
};

// 报"版本正确"的假探针:不起进程,前置校验直接过。
RipgrepVersionProbe FakeReadyProbe() {
    return [](const std::filesystem::path&) {
        return std::expected<std::string, SearchBackendErrorInfo>(
            std::string("ripgrep ") + std::string(kBundledRipgrepVersion) + " (rev test)\n");
    };
}

SearchRequest GrepRequest(std::string pattern, std::filesystem::path root) {
    SearchRequest request;
    request.mode = SearchMode::Grep;
    request.pattern = std::move(pattern);
    request.root = std::move(root);
    return request;
}

SearchRequest GlobRequest(std::string pattern, std::filesystem::path root) {
    SearchRequest request;
    request.mode = SearchMode::Glob;
    request.pattern = std::move(pattern);
    request.root = std::move(root);
    return request;
}

#ifdef LUBANCODE_FAKE_RG_EXE
const std::filesystem::path kFakeRgExe = Utf8ToPath(LUBANCODE_FAKE_RG_EXE);

// 起假 rg 的 runner:假探针过版本关,limits 默认即合同四道墙(个别用例
// 注入小帽提速)。
BundledRipgrepRunner MakeFakeRunner(RipgrepStreamLimits limits = {}) {
    return BundledRipgrepRunner(kFakeRgExe, FakeReadyProbe(), limits);
}

std::optional<std::string> ReadTextFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return content;
}

// 轮询等 marker 文件出现(再生孩子把 PID 写进去要几毫秒)。
std::optional<std::string> WaitForMarker(const std::filesystem::path& path, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::optional<std::string> content = ReadTextFile(path)) {
            return content;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return std::nullopt;
}

// 轮询等 PID 死透(收树后系统收尸有一拍延迟,等一等再断言,不闪断)。
bool WaitUntilDead(unsigned long pid, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!lubancode::platform::IsProcessAlive(pid)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return !lubancode::platform::IsProcessAlive(pid);
}
#endif  // LUBANCODE_FAKE_RG_EXE

}  // namespace

// ---------------------------------------------------------------------------
// 1) 纯函数:base64 / 路径规整 / UTF-8 截断
// ---------------------------------------------------------------------------

TEST_CASE("base64 解码: 标准字母表、填充、非法输入") {
    CHECK(DecodeBase64("") == "");
    CHECK(DecodeBase64("YQ==") == "a");
    CHECK(DecodeBase64("YWI=") == "ab");
    CHECK(DecodeBase64("YWJj") == "abc");
    // Y2Fm6SBuZWVkbGUK = "caf\xe9 needle\n"(rg --json 实测样例)
    CHECK(DecodeBase64("Y2Fm6SBuZWVkbGUK") == std::string("caf\xe9 needle\n"));
    CHECK(DecodeBase64("aGVsbG8gd29ybGQh") == "hello world!");
    // 非法字符/填充后又冒字符/落单字符:空串,不塞半截乱码。
    CHECK(DecodeBase64("!!!!").empty());
    CHECK(DecodeBase64("YQ==ZQ==").empty());
    CHECK(DecodeBase64("Y").empty());
}

TEST_CASE("rg 输出路径规整: 反斜杠统一、剥 ./ 前缀") {
    CHECK(NormalizeRipgrepPath("./sub/b.cpp") == "sub/b.cpp");
    CHECK(NormalizeRipgrepPath(".\\sub\\b.cpp") == "sub/b.cpp");  // Windows 实测形状
    CHECK(NormalizeRipgrepPath("a.txt") == "a.txt");              // 单文件 root 无前缀
    CHECK(NormalizeRipgrepPath("./deep\\c.md") == "deep/c.md");
}

TEST_CASE("UTF-8 边界截断: 不切半个多字节字符") {
    CHECK(TruncateUtf8Boundary("abc", 10) == "abc");   // 不用截
    CHECK(TruncateUtf8Boundary("abcdef", 3) == "abc");
    CHECK(TruncateUtf8Boundary("", 5).empty());
    // "关键词" 3 个汉字 9 字节;截 4 字节会落在汉字中间,须回收到 3。
    CHECK(TruncateUtf8Boundary("关键词", 4) == "\xe5\x85\xb3");
    CHECK(TruncateUtf8Boundary("关键词xyz", 12) == "关键词xyz");
    CHECK(TruncateUtf8Boundary("关键词xyz", 10) == "关键词x");
}

TEST_CASE("流式帽: max_results 只降不升,缺省走 100 硬帽") {
    CHECK(MakeRipgrepStreamLimits(0).max_hits == 100);     // 缺省
    CHECK(MakeRipgrepStreamLimits(1).max_hits == 1);
    CHECK(MakeRipgrepStreamLimits(50).max_hits == 50);
    CHECK(MakeRipgrepStreamLimits(100).max_hits == 100);
    CHECK(MakeRipgrepStreamLimits(9999).max_hits == 100);  // 调高不许
    // 四道墙的合同值顺带钉一遍。
    const RipgrepStreamLimits defaults;
    CHECK(defaults.max_total_result_bytes == 512 * 1024);
    CHECK(defaults.max_hit_line_bytes == 16 * 1024);
    CHECK(defaults.max_frame_bytes == 1024 * 1024);
    CHECK(defaults.max_stderr_bytes == 64 * 1024);
    CHECK(defaults.timeout_ms == 120'000);
}

// ---------------------------------------------------------------------------
// 1) 纯函数:分帧器
// ---------------------------------------------------------------------------

TEST_CASE("分帧器: JSONL 跨块切分、尾帧冲刷、帧回调停读") {
    std::vector<std::string> frames;
    const auto collect = [&frames](std::string_view frame) {
        frames.emplace_back(frame);
        return true;
    };
    RipgrepStreamFramer framer('\n', 1024 * 1024);
    CHECK(framer.Feed("{\"a\":1}", collect));   // 无换行:进缓冲
    CHECK(framer.Feed("\n{\"a\":2}\n", collect));
    REQUIRE(frames.size() == 2);
    CHECK(frames[0] == "{\"a\":1}");
    CHECK(frames[1] == "{\"a\":2}");
    CHECK(framer.pending_bytes() == 0);

    frames.clear();
    CHECK(framer.Feed("tail-without-newline", collect));
    REQUIRE(framer.FlushTail(collect));         // 尾帧无换行也要处理
    REQUIRE(frames.size() == 1);
    CHECK(frames[0] == "tail-without-newline");

    // 帧回调返回 false -> Feed 透传 false(满额停读信号)。
    RipgrepStreamFramer stop_framer('\n', 1024);
    const bool keep = stop_framer.Feed("x\ny\n", [](std::string_view) { return false; });
    CHECK_FALSE(keep);
}

TEST_CASE("分帧器: NUL 分帧(POSIX 文件名可含换行)与超帽") {
    std::vector<std::string> frames;
    const auto collect = [&frames](std::string_view frame) {
        frames.emplace_back(frame);
        return true;
    };
    RipgrepStreamFramer framer('\0', 1024);
    // 内嵌 NUL 的入参必须带显式长度:const char* 构造 string_view 会在第一个
    // NUL 处截断,那正是这个用例要防的坑。
    CHECK(framer.Feed(std::string_view("a.txt\0sub/b", 11), collect));
    CHECK(framer.Feed(std::string_view(".txt\0", 5), collect));
    REQUIRE(frames.size() == 2);
    CHECK(frames[0] == "a.txt");
    CHECK(frames[1] == "sub/b.txt");

    // 未完成帧超帽:Feed 返回 false(协议错),回调不再收帧。
    RipgrepStreamFramer capped('\0', 8);
    CHECK(capped.Feed(std::string_view("short\0", 6), collect));
    const bool over = capped.Feed("1234567890", collect);  // 无 NUL 闭帧,缓冲越 8 字节帽
    CHECK_FALSE(over);
}

// ---------------------------------------------------------------------------
// 1) 纯函数:JSONL 事件解析(text/bytes 两路、多 submatch 一事件、坏 JSON)
// ---------------------------------------------------------------------------

TEST_CASE("事件解析: match 的 text 路(Windows 反斜杠路径、尾换行剥除)") {
    const ParsedGrepEvent event = ParseGrepEventLine(
        "{\"type\":\"match\",\"data\":{\"path\":{\"text\":\".\\\\sub\\\\b.cpp\"},"
        "\"lines\":{\"text\":\"needle two\\n\"},\"line_number\":1,"
        "\"submatches\":[{\"match\":{\"text\":\"needle\"},\"start\":0,\"end\":6}]}}");
    REQUIRE(event.kind == ParsedGrepEvent::Kind::Match);
    CHECK(event.hit.path == "sub/b.cpp");
    CHECK(event.hit.text == "needle two");  // 尾 \n 剥掉
    CHECK(event.hit.line_number == 1);
}

TEST_CASE("事件解析: match 的 bytes 路(base64,非 UTF-8 正文清洗)") {
    // lines.bytes 是 "caf\xe9 needle\n" 的 base64;解码后含非法 UTF-8 字节,
    // 须过 SanitizeExternalText(0xe9 单独一字节非法,洗成替换符)。
    const ParsedGrepEvent event = ParseGrepEventLine(
        "{\"type\":\"match\",\"data\":{\"path\":{\"text\":\"latin1.txt\"},"
        "\"lines\":{\"bytes\":\"Y2Fm6SBuZWVkbGUK\"},\"line_number\":1,"
        "\"submatches\":[]}}");
    REQUIRE(event.kind == ParsedGrepEvent::Kind::Match);
    CHECK(event.hit.path == "latin1.txt");
    CHECK(event.hit.text.find("needle") != std::string::npos);
    CHECK(event.hit.text.find('\xe9') == std::string::npos);  // 非法字节不直塞
}

TEST_CASE("事件解析: 同一行多个 submatch 仍是一条命中(按命中行计)") {
    const ParsedGrepEvent event = ParseGrepEventLine(
        "{\"type\":\"match\",\"data\":{\"path\":{\"text\":\"m.txt\"},"
        "\"lines\":{\"text\":\"needle x needle y\\n\"},\"line_number\":1,"
        "\"submatches\":[{\"match\":{\"text\":\"needle\"},\"start\":0,\"end\":6},"
        "{\"match\":{\"text\":\"needle\"},\"start\":9,\"end\":15}]}}");
    REQUIRE(event.kind == ParsedGrepEvent::Kind::Match);
    CHECK(event.hit.text == "needle x needle y");
}

TEST_CASE("事件解析: summary 抽诊断字段,begin/end 忽略") {
    const ParsedGrepEvent summary = ParseGrepEventLine(
        "{\"data\":{\"elapsed_total\":{\"human\":\"0.006s\",\"nanos\":6179100,\"secs\":0},"
        "\"stats\":{\"bytes_printed\":480,\"bytes_searched\":28,\"matched_lines\":2,\"matches\":2,"
        "\"searches\":2,\"searches_with_match\":2}},\"type\":\"summary\"}");
    REQUIRE(summary.kind == ParsedGrepEvent::Kind::Summary);
    CHECK(summary.stats.matched_lines == 2);
    CHECK(summary.stats.searches == 2);
    CHECK(summary.stats.searches_with_match == 2);
    CHECK(summary.stats.elapsed_total_secs == doctest::Approx(0.0061791));

    const ParsedGrepEvent begin = ParseGrepEventLine(
        "{\"type\":\"begin\",\"data\":{\"path\":{\"text\":\".\\\\a.cpp\"}}}");
    CHECK(begin.kind == ParsedGrepEvent::Kind::Other);
    const ParsedGrepEvent end = ParseGrepEventLine(
        "{\"type\":\"end\",\"data\":{\"path\":{\"text\":\".\\\\a.cpp\"},\"binary_offset\":null}}");
    CHECK(end.kind == ParsedGrepEvent::Kind::Other);
}

TEST_CASE("事件解析: 坏 JSON / 缺字段不吞掉装作无命中") {
    CHECK(ParseGrepEventLine("this is not json").kind == ParsedGrepEvent::Kind::Invalid);
    CHECK(ParseGrepEventLine("{").kind == ParsedGrepEvent::Kind::Invalid);
    CHECK(ParseGrepEventLine("[]").kind == ParsedGrepEvent::Kind::Invalid);      // 不是对象
    CHECK(ParseGrepEventLine("{\"data\":{}}").kind == ParsedGrepEvent::Kind::Invalid);  // 无 type
    // match 缺 path/lines:形状不合。
    CHECK(ParseGrepEventLine("{\"type\":\"match\",\"data\":{\"line_number\":3}}").kind ==
          ParsedGrepEvent::Kind::Invalid);
}

// ---------------------------------------------------------------------------
// 2) 假 rg:基本终态(exit 0/1/2、半途死)
// ---------------------------------------------------------------------------

#ifdef LUBANCODE_FAKE_RG_EXE

TEST_CASE("runner 假 rg: jsonl-basic 两条命中,路径规整,exit 0") {
    const TempDir dir;
    BundledRipgrepRunner runner = MakeFakeRunner();
    const auto result = runner.Run(GrepRequest("@jsonl-basic", dir.Path()), SearchPolicy{},
                                   ToolExecutionContext{});
    REQUIRE(result.has_value());
    CHECK_FALSE(result->truncated);
    CHECK(result->exit_code == 0);
    REQUIRE(result->hits.size() == 2);
    CHECK(result->hits[0].path == "a.txt");
    CHECK(result->hits[0].line_number == 2);
    CHECK(result->hits[0].text == "needle one");
    CHECK(result->hits[1].line_number == 4);
}

TEST_CASE("runner 假 rg: 无命中 exit 1 走成功") {
    const TempDir dir;
    BundledRipgrepRunner runner = MakeFakeRunner();
    const auto result = runner.Run(GrepRequest("@no-match", dir.Path()), SearchPolicy{},
                                   ToolExecutionContext{});
    REQUIRE(result.has_value());
    CHECK(result->hits.empty());
    CHECK(result->exit_code == 1);
    CHECK_FALSE(result->truncated);
}

TEST_CASE("runner 假 rg: invalid regex exit 2 走稳定错误 search_pattern_invalid") {
    const TempDir dir;
    BundledRipgrepRunner runner = MakeFakeRunner();
    const auto result = runner.Run(GrepRequest("@invalid-regex", dir.Path()), SearchPolicy{},
                                   ToolExecutionContext{});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == SearchBackendError::PatternInvalid);
    CHECK(ToString(result.error().code) == "search_pattern_invalid");
    // stderr 洗成首行短句:有"regex parse error"字样,不带整段多行错误。
    CHECK(result.error().message.find("regex parse error") != std::string::npos);
    CHECK(result.error().message.find("unclosed group") == std::string::npos);
}

TEST_CASE("runner 假 rg: 进程半途死(exit 7)不冒充没搜到") {
    const TempDir dir;
    BundledRipgrepRunner runner = MakeFakeRunner();
    const auto result = runner.Run(GrepRequest("@exit7", dir.Path()), SearchPolicy{},
                                   ToolExecutionContext{});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == SearchBackendError::RunFailed);
    CHECK(ToString(result.error().code) == "search_backend_run_failed");
    CHECK(result.error().message.find("7") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 2) 假 rg:协议错(坏 JSON / 1 MiB 大帧)
// ---------------------------------------------------------------------------

TEST_CASE("runner 假 rg: 坏 JSON 按协议错收树") {
    const TempDir dir;
    BundledRipgrepRunner runner = MakeFakeRunner();
    const auto result = runner.Run(GrepRequest("@bad-json", dir.Path()), SearchPolicy{},
                                   ToolExecutionContext{});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == SearchBackendError::ProtocolError);
    CHECK(ToString(result.error().code) == "search_backend_protocol_error");
    CHECK(result.error().message.find("this is not json") != std::string::npos);
}

TEST_CASE("runner 假 rg: 2 MiB 未完成帧撞 1 MiB 协议墙") {
    const TempDir dir;
    BundledRipgrepRunner runner = MakeFakeRunner();  // 默认帽:1 MiB 协议帧
    const auto t0 = std::chrono::steady_clock::now();
    const auto result = runner.Run(GrepRequest("@big-frame", dir.Path()), SearchPolicy{},
                                   ToolExecutionContext{});
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == SearchBackendError::ProtocolError);
    // 撞墙即收树:不会把 2 MiB 全喝完(灌完 32 块 + 睡等的量级明显更大)。
    CHECK(elapsed < 5'000);
}

// ---------------------------------------------------------------------------
// 2) 假 rg:四道墙的条数帽与总量帽
// ---------------------------------------------------------------------------

TEST_CASE("runner 假 rg: 满 100 条主动停树,marker 证明没跑完全程") {
    const TempDir dir;
    const std::filesystem::path marker = dir.Path() / Utf8ToPath("done.marker");
    {
        EnvGuard guard("LUBANCODE_FAKE_RG_MARKER", PathToUtf8(marker));
        BundledRipgrepRunner runner = MakeFakeRunner();
        const auto result = runner.Run(GrepRequest("@many-hits:250", dir.Path()), SearchPolicy{},
                                       ToolExecutionContext{});
        REQUIRE(result.has_value());
        CHECK(result->hits.size() == 100);  // 全局 100 条,不是 rg 的每文件 100
        CHECK(result->truncated);
        // marker 是假 rg 吐完 250 条再睡 30 秒(默认,可经
        // LUBANCODE_FAKE_RG_MARKER_DELAY_MS 调)后才写的:满额收树把它截在
        // 半路,marker 不存在 = 真的提前停了,没有遍历余下 150 条。窗宽只
        // 是收树余量——慢机上收树慢半拍也掐在 marker 之前(CI run
        // 33944110255 的冤红即 300ms 窗掐不完所致)。
        CHECK_FALSE(std::filesystem::exists(marker));
    }
}

TEST_CASE("runner 假 rg: max_results 软请求只降不升") {
    const TempDir dir;
    const std::filesystem::path marker = dir.Path() / Utf8ToPath("done.marker");
    {
        EnvGuard guard("LUBANCODE_FAKE_RG_MARKER", PathToUtf8(marker));
        BundledRipgrepRunner runner = MakeFakeRunner();
        SearchRequest request = GrepRequest("@many-hits:250", dir.Path());
        request.max_results = 10;
        const auto result = runner.Run(request, SearchPolicy{}, ToolExecutionContext{});
        REQUIRE(result.has_value());
        CHECK(result->hits.size() == 10);
        CHECK(result->truncated);
        CHECK_FALSE(std::filesystem::exists(marker));
    }
    {
        // 调高不许:9999 仍按 100 帽截。
        EnvGuard guard("LUBANCODE_FAKE_RG_MARKER", PathToUtf8(marker));
        BundledRipgrepRunner runner = MakeFakeRunner();
        SearchRequest request = GrepRequest("@many-hits:250", dir.Path());
        request.max_results = 9999;
        const auto result = runner.Run(request, SearchPolicy{}, ToolExecutionContext{});
        REQUIRE(result.has_value());
        CHECK(result->hits.size() == 100);
    }
}

TEST_CASE("runner 假 rg: 512 KiB 总量墙(合同默认值)与 16 KiB 单行截断") {
    const TempDir dir;
    BundledRipgrepRunner runner = MakeFakeRunner();
    // 40 条 × 2 万字符:每条先被 16 KiB 单行墙截到 16384,累计到 512 KiB
    // (16384+路径 ≈ 16392/条)时第 32 条触发总量墙主动收树。
    const auto result = runner.Run(GrepRequest("@wide-lines:40", dir.Path()), SearchPolicy{},
                                   ToolExecutionContext{});
    REQUIRE(result.has_value());
    CHECK(result->hits.size() == 32);
    CHECK(result->truncated);
    for (const auto& hit : result->hits) {
        CHECK(hit.text.size() == 16 * 1024);  // 截断不切半个字符,恰 16384
    }
}

TEST_CASE("runner 假 rg: glob 模式 NUL 分帧、路径规整、满额停树") {
    const TempDir dir;
    BundledRipgrepRunner runner = MakeFakeRunner();
    const auto result = runner.Run(GlobRequest("@glob-files", dir.Path()), SearchPolicy{},
                                   ToolExecutionContext{});
    REQUIRE(result.has_value());
    REQUIRE(result->hits.size() == 3);
    CHECK(result->hits[0].path == "a.txt");
    CHECK(result->hits[1].path == "sub/b.txt");
    CHECK(result->hits[2].path == "deep/c.md");
    CHECK(result->hits[0].line_number == 0);  // glob 无行号

    const std::filesystem::path marker = dir.Path() / Utf8ToPath("done.marker");
    {
        EnvGuard guard("LUBANCODE_FAKE_RG_MARKER", PathToUtf8(marker));
        const auto many = runner.Run(GlobRequest("@glob-many:150", dir.Path()), SearchPolicy{},
                                     ToolExecutionContext{});
        REQUIRE(many.has_value());
        CHECK(many->hits.size() == 100);  // glob 按文件计
        CHECK(many->truncated);
        CHECK_FALSE(std::filesystem::exists(marker));
    }
}

// ---------------------------------------------------------------------------
// 2) 假 rg:stderr 帽、尾帧、取消/超时/孤儿/并发
// ---------------------------------------------------------------------------

TEST_CASE("runner 假 rg: stderr 100 KiB 灌到 64 KiB 截断并注明") {
    const TempDir dir;
    BundledRipgrepRunner runner = MakeFakeRunner();
    const auto result = runner.Run(GrepRequest("@stderr-flood", dir.Path()), SearchPolicy{},
                                   ToolExecutionContext{});
    REQUIRE(result.has_value());
    CHECK(result->stderr_text.size() <= 64 * 1024 + 64);  // 帽 + 截断注明一行
    CHECK(result->stderr_text.find("截断") != std::string::npos);
}

TEST_CASE("runner 假 rg: 尾帧无换行也要处理") {
    const TempDir dir;
    BundledRipgrepRunner runner = MakeFakeRunner();
    const auto result = runner.Run(GrepRequest("@tail-no-newline", dir.Path()), SearchPolicy{},
                                   ToolExecutionContext{});
    REQUIRE(result.has_value());
    REQUIRE(result->hits.size() == 1);
    CHECK(result->hits[0].path == "tail.txt");
    CHECK(result->hits[0].text == "tail without newline");
}

TEST_CASE("runner 假 rg: 起跑前取消不起进程") {
    const TempDir dir;
    int probe_calls = 0;
    RipgrepVersionProbe probe = [&probe_calls](const std::filesystem::path&) {
        ++probe_calls;
        return std::expected<std::string, SearchBackendErrorInfo>(
            std::string("ripgrep ") + std::string(kBundledRipgrepVersion) + "\n");
    };
    BundledRipgrepRunner runner(kFakeRgExe, probe);
    std::atomic<bool> cancel{true};
    ToolExecutionContext context;
    context.cancel = &cancel;
    const auto result = runner.Run(GrepRequest("@never-exit", dir.Path()), SearchPolicy{}, context);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == SearchBackendError::Cancelled);
    CHECK(probe_calls == 0);  // 起跑前的取消连 smoke 探针都不跑
}

TEST_CASE("runner 假 rg: 运行中取消收树,进程表无残留") {
    const TempDir dir;
    const std::filesystem::path marker = dir.Path() / Utf8ToPath("self.pid");
    {
        EnvGuard guard("LUBANCODE_FAKE_RG_MARKER", PathToUtf8(marker));
        BundledRipgrepRunner runner = MakeFakeRunner();
        std::atomic<bool> cancel{false};
        ToolExecutionContext context;
        context.cancel = &cancel;
        // 取消点给足进程启动余量:假 rg 起来第一件事就是把 PID 写进 marker,
        // 但满负荷的机器上起进程可能要几百毫秒——150ms 的取消点在压力下会
        // 把它掐在 marker 之前,测试就不是在考"取消收树"而是在赌调度了。
        std::thread canceller([&cancel] {
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            cancel.store(true);
        });
        const auto result = runner.Run(GrepRequest("@never-exit", dir.Path()), SearchPolicy{}, context);
        canceller.join();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == SearchBackendError::Cancelled);
        CHECK(ToString(result.error().code) == "search_cancelled");

        // 假 rg 把自己的 PID 写在 marker 里:Run 返回后它必须已死透。
        const std::optional<std::string> pid_text = WaitForMarker(marker, 2'000);
        REQUIRE(pid_text.has_value());
        const unsigned long pid = std::strtoul(pid_text->c_str(), nullptr, 10);
        REQUIRE(pid != 0);
        CHECK(WaitUntilDead(pid, 2'000));
    }
}

TEST_CASE("runner 假 rg: rg 再生孩子,取消收树不留孤儿(Job Object/进程组)") {
    const TempDir dir;
    const std::filesystem::path marker = dir.Path() / Utf8ToPath("grandchild.pid");
    {
        EnvGuard guard("LUBANCODE_FAKE_RG_MARKER", PathToUtf8(marker));
        BundledRipgrepRunner runner = MakeFakeRunner();
        std::atomic<bool> cancel{false};
        ToolExecutionContext context;
        context.cancel = &cancel;
        std::thread canceller([&cancel] {
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
            cancel.store(true);
        });
        const auto result =
            runner.Run(GrepRequest("@spawn-child", dir.Path()), SearchPolicy{}, context);
        canceller.join();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == SearchBackendError::Cancelled);

        // 假 rg 的孩子把 PID 写在 marker 里:整棵树(Windows Job Object 连坐/
        // POSIX killpg 连坐)必须把它一并收掉,不许孤儿。
        const std::optional<std::string> pid_text = WaitForMarker(marker, 2'000);
        REQUIRE(pid_text.has_value());
        const unsigned long pid = std::strtoul(pid_text->c_str(), nullptr, 10);
        REQUIRE(pid != 0);
        CHECK(WaitUntilDead(pid, 3'000));
    }
}

TEST_CASE("runner 假 rg: 墙钟超时各自终态(注入小 timeout)") {
    const TempDir dir;
    RipgrepStreamLimits limits;
    limits.timeout_ms = 400;
    BundledRipgrepRunner runner(kFakeRgExe, FakeReadyProbe(), limits);
    const auto t0 = std::chrono::steady_clock::now();
    const auto result = runner.Run(GrepRequest("@never-exit", dir.Path()), SearchPolicy{},
                                   ToolExecutionContext{});
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == SearchBackendError::Timeout);
    CHECK(ToString(result.error().code) == "search_timeout");
    CHECK(elapsed < 5'000);  // 400ms 帽 + 收树,不吊 30 秒
}

TEST_CASE("runner 假 rg: 满额停读与 Shutdown 收树的并发竞态,高压 1000 轮") {
    const TempDir dir;
    // 每一轮都在第 1 条命中即满额(注入 max_hits=1):stdout 回调返回 false
    // 与主线程 Shutdown 在毫秒级窗口里赛跑——正是设计单 10.5 点名的竞态面。
    RipgrepStreamLimits limits;
    limits.max_hits = 1;
    BundledRipgrepRunner runner(kFakeRgExe, FakeReadyProbe(), limits);
    for (int i = 0; i < 1000; ++i) {
        const auto result = runner.Run(GrepRequest("@jsonl-basic", dir.Path()), SearchPolicy{},
                                       ToolExecutionContext{});
        if (!result.has_value()) {
            const std::string why = "round " + std::to_string(i) + " failed: " + result.error().message;
            REQUIRE_MESSAGE(false, why.c_str());
        }
        REQUIRE(result.has_value());
        CHECK(result->hits.size() == 1);
        CHECK(result->truncated);
    }
}

#endif  // LUBANCODE_FAKE_RG_EXE

// ---------------------------------------------------------------------------
// 3) 真 rg(分期入位后启用):SearchTool 生产路端到端
// ---------------------------------------------------------------------------

#ifdef LUBANCODE_TEST_HAS_BUNDLED_RG

TEST_CASE("真 rg: SearchTool 默认构造端到端(grep/glob/中文/截断)") {
    const TempDir dir;
    dir.WriteFile("a.txt", "line one\nneedle here\n");
    dir.WriteFile("sub/b.cpp", "关键词 in cpp\n");
    dir.WriteFile("skip.bin", std::string("BIN\0ARY", 7));
    std::string many;
    for (int i = 0; i < 150; ++i) {
        many += "hit_line\n";
    }
    dir.WriteFile("many.txt", many);

    SearchTool tool;  // 生产装配:定位走测试运行目录 libexec/(CMake 分期入位)
    const Tool::Result grep = tool.execute(nlohmann::json{
        {"mode", "grep"}, {"pattern", "needle"}, {"path", dir.Utf8Path()}});
    CHECK_FALSE(grep.is_error);
    CHECK(grep.content.find("a.txt:2:needle here") != std::string::npos);

    const Tool::Result chinese = tool.execute(nlohmann::json{
        {"mode", "grep"}, {"pattern", "关键词"}, {"path", dir.Utf8Path()}});
    CHECK_FALSE(chinese.is_error);
    CHECK(chinese.content.find("sub/b.cpp:1:") != std::string::npos);

    // 二进制文件跳过:skip.bin 里的字面量搜不到。
    const Tool::Result binary = tool.execute(nlohmann::json{
        {"mode", "grep"}, {"pattern", "ARY"}, {"path", dir.Utf8Path()}});
    CHECK_FALSE(binary.is_error);
    CHECK(binary.content.find("没搜到匹配的内容") != std::string::npos);

    const Tool::Result truncated = tool.execute(nlohmann::json{
        {"mode", "grep"}, {"pattern", "hit_line"}, {"path", dir.Utf8Path()}});
    CHECK_FALSE(truncated.is_error);
    CHECK(truncated.content.find("截断") != std::string::npos);
    int count = 0;
    std::size_t pos = 0;
    while ((pos = truncated.content.find("hit_line", pos)) != std::string::npos) {
        ++count;
        pos += 8;
    }
    CHECK(count == 100);

    const Tool::Result glob = tool.execute(nlohmann::json{
        {"mode", "glob"}, {"pattern", "*.cpp"}, {"path", dir.Utf8Path()}});
    CHECK_FALSE(glob.is_error);
    CHECK(glob.content.find("sub/b.cpp") != std::string::npos);

    const Tool::Result no_match = tool.execute(nlohmann::json{
        {"mode", "grep"}, {"pattern", "zzz_absent_token"}, {"path", dir.Utf8Path()}});
    CHECK_FALSE(no_match.is_error);
    CHECK(no_match.content.find("没搜到匹配的内容") != std::string::npos);
}

TEST_CASE("真 rg: fixed_strings 按字面量,regex 元字符默认解析") {
    const TempDir dir;
    dir.WriteFile("dot.txt", "a.b\naxb\n");

    SearchTool tool;
    const Tool::Result regex = tool.execute(nlohmann::json{
        {"mode", "grep"}, {"pattern", "a.b"}, {"path", dir.Utf8Path("dot.txt")}});
    CHECK_FALSE(regex.is_error);
    CHECK(regex.content.find("a.b") != std::string::npos);
    CHECK(regex.content.find("axb") != std::string::npos);  // '.' 是元字符

    const Tool::Result fixed = tool.execute(nlohmann::json{
        {"mode", "grep"}, {"pattern", "a.b"}, {"path", dir.Utf8Path("dot.txt")},
        {"fixed_strings", true}});
    CHECK_FALSE(fixed.is_error);
    CHECK(fixed.content.find("a.b") != std::string::npos);
    CHECK(fixed.content.find("axb") == std::string::npos);  // 字面量只中 a.b
}

TEST_CASE("真 rg: Rust regex 语法(backreference/lookahead 报稳定错,\\p{Han} 可用)") {
    const TempDir dir;
    dir.WriteFile("zh.txt", "中文内容 alpha\n");
    dir.WriteFile("rep.txt", "foo foo\n");

    SearchTool tool;
    // 旧内核认、Rust regex 不认的语法:稳定错误,不崩。
    const Tool::Result backref = tool.execute(nlohmann::json{
        {"mode", "grep"}, {"pattern", R"((\w+) \1)"}, {"path", dir.Utf8Path()}});
    CHECK(backref.is_error);
    CHECK(backref.content.find("search_pattern_invalid") != std::string::npos);

    const Tool::Result lookahead = tool.execute(nlohmann::json{
        {"mode", "grep"}, {"pattern", "foo(?= bar)"}, {"path", dir.Utf8Path()}});
    CHECK(lookahead.is_error);
    CHECK(lookahead.content.find("search_pattern_invalid") != std::string::npos);

    // 新增能力:Unicode 属性转义(旧内核做不到,差分表里的"纯增益")。
    const Tool::Result han = tool.execute(nlohmann::json{
        {"mode", "grep"}, {"pattern", R"(\p{Han}+内容)"}, {"path", dir.Utf8Path("zh.txt")}});
    CHECK_FALSE(han.is_error);
    CHECK(han.content.find("zh.txt:1:") != std::string::npos);
}

TEST_CASE("真 rg: ignore 文件默认遵守,显式点名单文件放行") {
    const TempDir dir;
    // rg 的 require-git 语义:不在 git 仓库里时 .gitignore 不生效(.ignore/
    // .rgignore 不受此限)。临时目录不是仓库,放一枚 .git 目录让 rg 认账;
    // 目录内容被宿主硬排除,不影响本测。
    dir.WriteFile(".git/HEAD", "ref: refs/heads/main\n");
    dir.WriteFile(".gitignore", "secret.log\n");
    dir.WriteFile("secret.log", "ignored_needle\n");
    dir.WriteFile("real.txt", "ignored_needle\n");

    SearchTool tool;
    const Tool::Result tree = tool.execute(nlohmann::json{
        {"mode", "grep"}, {"pattern", "ignored_needle"}, {"path", dir.Utf8Path()}});
    CHECK_FALSE(tree.is_error);
    CHECK(tree.content.find("real.txt") != std::string::npos);
    CHECK(tree.content.find("secret.log") == std::string::npos);  // 默认不搜 ignored

    const Tool::Result explicit_file = tool.execute(nlohmann::json{
        {"mode", "grep"}, {"pattern", "ignored_needle"}, {"path", dir.Utf8Path("secret.log")}});
    CHECK_FALSE(explicit_file.is_error);
    CHECK(explicit_file.content.find("secret.log:1:ignored_needle") != std::string::npos);

    // rg 的优先级合同(15.2.0 实测,差分批准表里记了账):显式 include glob
    // 压过 ignore 规则——被 ignore 的 secret.log 只要配得上 *.log 就回来。
    // 宿主的 !**/build/** 等排除项不受影响(它们是排除,不是 include)。
    const Tool::Result glob_override = tool.execute(nlohmann::json{
        {"mode", "grep"}, {"pattern", "ignored_needle"}, {"path", dir.Utf8Path()},
        {"glob", "*.log"}});
    CHECK_FALSE(glob_override.is_error);
    CHECK(glob_override.content.find("secret.log") != std::string::npos);
    CHECK(glob_override.content.find("real.txt") == std::string::npos);  // include 只认 *.log
}

TEST_CASE("真 rg: 隐藏文件可搜、.git/build 硬排除、显式点名 build/ 放行") {
    const TempDir dir;
    dir.WriteFile(".hidden_cfg", "hidden_needle\n");
    dir.WriteFile(".github/workflows.yml", "hidden_needle\n");
    dir.WriteFile(".git/objects/obj", "hidden_needle\n");
    dir.WriteFile("build/generated.txt", "hidden_needle\n");
    dir.WriteFile("real.txt", "hidden_needle\n");

    SearchTool tool;
    const Tool::Result tree = tool.execute(nlohmann::json{
        {"mode", "grep"}, {"pattern", "hidden_needle"}, {"path", dir.Utf8Path()}});
    CHECK_FALSE(tree.is_error);
    CHECK(tree.content.find(".hidden_cfg") != std::string::npos);       // --hidden
    CHECK(tree.content.find(".github") != std::string::npos);           // 项目文件可搜
    CHECK(tree.content.find(".git/objects") == std::string::npos);      // 硬排除
    CHECK(tree.content.find("build/") == std::string::npos);            // 硬排除
    CHECK(tree.content.find("real.txt") != std::string::npos);

    const Tool::Result named_build = tool.execute(nlohmann::json{
        {"mode", "grep"}, {"pattern", "hidden_needle"}, {"path", dir.Utf8Path("build")}});
    CHECK_FALSE(named_build.is_error);
    CHECK(named_build.content.find("generated.txt") != std::string::npos);  // 点名放行
}

TEST_CASE("真 rg: 单文件 path、glob 过滤、glob 模式各层通配") {
    const TempDir dir;
    dir.WriteFile("keep.cpp", "target_word\n");
    dir.WriteFile("skip.txt", "target_word\n");
    dir.WriteFile("docs/a.md", "");
    dir.WriteFile("docs/deep/b.md", "");
    dir.WriteFile("root.md", "");

    SearchTool tool;
    const Tool::Result single = tool.execute(nlohmann::json{
        {"mode", "grep"}, {"pattern", "target_word"}, {"path", dir.Utf8Path("keep.cpp")}});
    CHECK_FALSE(single.is_error);
    CHECK(single.content.find("keep.cpp:1:target_word") != std::string::npos);

    const Tool::Result filtered = tool.execute(nlohmann::json{
        {"mode", "grep"}, {"pattern", "target_word"}, {"path", dir.Utf8Path()},
        {"glob", "*.cpp"}});
    CHECK_FALSE(filtered.is_error);
    CHECK(filtered.content.find("keep.cpp") != std::string::npos);
    CHECK(filtered.content.find("skip.txt") == std::string::npos);

    const Tool::Result one_level = tool.execute(nlohmann::json{
        {"mode", "glob"}, {"pattern", "docs/*.md"}, {"path", dir.Utf8Path()}});
    CHECK_FALSE(one_level.is_error);
    CHECK(one_level.content.find("docs/a.md") != std::string::npos);
    CHECK(one_level.content.find("deep/b.md") == std::string::npos);

    const Tool::Result any_depth = tool.execute(nlohmann::json{
        {"mode", "glob"}, {"pattern", "**/*.md"}, {"path", dir.Utf8Path()}});
    CHECK_FALSE(any_depth.is_error);
    CHECK(any_depth.content.find("root.md") != std::string::npos);
    CHECK(any_depth.content.find("docs/a.md") != std::string::npos);
    CHECK(any_depth.content.find("deep/b.md") != std::string::npos);
}

TEST_CASE("真 rg: CRLF、非法 UTF-8 正文(bytes 路)、单行 20 万字符(16 KiB 截断)") {
    const TempDir dir;
    dir.WriteFile("crlf.txt", "plain\r\nneedle_crlf\r\n");
    // 0xe9 单字节:非法 UTF-8,rg 的 lines 走 base64,宿主解码后清洗。
    dir.WriteFile("latin1.txt", std::string("caf\xe9 latin_needle\n"));
    // 锚点放行首:16 KiB 截断留的是前段,锚点放 20 万字符的尾巴上必被剁掉。
    std::string long_line = "needle_long_anchor ";
    long_line += std::string(200'000, 'x');
    dir.WriteFile("long.txt", long_line + "\n");

    SearchTool tool;
    const Tool::Result crlf = tool.execute(nlohmann::json{
        {"mode", "grep"}, {"pattern", "needle_crlf"}, {"path", dir.Utf8Path("crlf.txt")}});
    CHECK_FALSE(crlf.is_error);
    CHECK(crlf.content.find("\r") == std::string::npos);  // 行内容不带 \r
    CHECK(crlf.content.find("needle_crlf") != std::string::npos);

    const Tool::Result latin = tool.execute(nlohmann::json{
        {"mode", "grep"}, {"pattern", "latin_needle"}, {"path", dir.Utf8Path("latin1.txt")}});
    CHECK_FALSE(latin.is_error);
    CHECK(latin.content.find("latin_needle") != std::string::npos);
    CHECK(latin.content.find('\xe9') == std::string::npos);  // 非法字节清洗过

    const Tool::Result long_hit = tool.execute(nlohmann::json{
        {"mode", "grep"}, {"pattern", "needle_long_anchor"}, {"path", dir.Utf8Path("long.txt")}});
    CHECK_FALSE(long_hit.is_error);
    CHECK(long_hit.content.find("needle_long_anchor") != std::string::npos);
    // 第三道墙:单条命中行截到 16 KiB,不吃掉整个上下文。
    const std::size_t body = long_hit.content.find('\n');
    CHECK(body <= 16 * 1024 + 64);
}

TEST_CASE("真 rg: max_results 事前声明,glob 按文件计") {
    const TempDir dir;
    std::string many;
    for (int i = 0; i < 50; ++i) {
        many += "hit_line\n";
    }
    dir.WriteFile("many.txt", many);
    for (int i = 0; i < 30; ++i) {
        dir.WriteFile("f" + std::to_string(i) + ".txt", "");
    }

    SearchTool tool;
    const Tool::Result grep = tool.execute(nlohmann::json{
        {"mode", "grep"}, {"pattern", "hit_line"}, {"path", dir.Utf8Path()},
        {"max_results", 10}});
    CHECK_FALSE(grep.is_error);
    CHECK(grep.content.find("截断") != std::string::npos);
    int count = 0;
    std::size_t pos = 0;
    while ((pos = grep.content.find("hit_line", pos)) != std::string::npos) {
        ++count;
        pos += 8;
    }
    CHECK(count == 10);

    const Tool::Result glob = tool.execute(nlohmann::json{
        {"mode", "glob"}, {"pattern", "*.txt"}, {"path", dir.Utf8Path()},
        {"max_results", 5}});
    CHECK_FALSE(glob.is_error);
    CHECK(glob.content.find("截断") != std::string::npos);
    int files = 0;
    std::size_t gpos = 0;
    while ((gpos = glob.content.find(".txt", gpos)) != std::string::npos) {
        ++files;
        gpos += 4;
    }
    CHECK(files == 5);
}

TEST_CASE("真 rg: 观察边界登记目录默认剪枝,显式点名放行") {
    // 与旧内核同一合同:默认递归时边界内目录不搜;path 点名进边界照常搜。
    const TempDir dir;
    dir.WriteFile("logs/subagent.log", "boundary_needle\n");
    dir.WriteFile("real.txt", "boundary_needle\n");
    lubancode::tools::ObservationBoundary::Instance().Reset();
    lubancode::tools::ObservationBoundary::Instance().AddExcludedDir(dir.Path() / Utf8ToPath("logs"));

    SearchTool tool;
    const Tool::Result tree = tool.execute(nlohmann::json{
        {"mode", "grep"}, {"pattern", "boundary_needle"}, {"path", dir.Utf8Path()}});
    CHECK_FALSE(tree.is_error);
    CHECK(tree.content.find("real.txt") != std::string::npos);
    CHECK(tree.content.find("subagent.log") == std::string::npos);  // 登记目录剪枝

    const Tool::Result named = tool.execute(nlohmann::json{
        {"mode", "grep"}, {"pattern", "boundary_needle"}, {"path", dir.Utf8Path("logs")}});
    CHECK_FALSE(named.is_error);
    CHECK(named.content.find("subagent.log") != std::string::npos);  // 点名放行

    lubancode::tools::ObservationBoundary::Instance().Reset();
}

TEST_CASE("真 rg: 本仓 src/ 冒烟(grep/glob 双模式,中型语料)") {
    // 不钉具体内容(仓库内容会长大),钉"能搜、不报错、有产出"。
    const std::filesystem::path repo_src =
        std::filesystem::path(LUBANCODE_SOURCE_DIR) / "src";
    if (!std::filesystem::exists(repo_src)) {
        return;  // 源码树不完整的构建:跳过
    }
    SearchTool tool;
    const Tool::Result grep = tool.execute(nlohmann::json{
        {"mode", "grep"}, {"pattern", "SearchTool"}, {"path", PathToUtf8(repo_src)}});
    CHECK_FALSE(grep.is_error);
    CHECK(grep.content.find("search") != std::string::npos);

    const Tool::Result glob = tool.execute(nlohmann::json{
        {"mode", "glob"}, {"pattern", "**/*.hpp"}, {"path", PathToUtf8(repo_src)}});
    CHECK_FALSE(glob.is_error);
    CHECK(glob.content.find(".hpp") != std::string::npos);
}

#else

TEST_CASE("真 rg 集成组: 未分期入位,如实跳过") {
    MESSAGE("LUBANCODE_BUNDLED_RG_DIR 未设置:真 ripgrep 集成组跳过"
            "(configure 传 -DLUBANCODE_BUNDLED_RG_DIR=<fetch_ripgrep.py 产物目录> 启用)");
}

#endif  // LUBANCODE_TEST_HAS_BUNDLED_RG
