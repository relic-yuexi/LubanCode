// T11-B / T11-C / V3-GAP-06(Session v3 旧设计清理单):审批档与环境快照。
//
// T11-B 断点:v3 场审批档只在内存生效(session.v3_approval_mode_memory_
// only),切档无账、无来源/策略版本;v2 源 resume 直接继承旧 manifest 档位
// ——旧历史的更高权限被原样恢复(静默提权)。
//   本册钉:launch 基线事实、切档事实(user_toggle/oldMode/policyVersion)、
//   恢复重算(有效档 = 源场档与当前策略较严者;变更后崩溃/策略收紧不提权)。
//
// T11-C 断点:v3 场 CaptureEnvironment 回 no_recorder,环境事实整场不落。
//   本册钉:session.environment.captured 落账(快照 blob 引用 + 重现等级 +
//   取材缺口 + 脱敏声明)、秘密 canary 不入档、没采集 = unavailable、
//   旧场恢复不拿今天环境补昨天事实(源场字节不动,新场重采自己的)。
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "approval_mode.hpp"
#include "app/session_stack.hpp"  // BuildRedactedConfigSnapshot(脱敏合同)
#include "config/config.hpp"
#include "platform/paths.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/identity.hpp"

namespace platform = lubancode::platform;
using namespace lubancode;
using lubancode::runtime::TrajectorySessionLedger;

namespace {

namespace fs = std::filesystem;

struct EnvGuard {
    explicit EnvGuard(const char* name, const char* value) : name_(name) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=" + value).c_str());
#else
        setenv(name_, value, 1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    const char* name_;
};

fs::path FreshRoot(const char* tag) {
    // Windows MAX_PATH:CI 临时根本身就近 40 字符,目录名必须短——长名 +
    // 派生 workspace key + blob 哈希文件名会顶穿 260(2026-09-16 CI 实锤:
    // blob 临时文件打不开,路径实长 262)。用"t11ae-<tag>"短前缀。
    const auto dir = fs::temp_directory_path() / ("t11ae-" + std::string(tag));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

TrajectorySessionLedger::Options LedgerOptions(const fs::path& root,
                                               ApprovalMode launch_mode = ApprovalMode::Default) {
    TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "repo";
    options.workspace_identity = lubancode::workspace::MakeFallbackIdentity(root / "repo");
    options.launch_cwd = "D:/tmp/repo";
    options.lubancode_version = "0.26.269-test";
    options.v3_system_content = "你是 LubanCode。";
    options.approval_mode = launch_mode;
    return options;
}

fs::path V3StreamOf(const TrajectorySessionLedger& ledger) {
    return ledger.session_dir() /
           platform::Utf8ToPath(platform::PathToUtf8(ledger.session_dir().filename()) + ".jsonl");
}

std::vector<nlohmann::json> ReadLines(const fs::path& stream) {
    std::vector<nlohmann::json> rows;
    std::ifstream file(stream, std::ios::binary);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        rows.push_back(nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false));
        REQUIRE_FALSE(rows.back().is_discarded());
    }
    return rows;
}

std::vector<const nlohmann::json*> RowsOfKind(const std::vector<nlohmann::json>& rows,
                                              const char* kind) {
    std::vector<const nlohmann::json*> out;
    for (const auto& row : rows) {
        if (row.value("kind", std::string()) == kind) {
            out.push_back(&row);
        }
    }
    return out;
}

std::string ReadFileText(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// 目录下全部文件文本拼起来(canary 扫描用,含 blob)。
std::string ConcatDirText(const fs::path& dir) {
    std::string all;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        std::error_code ec;
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        all += ReadFileText(entry.path());
    }
    return all;
}

}  // namespace

// ---------------------------------------------------------------------------
// T11-B 审批档
// ---------------------------------------------------------------------------

