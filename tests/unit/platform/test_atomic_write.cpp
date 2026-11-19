// 统一原子写 platform::AtomicWriteFile 的回归(src 重复职责收口审计
// P1:目标已存在/目标不存在/替换失败/临时写失败/两写者并发/中断残留;
// FD-04:提交阶段合同——未提交与"已替换未耐久"分开,失败注入分阶段)。
// 合同要点:
//   - 任何失败路径不删正式文件换取成功(Windows 上老写法"rename 不动就
//     先 remove(target) 再 rename"留出文件不存在窗口,正是本件要杀的);
//   - 唯一临时名:并发写同一目标不互踩;
//   - 失败后自己的临时件删净;
//   - 两档持久明分:AtomicVisibility / ProcessCrashDurability 都能写;
//   - 失败带阶段(FD-04):换名前失败 outcome=NotCommitted(盘上原样),
//     换名后目录刷盘失败 outcome=CommittedDurabilityUnconfirmed(新内容
//     已可见,不得当未写盘处理);
//   - 成功回执分档:AtomicVisibility 成功不冒充已确认耐久。
#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "platform/atomic_write.hpp"
#include "platform/paths.hpp"

using lubancode::platform::AtomicWriteFile;
using lubancode::platform::WriteDurability;
using lubancode::platform::WriteFailureKind;
using lubancode::platform::WriteOutcome;

namespace {

std::filesystem::path MakeTempRoot(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-atomic-write-" + std::string(name));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string ReadAll(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteAll(const std::filesystem::path& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

// 目录里所有"像临时件"的文件名(带 .tmp 后缀的)。
std::set<std::string> TempLeftovers(const std::filesystem::path& dir) {
    std::set<std::string> found;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = lubancode::platform::PathToUtf8(entry.path().filename());
        if (name.find(".tmp") != std::string::npos) {
            found.insert(name);
        }
    }
    return found;
}

// 失败注入钩子的 RAII 还原:REQUIRE 半路炸了也不许把注入漏给后面的用例。
struct HookGuard {
    ~HookGuard() {
        lubancode::platform::SetFileFlushFailureForTest(false);
        lubancode::platform::SetDirectoryFlushFailureForTest(false);
    }
};

}  // namespace

TEST_CASE("AtomicWriteFile: 目标已存在——整份换新,无临时件残留") {
    const auto root = MakeTempRoot("exists");
    const auto target = root / "state.json";
    WriteAll(target, R"({"v":1})");

    const auto result = AtomicWriteFile(target, R"({"v":2})");
    REQUIRE(result.has_value());
    CHECK(ReadAll(target) == R"({"v":2})");
    CHECK(TempLeftovers(root).empty());
}

TEST_CASE("AtomicWriteFile: 目标不存在——直接落成") {
    const auto root = MakeTempRoot("fresh");
    const auto target = root / "nested" / "dir" / "state.json";

    const auto result = AtomicWriteFile(target, "hello");
    REQUIRE(result.has_value());
    CHECK(ReadAll(target) == "hello");
    CHECK(TempLeftovers(root / "nested" / "dir").empty());
}

TEST_CASE("AtomicWriteFile: 替换失败——报错收场,绝不删正式文件换成功") {
    const auto root = MakeTempRoot("replace-fail");
    // 目标是一个(空)目录:平台原子替换(POSIX rename/file->dir、Windows
    // MoveFileEx)都换不上去。老的私房写法会先 remove(target) 删掉空目录
    // 再 rename 换成"成功"——那份成功是拿"正式目标先消失"换来的,这里
    // 钉死:必须报错,目录必须还在。
    const auto target = root / "occupied";
    std::error_code ec;
    std::filesystem::create_directory(target, ec);
    REQUIRE(std::filesystem::is_directory(target));

    const auto result = AtomicWriteFile(target, "x");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == "atomic.replace_failed");
    CHECK(std::filesystem::is_directory(target));
    CHECK(TempLeftovers(root).empty());
}

TEST_CASE("AtomicWriteFile: 临时写失败——结构化报错,不留临时件") {
    const auto root = MakeTempRoot("tmp-fail");
    // 文件名分量超长(>255 字符):两平台 fopen 都打不开临时件,父目录本身
    // 合法——踩中的是 tmp_open_failed 这格,不是 mkdir。
    const std::string long_name(300, 'n');
    const auto target = root / long_name;

    const auto result = AtomicWriteFile(target, "x");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == "atomic.tmp_open_failed");
    // 目录里除 root 自身外空空如也:没有临时件尾巴。
    CHECK(TempLeftovers(root).empty());
}

