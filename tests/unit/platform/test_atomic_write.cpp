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
        const std::string name = entry.path().filename().string();
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