TEST_CASE("T11-B 切档: v3 场落 approval.mode.applied,来源与策略版本齐") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("toggle");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root, ApprovalMode::Default));
    REQUIRE(ledger.has_value());
    const fs::path stream = V3StreamOf(*ledger);

    // 建场基线:launch 事实先落(起手 default)。
    {
        const auto rows = ReadLines(stream);
        const auto baseline = RowsOfKind(rows, "approval.mode.applied");
        REQUIRE(baseline.size() == 1);
        CHECK(baseline[0]->at("payload").value("mode", std::string()) == "default");
        CHECK(baseline[0]->at("payload").value("source", std::string()) == "launch");
        CHECK(baseline[0]->at("payload").value("policyVersion", std::string()) ==
              std::string(kApprovalPolicyVersion));
    }
    // 用户切档:写账成功(不再是 memory_only 稳定码),来源 user_toggle。
    CHECK(ledger->UpdateApprovalMode(ApprovalMode::Yolo).empty());
    {
        const auto rows = ReadLines(stream);
        const auto facts = RowsOfKind(rows, "approval.mode.applied");
        REQUIRE(facts.size() == 2);
        CHECK(facts[1]->at("payload").value("mode", std::string()) == "yolo");
        CHECK(facts[1]->at("payload").value("source", std::string()) == "user_toggle");
        CHECK(facts[1]->at("payload").value("oldMode", std::string()) == "default");
        CHECK(facts[1]->at("payload").value("policyVersion", std::string()) ==
              std::string(kApprovalPolicyVersion));
    }
    // 读面:最后一枚档位事实是 yolo(事实,不是裁决)。
    const auto fact = lubancode::trajectory::v3::FindLastApprovalMode(
        *lubancode::trajectory::v3::ReadV3Ledger(stream));
    REQUIRE(fact.has_value());
    CHECK(fact->mode == "yolo");
    CHECK(fact->source == "user_toggle");
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);
}

TEST_CASE("T11-B 恢复重算: 变更后崩溃 + 策略收紧,不静默提权") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("clamp");
    // 进程 1:default 起手,用户切到 yolo,场子封口(变更后崩溃的账面形状)。
    std::string source_id;
    fs::path source_stream;
    {
        auto first = TrajectorySessionLedger::Open(LedgerOptions(root, ApprovalMode::Default));
        REQUIRE(first.has_value());
        source_id = first->session_id();
        source_stream = V3StreamOf(*first);
        CHECK(first->UpdateApprovalMode(ApprovalMode::Yolo).empty());
        const auto closed = first->CloseSession("exit");
        CHECK(closed.error_code.empty());
    }
    const std::string source_bytes = ReadFileText(source_stream);

    // 进程 2:策略收紧(起手 default),恢复源场——旧历史的 yolo 不许回来。
    {
        auto second = TrajectorySessionLedger::Open(LedgerOptions(root, ApprovalMode::Default));
        REQUIRE(second.has_value());
        const auto summary = second->ResumeInteractive(source_id);
        REQUIRE(summary.outcome.error_code.empty());
        // 重算后的有效档:较严者(default),不是源场的 yolo。
        REQUIRE(summary.outcome.approval_mode.has_value());
        CHECK(*summary.outcome.approval_mode == ApprovalMode::Default);
        // 新账记录重算事实:oldMode 如实带源场原始档,mode 是重算结果。
        const auto rows = ReadLines(V3StreamOf(*second));
        const auto facts = RowsOfKind(rows, "approval.mode.applied");
        REQUIRE(facts.size() >= 2);  // launch 基线 + resume_recomputed
        const auto& recomputed = *facts.back();
        CHECK(recomputed.at("payload").value("source", std::string()) == "resume_recomputed");
        CHECK(recomputed.at("payload").value("mode", std::string()) == "default");
        CHECK(recomputed.at("payload").value("oldMode", std::string()) == "yolo");
        CHECK(lubancode::trajectory::v3::VerifyV3File(V3StreamOf(*second)).ok);
    }
    // 源场账一个字节不动。
    CHECK(ReadFileText(source_stream) == source_bytes);

    // 反向:当前策略更宽(launch=yolo),源场 default——按当前策略走
    //(重算不降格,也不从旧历史提权)。
    {
        auto third = TrajectorySessionLedger::Open(LedgerOptions(root, ApprovalMode::Yolo));
        REQUIRE(third.has_value());
        // 先封掉上一进程留的场再 resume(一账本一活场)。
        const auto summary = third->ResumeInteractive(source_id);
        REQUIRE(summary.outcome.error_code.empty());
        REQUIRE(summary.outcome.approval_mode.has_value());
        CHECK(*summary.outcome.approval_mode == ApprovalMode::Yolo);
    }
}

// ---------------------------------------------------------------------------
// T11-C 环境快照
// ---------------------------------------------------------------------------