TEST_CASE("AtomicWriteFile: 父目录建不成——mkdir_failed 格") {
    const auto root = MakeTempRoot("mkdir-fail");
    // 父路径被一个普通文件占着:建目录必败。
    const auto blocker = root / "blocker";
    WriteAll(blocker, "not a dir");
    const auto target = blocker / "state.json";

    const auto result = AtomicWriteFile(target, "x");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == "atomic.mkdir_failed");
    CHECK(std::filesystem::is_regular_file(blocker));
    CHECK(ReadAll(blocker) == "not a dir");
}

TEST_CASE("AtomicWriteFile: 两写者并发——同一目标只见整份,临时件不互踩") {
    const auto root = MakeTempRoot("concurrent");
    const auto target = root / "shared.json";

    // 每个写者反复写自己的整份标记(A 方 4KB 'a' 行、B 方 4KB 'b' 行),
    // 内容足够大,让固定临时名 + 截断打开的互踩有概率掺出半份。并发收尾
    // 后目标必须是某一方的整份,绝无混合。
    constexpr int kRounds = 120;
    const std::string payload_a(4096, 'a');
    const std::string payload_b(4096, 'b');

    std::atomic<bool> failed{false};
    std::mutex error_mutex;
    std::string first_error;
    const auto writer = [&](const std::string& payload) {
        for (int i = 0; i < kRounds; ++i) {
            const auto result = AtomicWriteFile(target, payload);
            if (!result.has_value()) {
                failed = true;
                const std::lock_guard<std::mutex> lock(error_mutex);
                if (first_error.empty()) {
                    first_error = result.error().code + ": " + result.error().message;
                }
                return;
            }
        }
    };
    std::thread ta(writer, payload_a);
    std::thread tb(writer, payload_b);
    ta.join();
    tb.join();

    REQUIRE_MESSAGE(!failed.load(), "并发写各自都该成功,首错: ", first_error);
    const std::string final_content = ReadAll(target);
    CHECK(final_content.size() == 4096);
    const bool all_a = final_content.find_first_not_of('a') == std::string::npos;
    const bool all_b = final_content.find_first_not_of('b') == std::string::npos;
    const bool whole_copy = all_a || all_b;
    CHECK_MESSAGE(whole_copy, "并发写收尾后目标必须是某一方的整份,不得掺半");
    CHECK(TempLeftovers(root).empty());
}

TEST_CASE("AtomicWriteFile: 中断残留——陈年临时件不碍事,失败路径不留新尾巴") {
    const auto root = MakeTempRoot("leftover");
    const auto target = root / "state.json";
    WriteAll(target, "old");
    // 模拟别的进程/上次中断留下的陈年临时件(含老协议的固定 .tmp 名)。
    WriteAll(root / "state.json.tmp", "stale");
    WriteAll(root / "state.json.99999-42.tmp", "stale-too");

    REQUIRE(AtomicWriteFile(target, "new").has_value());
    CHECK(ReadAll(target) == "new");

    // 失败路径同样不许新增临时件:拿"目标是目录"造替换失败,目录清单里
    // 的 .tmp 件应该仍是那两个陈年货,没有新面孔。
    const auto blocked = root / "blocked";
    std::error_code ec;
    std::filesystem::create_directory(blocked, ec);
    REQUIRE_FALSE(AtomicWriteFile(blocked, "x").has_value());
    const std::set<std::string> leftovers = TempLeftovers(root);
    const std::set<std::string> stale_only = {"state.json.tmp", "state.json.99999-42.tmp"};
    const bool only_stale = leftovers == stale_only;
    CHECK_MESSAGE(only_stale, "失败路径不得新增临时件,只许剩陈年货");
}

TEST_CASE("AtomicWriteFile: 两档持久都能写,内容一致") {
    const auto root = MakeTempRoot("durability");
    const auto visible = root / "visible.json";
    const auto durable = root / "durable.json";

    REQUIRE(AtomicWriteFile(visible, "same", WriteDurability::AtomicVisibility).has_value());
    REQUIRE(AtomicWriteFile(durable, "same", WriteDurability::ProcessCrashDurability).has_value());
    CHECK(ReadAll(visible) == "same");
    CHECK(ReadAll(durable) == "same");
    CHECK(TempLeftovers(root).empty());
}

TEST_CASE("AtomicWriteFile: 空内容与父目录缺失都合法") {
    const auto root = MakeTempRoot("edge");
    REQUIRE(AtomicWriteFile(root / "empty.json", "").has_value());
    CHECK(std::filesystem::file_size(root / "empty.json") == 0);
    CHECK(ReadAll(root / "empty.json").empty());
}

// ---- FD-04 提交阶段合同 ----------------------------------------------------

TEST_CASE("AtomicWriteFile: 成功回执分档——耐久未请求不冒充已确认") {
    const auto root = MakeTempRoot("receipt");
    const auto visible = root / "visible.json";
    const auto durable = root / "durable.json";

    const auto vis = AtomicWriteFile(visible, "v", WriteDurability::AtomicVisibility);
    REQUIRE(vis.has_value());
    // AtomicVisibility 成功只保证换名可见;回执不得标成耐久已确认。
    CHECK(vis->outcome == WriteOutcome::CommittedDurabilityNotRequested);

    const auto dur = AtomicWriteFile(durable, "d", WriteDurability::ProcessCrashDurability);
    REQUIRE(dur.has_value());
    CHECK(dur->outcome == WriteOutcome::CommittedDurable);
}

TEST_CASE("AtomicWriteFile: 换名前失败——outcome=NotCommitted,盘上旧内容原样") {
    const auto root = MakeTempRoot("pre-commit-fail");
    const auto target = root / "state.json";
    WriteAll(target, "old");

    // 建目录挡路(mkdir_failed)。
    const auto blocker = root / "blocker";
    WriteAll(blocker, "not a dir");
    const auto mkdir_result = AtomicWriteFile(blocker / "x.json", "new");
    REQUIRE_FALSE(mkdir_result.has_value());
    CHECK(mkdir_result.error().code == "atomic.mkdir_failed");
    CHECK(mkdir_result.error().outcome == WriteOutcome::NotCommitted);
    CHECK(mkdir_result.error().failure_kind == WriteFailureKind::Permanent);

    // 文件名分量超长(tmp_open_failed)。
    const std::string long_name(300, 'n');
    const auto open_result = AtomicWriteFile(root / long_name, "new");
    REQUIRE_FALSE(open_result.has_value());
    CHECK(open_result.error().code == "atomic.tmp_open_failed");
    CHECK(open_result.error().outcome == WriteOutcome::NotCommitted);
    CHECK(open_result.error().failure_kind == WriteFailureKind::Permanent);

    // 铺底:把 target 换成一份长内容,后面统一断言"换名前失败不动它"。
    WriteAll(target, std::string(4000, 'x'));
    const auto occupied = root / "occupied";
    std::error_code ec;
    std::filesystem::create_directory(occupied, ec);
    const auto replace_result = AtomicWriteFile(occupied, "new");
    REQUIRE_FALSE(replace_result.has_value());
    CHECK(replace_result.error().code == "atomic.replace_failed");
    CHECK(replace_result.error().outcome == WriteOutcome::NotCommitted);
    CHECK(std::filesystem::is_directory(occupied));

    // 盘面:换名前失败一概旧内容原样,没有临时件尾巴。
    CHECK(ReadAll(target) == std::string(4000, 'x'));
    CHECK(TempLeftovers(root).empty());
}

TEST_CASE("AtomicWriteFile: 注入文件刷盘失败——未提交,旧内容原样") {
    HookGuard guard;
    lubancode::platform::SetFileFlushFailureForTest(true);

    const auto root = MakeTempRoot("inject-file-flush");
    const auto target = root / "state.json";
    WriteAll(target, "old");

    // ProcessCrashDurability 档的文件刷盘发生在换名之前:这一步失败,
    // 换名没发生,target 必须还是旧内容——这就是"未提交"阶段。
    const auto result = AtomicWriteFile(target, "new", WriteDurability::ProcessCrashDurability);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == "atomic.tmp_write_failed");
    CHECK(result.error().outcome == WriteOutcome::NotCommitted);
    CHECK(result.error().failure_kind == WriteFailureKind::Permanent);
    CHECK(ReadAll(target) == "old");
    CHECK(TempLeftovers(root).empty());

    // 同一注入下 AtomicVisibility 档不受影响:文件刷盘不在该档合同里。
    const auto vis = AtomicWriteFile(root / "vis.json", "v", WriteDurability::AtomicVisibility);
    REQUIRE(vis.has_value());
    CHECK(vis->outcome == WriteOutcome::CommittedDurabilityNotRequested);
}