TEST_CASE("T11-C 采集: session.environment.captured 落账,canary 不入档") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("capture");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const fs::path stream = V3StreamOf(*ledger);

    // 配置里埋 canary(inline api_key),脱敏快照走生产同一只
    // BuildRedactedConfigSnapshot——窄键集只放行 language/features/超时,
    // 密钥一概不进。
    constexpr const char* kCanary = "sk-canary-T11C-do-not-leak";
    config::Config config;
    config::ProviderConfig provider;
    provider.name = "moonshot";
    provider.auth = config::ProviderAuthMode::Inline;
    provider.api_key = kCanary;
    config.providers.push_back(provider);

    TrajectorySessionLedger::EnvironmentFacts facts;
    facts.provider = "moonshot";
    facts.wire = "anthropic";
    facts.model = "kimi-k2.6";
    facts.system_prompt = "SYSTEM-X";
    facts.config_snapshot_redacted = app::BuildRedactedConfigSnapshot(config);
    CHECK(ledger->CaptureEnvironment(facts).empty());

    const auto rows = ReadLines(stream);
    const auto captured = RowsOfKind(rows, "session.environment.captured");
    REQUIRE(captured.size() == 1);
    CHECK(captured[0]->at("payload").value("replayLevel", std::string()) == "input_only");
    CHECK(captured[0]->at("payload").value("configRedacted", false) == true);
    const auto& gaps = captured[0]->at("payload").at("gaps");
    CHECK(gaps.is_array());
    CHECK_FALSE(gaps.empty());  // 非 git 仓:取材缺口如实记
    CHECK(captured[0]->at("payload").contains("snapshotRef"));
    // 幂等:一场 run 一次。
    CHECK(ledger->CaptureEnvironment(facts).empty());
    const auto again_rows = ReadLines(stream);
    CHECK(RowsOfKind(again_rows, "session.environment.captured").size() == 1);
    // canary 不入账、不入 blob、不入任何会话文件。
    CHECK(ConcatDirText(ledger->session_dir()).find(kCanary) == std::string::npos);
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);

    // 读面:采集事实可读;没采集的场 unavailable。
    const auto fact = lubancode::trajectory::v3::FindLastEnvironmentCapture(
        *lubancode::trajectory::v3::ReadV3Ledger(stream));
    REQUIRE(fact.has_value());
    CHECK(fact->replay_level == "input_only");
}

TEST_CASE("T11-C 恢复: 旧场环境事实不动,新场重采自己的") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("resume");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::string source_id = ledger->session_id();
    const fs::path source_stream = V3StreamOf(*ledger);

    TrajectorySessionLedger::EnvironmentFacts facts;
    facts.provider = "moonshot";
    facts.wire = "anthropic";
    facts.model = "kimi-k2.6";
    CHECK(ledger->CaptureEnvironment(facts).empty());
    // 先封口再定格字节:resume 的换场事务对当前场落的 session.ended 是
    // 合法封口,不是改写;封口后源场一个字节不许再动。
    const auto closed = ledger->CloseSession("exit");
    CHECK(closed.error_code.empty());
    const std::string source_bytes = ReadFileText(source_stream);
    const auto source_capture = lubancode::trajectory::v3::FindLastEnvironmentCapture(
        *lubancode::trajectory::v3::ReadV3Ledger(source_stream));
    REQUIRE(source_capture.has_value());

    // 恢复换场:昨天的事实留在旧场(字节不动);新场没采集前 unavailable,
    // 采集后是新场自己今天的快照。恢复走第二只账本(生产形状:恢复总从
    // 活场出发,封口后的源场由新进程接管)。
    auto second = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(second.has_value());
    const auto summary = second->ResumeInteractive(source_id);
    REQUIRE(summary.outcome.error_code.empty());
    CHECK(ReadFileText(source_stream) == source_bytes);
    const auto new_stream = V3StreamOf(*second);
    CHECK_FALSE(lubancode::trajectory::v3::FindLastEnvironmentCapture(
                    *lubancode::trajectory::v3::ReadV3Ledger(new_stream))
                    .has_value());  // 没采集就 unavailable,不拿源场的补
    facts.model = "kimi-k2.6-today";
    CHECK(second->CaptureEnvironment(facts).empty());
    const auto new_capture = lubancode::trajectory::v3::FindLastEnvironmentCapture(
        *lubancode::trajectory::v3::ReadV3Ledger(new_stream));
    REQUIRE(new_capture.has_value());
    CHECK(new_capture->event_id != source_capture->event_id);
    CHECK(lubancode::trajectory::v3::VerifyV3File(new_stream).ok);
}