TEST_CASE("AtomicWriteFile: 注入目录刷盘失败——已提交可见,只报耐久未确认") {
    HookGuard guard;
    lubancode::platform::SetDirectoryFlushFailureForTest(true);

    const auto root = MakeTempRoot("inject-dir-flush");
    const auto target = root / "state.json";
    WriteAll(target, "old");

    // 换名已生效、父目录条目没确认落盘:同一个 unexpected 形状,但盘面
    // 是新内容已可见。上层不得当"未写盘"回滚内存,更不得删 target。
    const auto result = AtomicWriteFile(target, "new", WriteDurability::ProcessCrashDurability);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == "atomic.durability_flush_failed");
    CHECK(result.error().outcome == WriteOutcome::CommittedDurabilityUnconfirmed);
    CHECK(result.error().failure_kind == WriteFailureKind::Permanent);
    CHECK(ReadAll(target) == "new");
    CHECK(TempLeftovers(root).empty());

    // 同一注入下 AtomicVisibility 档照常成功:目录刷盘不在该档合同里。
    const auto vis = AtomicWriteFile(root / "vis.json", "v", WriteDurability::AtomicVisibility);
    REQUIRE(vis.has_value());
    CHECK(vis->outcome == WriteOutcome::CommittedDurabilityNotRequested);
}

TEST_CASE("AtomicWriteFile: 裸文件名无父目录——耐久档按合同视为已确认") {
    // 唯一文件名落在进程当前目录(测试可写),完事删净。钉死现行合同:
    // 无父段可开目录句柄,FlushParentDirectory 空路径直接成功,回执记
    // CommittedDurable(文件数据那一层照刷)。
    static std::atomic<int> seq{0};
    const std::string name = "lubancode-atomic-bare-" + std::to_string(seq.fetch_add(1)) + ".json";
    const auto result =
        AtomicWriteFile(std::filesystem::path(name), "bare", WriteDurability::ProcessCrashDurability);
    REQUIRE(result.has_value());
    CHECK(result->outcome == WriteOutcome::CommittedDurable);
    CHECK(ReadAll(name) == "bare");
    std::error_code ignored;
    std::filesystem::remove(std::filesystem::path(name), ignored);
}

TEST_CASE("AtomicWriteFile: 替换失败的短拒分类——Windows 记可重试,POSIX 记真失败") {
    const auto root = MakeTempRoot("replace-kind");
    const auto target = root / "occupied";
    std::error_code ec;
    std::filesystem::create_directory(target, ec);
    REQUIRE(std::filesystem::is_directory(target));

    const auto result = AtomicWriteFile(target, "x");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == "atomic.replace_failed");
    CHECK(result.error().outcome == WriteOutcome::NotCommitted);
#ifdef _WIN32
    // Windows 的原子换名有防病毒/过滤驱动短拒类(work_pump.cpp 三案 CI
    // 实测注记):replace_failed 记 TransientReject,调用方有界重试;少数
    // 永久因(目标是目录)混在同类里——重试无害,换名原子、失败时 target
    // 未动,有界重试后仍失败照实报。
    CHECK(result.error().failure_kind == WriteFailureKind::TransientReject);
#else
    CHECK(result.error().failure_kind == WriteFailureKind::Permanent);
#endif
    CHECK(std::filesystem::is_directory(target));
}

TEST_CASE("FileIoPath: short paths retain their original spelling") {
    namespace fs = std::filesystem;
    using lubancode::platform::FileIoPath;
    const fs::path relative = fs::path("short") / "state.json";
    REQUIRE(fs::absolute(relative).native().size() < 248);
    CHECK(FileIoPath(relative).native() == relative.native());
    CHECK(FileIoPath(fs::path{}).empty());
#ifndef _WIN32
    const fs::path long_path = fs::path("/tmp") / std::string(200, 'a') / std::string(100, 'b');
    CHECK(FileIoPath(long_path).native() == long_path.native());
#endif
}

#ifdef _WIN32
namespace {
// The disk witness uses an explicit native path, independently of FileIoPath.
// A helper that points at the wrong file cannot make these content checks pass.
std::filesystem::path ExplicitWindowsPath(std::filesystem::path path) {
    path = std::filesystem::absolute(path).lexically_normal();
    path.make_preferred();
    const auto native = path.native();
    return std::filesystem::path(native.starts_with(L"\\\\")
        ? L"\\\\?\\UNC\\" + native.substr(2) : L"\\\\?\\" + native);
}

struct LongAtomicRoot {
    std::filesystem::path path;
    explicit LongAtomicRoot(const char* name) : path(MakeTempRoot(name)) {}
    ~LongAtomicRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(ExplicitWindowsPath(path), ignored);
    }
};
} // namespace

TEST_CASE("FileIoPath: Windows UNC and explicit namespaces preserve their meaning") {
    namespace fs = std::filesystem;
    using lubancode::platform::FileIoPath;
    const std::wstring tail = std::wstring(100, L'a') + L"\\" + std::wstring(100, L'b') +
                              L"\\" + std::wstring(60, L'c') + L"\\state.json";
    const fs::path unc(L"\\\\server\\share\\" + tail);
    CHECK(FileIoPath(unc).native() == L"\\\\?\\UNC\\server\\share\\" + tail);
    const fs::path extended(L"\\\\?\\C:\\already\\file.");
    const fs::path device(L"\\\\.\\NUL");
    CHECK(FileIoPath(extended).native() == extended.native());
    CHECK(FileIoPath(device).native() == device.native());
    for (const auto& path : {fs::path(L"C:\\" + tail + L"."), fs::path(L"C:\\" + tail + L" "),
                            fs::path(L"C:\\parent.\\" + tail), fs::path(L"C:\\parent \\" + tail)}) {
        CHECK(FileIoPath(path).native() == path.native());
    }
    for (const auto* reserved : {L"NUL.txt", L"CoM1", L"LPT9.log", L"COM¹.txt", L"NUL:stream"}) {
        const fs::path path = fs::path(L"C:\\" + tail) / reserved;
        CHECK(FileIoPath(path).native() == path.native());
    }
    const fs::path ordinary = fs::path(L"C:\\" + tail) / L"COM10.txt";
    CHECK(FileIoPath(ordinary).native() == ExplicitWindowsPath(ordinary).native());
    const fs::path relative = fs::path("relative-long-path") / fs::path(tail);
    REQUIRE_FALSE(relative.is_absolute());
    CHECK(FileIoPath(relative).native() == ExplicitWindowsPath(relative).native());
}

TEST_CASE("AtomicWriteFile: Windows long logical paths create replace and clean up real files") {
    namespace fs = std::filesystem;
    LongAtomicRoot root("long-native-io");
    auto parent = root.path;
    unsigned level = 0;
    while (parent.native().size() < 285) parent /= std::string(32, 'd') + std::to_string(++level);
    const auto target = parent / lubancode::platform::Utf8ToPath("完整结果.txt");
    REQUIRE(target.native().size() > 260);
    REQUIRE_FALSE(target.native().starts_with(L"\\\\?\\"));
    const auto native_target = ExplicitWindowsPath(target);
    const auto native_parent = ExplicitWindowsPath(parent);

    const auto created = AtomicWriteFile(target, "original long-path bytes");
    REQUIRE_MESSAGE(created.has_value(), (created ? "" : created.error().message));
    REQUIRE(fs::is_regular_file(native_target));
    CHECK(ReadAll(native_target) == "original long-path bytes");
    const auto replaced = AtomicWriteFile(target, "replacement long-path bytes", WriteDurability::ProcessCrashDurability);
    REQUIRE_MESSAGE(replaced.has_value(), (replaced ? "" : replaced.error().message));
    CHECK(replaced->outcome == WriteOutcome::CommittedDurable);
    CHECK(ReadAll(native_target) == "replacement long-path bytes");
    CHECK(TempLeftovers(native_parent).empty());

    HookGuard guard;
    lubancode::platform::SetFileFlushFailureForTest(true);
    const auto failed = AtomicWriteFile(target, "must not replace", WriteDurability::ProcessCrashDurability);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == "atomic.tmp_write_failed");
    CHECK(failed.error().outcome == WriteOutcome::NotCommitted);
    CHECK(ReadAll(native_target) == "replacement long-path bytes");
    CHECK(TempLeftovers(native_parent).empty());
    lubancode::platform::SetFileFlushFailureForTest(false);

    const auto occupied = parent / "occupied";
    REQUIRE(fs::create_directory(ExplicitWindowsPath(occupied)));
    const auto rejected = AtomicWriteFile(occupied, "not a directory");
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == "atomic.replace_failed");
    CHECK(fs::is_directory(ExplicitWindowsPath(occupied)));
    CHECK(TempLeftovers(native_parent).empty());
}

TEST_CASE("AtomicWriteFile: Windows temporary suffix can cross MAX_PATH before the target does") {
    namespace fs = std::filesystem;
    LongAtomicRoot root("long-temp-only");
    constexpr std::size_t target_length = 255;
    REQUIRE(root.path.native().size() + 12 < target_length);
    const auto filename_length = target_length - root.path.native().size() - 1;
    REQUIRE(filename_length < 240); // every component, including its temp suffix, stays legal
    const auto target = root.path / (std::string(filename_length - 4, 'f') + ".txt");
    REQUIRE(target.native().size() == target_length);
    REQUIRE(target.native().size() < 260);
    REQUIRE(target.native().size() + std::string(".1-0.tmp").size() >= 260);

    REQUIRE(AtomicWriteFile(target, "before").has_value());
    REQUIRE(AtomicWriteFile(target, "after", WriteDurability::ProcessCrashDurability).has_value());
    // The logical target itself is below MAX_PATH; only the writer's temporary
    // filename needs the extended form. Read the original spelling directly.
    CHECK(ReadAll(target) == "after");
    CHECK(TempLeftovers(root.path).empty());
}
#endif

// The original nineteen CASE declarations above remain unchanged. These six
// exercise the internal SDK plan retry seam, not a global AtomicWrite policy.
#include <chrono>
#include <iostream>
#include "sdk/plan_write.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {
using PlanClock = std::chrono::steady_clock;
using PlanWriteResult = std::expected<lubancode::platform::AtomicWriteReceipt,
                                      lubancode::platform::AtomicWriteError>;
using lubancore::detail::WriteFrozenPlanWithRetry;

lubancode::platform::AtomicWriteError PlanRejected(unsigned attempt = 1) {
    return {"atomic.replace_failed", "original short rejection " + std::to_string(attempt),
            WriteOutcome::NotCommitted, WriteFailureKind::TransientReject};
}

struct PlanRetryRoot {
    std::filesystem::path path;
    explicit PlanRetryRoot(const char* name) : path(MakeTempRoot(name)) {}
    ~PlanRetryRoot() { std::error_code error; std::filesystem::remove_all(path, error); }
};

#ifdef _WIN32
struct PlanBlockingHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~PlanBlockingHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    bool Close() {
        if (value == INVALID_HANDLE_VALUE) return false;
        if (!CloseHandle(value)) return false;
        value = INVALID_HANDLE_VALUE;
        return true;
    }
};
#endif
} // namespace

TEST_CASE("SDK frozen plan: typed short rejection retries identical bytes then commits durably") {
    PlanRetryRoot root("sdk-plan-retry-success");
    const auto target = root.path / "plan.json";
    const std::string frozen = R"({"sessionId":"this-session","schemaVersion":1})";
    WriteAll(target, "old plan");
    unsigned calls = 0, waits = 0;
    auto time = PlanClock::time_point{};
    const auto saved = WriteFrozenPlanWithRetry(target, frozen,
        [&](const auto& path, std::string_view bytes, WriteDurability durability) -> PlanWriteResult {
            CHECK(path == target);
            CHECK(bytes == frozen);
            CHECK(durability == WriteDurability::ProcessCrashDurability);
            if (++calls == 1) return std::unexpected(PlanRejected());
            return AtomicWriteFile(path, bytes, durability);
        }, [&] { return time; }, [&](PlanClock::duration duration) {
            CHECK(duration == std::chrono::milliseconds(20));
            CHECK(ReadAll(target) == "old plan");
            ++waits; time += duration;
        });
    REQUIRE(saved.has_value());
    REQUIRE(saved->outcome == WriteOutcome::CommittedDurable);
    CHECK(calls == 2); CHECK(waits == 1);
    CHECK(ReadAll(target) == frozen);
    CHECK(TempLeftovers(root.path).empty());
    std::cout << "[sdk-plan-retry] retry-success\n";
}

TEST_CASE("SDK frozen plan: permanent refusal and real pre-commit flush failure stop immediately") {
    PlanRetryRoot root("sdk-plan-permanent");
    const auto target = root.path / "plan.json";
    WriteAll(target, "old plan");
    unsigned calls = 0, waits = 0;
    const lubancode::platform::AtomicWriteError permanent{
        "atomic.tmp_open_failed", "original temporary open error", WriteOutcome::NotCommitted,
        WriteFailureKind::Permanent};
    const auto refused = WriteFrozenPlanWithRetry(target, "new plan",
        [&](const auto&, auto, auto) -> PlanWriteResult { ++calls; return std::unexpected(permanent); },
        [] { return PlanClock::time_point{}; }, [&](auto) { ++waits; });
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == permanent.code); CHECK(refused.error().message == permanent.message);
    CHECK(refused.error().failure_kind == WriteFailureKind::Permanent);
    CHECK(refused.error().outcome == WriteOutcome::NotCommitted);
    CHECK(calls == 1); CHECK(waits == 0);
    HookGuard hooks;
    lubancode::platform::SetFileFlushFailureForTest(true);
    calls = 0;
    const auto failed = WriteFrozenPlanWithRetry(target, "new plan",
        [&](const auto& path, auto bytes, auto durability) { ++calls; return AtomicWriteFile(path, bytes, durability); },
        [] { return PlanClock::time_point{}; }, [&](auto) { ++waits; });
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == "atomic.tmp_write_failed");
    CHECK(failed.error().failure_kind == WriteFailureKind::Permanent);
    CHECK(failed.error().outcome == WriteOutcome::NotCommitted);
    CHECK(calls == 1); CHECK(waits == 0);
    CHECK(ReadAll(target) == "old plan");
    CHECK(TempLeftovers(root.path).empty());
    std::cout << "[sdk-plan-retry] permanent-stop\n";
}

TEST_CASE("SDK frozen plan: any committed outcome stops and actual post-replace uncertainty stays visible") {
    PlanRetryRoot root("sdk-plan-committed");
    const auto target = root.path / "plan.json";
    WriteAll(target, "old plan");
    for (const auto outcome : {WriteOutcome::CommittedDurabilityNotRequested,
                               WriteOutcome::CommittedDurabilityUnconfirmed, WriteOutcome::CommittedDurable}) {
        unsigned calls = 0, waits = 0;
        auto error = PlanRejected(); error.outcome = outcome;
        const auto refused = WriteFrozenPlanWithRetry(target, "new plan",
            [&](const auto&, auto, auto) -> PlanWriteResult { ++calls; return std::unexpected(error); },
            [] { return PlanClock::time_point{}; }, [&](auto) { ++waits; });
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == error.code); CHECK(refused.error().message == error.message);
        CHECK(refused.error().outcome == outcome);
        CHECK(refused.error().failure_kind == WriteFailureKind::TransientReject);
        CHECK(calls == 1); CHECK(waits == 0);
        // Even an unusual successful receipt is returned once, never retried.
        // Each of the three real plan owners separately requires Durable before binding.
        const auto receipt = WriteFrozenPlanWithRetry(target, "new plan",
            [&](const auto&, auto, auto) -> PlanWriteResult { ++calls; return lubancode::platform::AtomicWriteReceipt{outcome}; },
            [] { return PlanClock::time_point{}; }, [&](auto) { ++waits; });
        REQUIRE(receipt.has_value()); CHECK(receipt->outcome == outcome);
        CHECK(calls == 2); CHECK(waits == 0);
    }
    HookGuard hooks;
    lubancode::platform::SetDirectoryFlushFailureForTest(true);
    unsigned calls = 0, waits = 0;
    const auto failed = WriteFrozenPlanWithRetry(target, "new plan",
        [&](const auto& path, auto bytes, auto durability) { ++calls; return AtomicWriteFile(path, bytes, durability); },
        [] { return PlanClock::time_point{}; }, [&](auto) { ++waits; });
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == "atomic.durability_flush_failed");
    CHECK(failed.error().outcome == WriteOutcome::CommittedDurabilityUnconfirmed);
    CHECK(calls == 1); CHECK(waits == 0);
    CHECK(ReadAll(target) == "new plan"); // No rollback or second publication.
    CHECK(TempLeftovers(root.path).empty());
    std::cout << "[sdk-plan-retry] committed-stop\n";
}

TEST_CASE("SDK frozen plan: attempt budget returns the last original error without touching target") {
    PlanRetryRoot root("sdk-plan-attempts");
    const auto target = root.path / "plan.json";
    WriteAll(target, "old plan");
    unsigned calls = 0, waits = 0;
    const auto failed = WriteFrozenPlanWithRetry(target, "new plan",
        [&](const auto& path, auto bytes, auto durability) -> PlanWriteResult {
            CHECK(path == target); CHECK(bytes == "new plan");
            CHECK(durability == WriteDurability::ProcessCrashDurability);
            return std::unexpected(PlanRejected(++calls));
        }, [] { return PlanClock::time_point{}; }, [&](PlanClock::duration duration) {
            CHECK(duration == std::chrono::milliseconds(20)); ++waits;
        });
    REQUIRE_FALSE(failed.has_value());
    CHECK(calls == 51); CHECK(waits == 50);
    CHECK(failed.error().code == "atomic.replace_failed");
    CHECK(failed.error().message == "original short rejection 51");
    CHECK(failed.error().failure_kind == WriteFailureKind::TransientReject);
    CHECK(failed.error().outcome == WriteOutcome::NotCommitted);
    CHECK(ReadAll(target) == "old plan"); CHECK(TempLeftovers(root.path).empty());
    std::cout << "[sdk-plan-retry] attempt-budget\n";
}

TEST_CASE("SDK frozen plan: deadline bounds rescheduling and clamps the remaining wait") {
    PlanRetryRoot root("sdk-plan-deadline");
    const auto target = root.path / "plan.json";
    WriteAll(target, "old plan");
    for (const auto elapsed : {std::chrono::milliseconds(1000), std::chrono::milliseconds(990)}) {
        unsigned calls = 0, waits = 0;
        auto time = PlanClock::time_point{};
        const auto failed = WriteFrozenPlanWithRetry(target, "new plan",
            [&](const auto&, auto, auto) -> PlanWriteResult { ++calls; time += elapsed; return std::unexpected(PlanRejected()); },
            [&] { return time; }, [&](PlanClock::duration duration) {
                CHECK(duration == std::chrono::milliseconds(10)); ++waits; time += duration;
            });
        REQUIRE_FALSE(failed.has_value()); CHECK(calls == 1);
        CHECK(waits == (elapsed == std::chrono::milliseconds(990) ? 1 : 0));
        CHECK(failed.error().code == "atomic.replace_failed");
        CHECK(failed.error().message == "original short rejection 1");
        CHECK(failed.error().failure_kind == WriteFailureKind::TransientReject);
        CHECK(failed.error().outcome == WriteOutcome::NotCommitted);
    }
    CHECK(ReadAll(target) == "old plan"); CHECK(TempLeftovers(root.path).empty());
    std::cout << "[sdk-plan-retry] deadline-budget\n";
}

TEST_CASE("SDK frozen plan: native sharing rejection releases the actual handle before durable retry") {
    PlanRetryRoot root("sdk-plan-native");
    const auto target = root.path / "plan.json";
    const std::string frozen = R"({"sessionId":"native-session","schemaVersion":1})";
    WriteAll(target, "old plan"); REQUIRE(ReadAll(target) == "old plan");
#ifdef _WIN32
    PlanBlockingHandle held;
    const auto native = lubancode::platform::FileIoPath(target);
    held.value = CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held.value != INVALID_HANDLE_VALUE);
    // DELETE-open and replacement are distinct native calls. The held handle
    // rejects deletion with 32; MoveFileExW may report 5 for that same denial.
    PlanBlockingHandle delete_probe;
    delete_probe.value = CreateFileW(native.c_str(), DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD delete_error = delete_probe.value == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    REQUIRE(delete_probe.value == INVALID_HANDLE_VALUE);
    REQUIRE(delete_error == ERROR_SHARING_VIOLATION);
    unsigned calls = 0, waits = 0;
    DWORD replacement_error = ERROR_SUCCESS;
    bool sharing_verified = false;
    const auto saved = WriteFrozenPlanWithRetry(target, frozen,
        [&](const auto& path, auto bytes, auto durability) -> PlanWriteResult {
            ++calls; CHECK(path == target); CHECK(bytes == frozen);
            CHECK(durability == WriteDurability::ProcessCrashDurability);
            auto result = AtomicWriteFile(path, bytes, durability);
            if (calls == 1) {
                const auto diagnostic = result ? std::string("unexpected success") : result.error().message;
                INFO(diagnostic);
                REQUIRE_FALSE(result.has_value());
                REQUIRE(result.error().code == "atomic.replace_failed");
                REQUIRE(result.error().failure_kind == WriteFailureKind::TransientReject);
                REQUIRE(result.error().outcome == WriteOutcome::NotCommitted);
                const std::string prefix = "原子替换失败: 原子替换文件失败，Windows 错误码 ";
                const auto access_denied = prefix + std::to_string(ERROR_ACCESS_DENIED);
                const auto sharing_denied = prefix + std::to_string(ERROR_SHARING_VIOLATION);
                REQUIRE((result.error().message == access_denied || result.error().message == sharing_denied));
                replacement_error = result.error().message == access_denied ? ERROR_ACCESS_DENIED : ERROR_SHARING_VIOLATION;
                REQUIRE(ReadAll(target) == "old plan");
                REQUIRE(TempLeftovers(root.path).empty());
                sharing_verified = true;
            }
            return result;
        }, [] { return PlanClock::now(); }, [&](PlanClock::duration duration) {
            REQUIRE(sharing_verified); REQUIRE(held.value != INVALID_HANDLE_VALUE);
            REQUIRE(duration > PlanClock::duration::zero());
            REQUIRE(duration <= std::chrono::milliseconds(20));
            REQUIRE(held.Close()); ++waits;
        });
    REQUIRE(saved.has_value()); REQUIRE(saved->outcome == WriteOutcome::CommittedDurable);
    REQUIRE(calls == 2); REQUIRE(waits == 1);
    REQUIRE(held.value == INVALID_HANDLE_VALUE);
    REQUIRE(ReadAll(target) == frozen); REQUIRE(TempLeftovers(root.path).empty());
    std::cout << "[sdk-plan-native-error] delete-open=" << delete_error << " replace=" << replacement_error << '\n';
    std::cout << "[sdk-plan-retry] windows-sharing-recovery\n";
#else
    const auto saved = lubancore::detail::WriteFrozenPlan(target, frozen);
    REQUIRE(saved.has_value()); REQUIRE(saved->outcome == WriteOutcome::CommittedDurable);
    REQUIRE(ReadAll(target) == frozen); REQUIRE(TempLeftovers(root.path).empty());
#endif
    std::cout << "[sdk-plan-retry] native-write\n";
}
