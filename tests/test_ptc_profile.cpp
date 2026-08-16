// PTC 能力画像单测:四档状态、指纹成分、auto 门槛(规格"基准"节)、
// 熔断器计数规则、画像存档读写、config 的 tool_calling/ptc 段解析合并。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "config/config.hpp"
#include "platform/paths.hpp"
#include "ptc/profile.hpp"
#include "ptc/runner.hpp"

using namespace lubancode::ptc;

namespace {

// 临时存档路径(测试结束即删)。
struct TempStore {
    std::filesystem::path path;
    TempStore() {
        path = std::filesystem::temp_directory_path() /
               ("lubancode-ptc-profiles-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    }
    ~TempStore() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

PtcRunResult MakeRun(PtcFailure failure, int call_count, bool has_emit) {
    PtcRunResult run;
    run.failure = failure;
    run.ok = failure == PtcFailure::None;
    for (int i = 0; i < call_count; ++i) {
        PtcCallRecord record;
        record.tool = "read_file";
        record.ok = true;
        run.calls.push_back(record);
    }
    if (has_emit) {
        run.emit_value = nlohmann::json{{"a", 1}};
    }
    return run;
}

}  // namespace

TEST_CASE("状态四档: ToString/ParseStatus 往返,认不得的值拒") {
    CHECK(ToString(PtcStatus::Unsupported) == "unsupported");
    CHECK(ToString(PtcStatus::Unknown) == "unknown");
    CHECK(ToString(PtcStatus::Experimental) == "experimental");
    CHECK(ToString(PtcStatus::Verified) == "verified");
    for (const auto* text : {"unsupported", "unknown", "experimental", "verified"}) {
        REQUIRE(ParseStatus(text).has_value());
        CHECK(ToString(*ParseStatus(text)) == text);
    }
    CHECK_FALSE(ParseStatus("yes").has_value());
    CHECK_FALSE(ParseStatus("").has_value());
}

TEST_CASE("指纹: 任一成分变,指纹变;同成分同指纹") {
    const std::string base = BuildPtcFingerprint("prov", "http://e", "model-a", "anthropic", "3.11.8", "ptc-v1");
    CHECK(base == BuildPtcFingerprint("prov", "http://e", "model-a", "anthropic", "3.11.8", "ptc-v1"));
    CHECK(base != BuildPtcFingerprint("prov2", "http://e", "model-a", "anthropic", "3.11.8", "ptc-v1"));
    CHECK(base != BuildPtcFingerprint("prov", "http://e2", "model-a", "anthropic", "3.11.8", "ptc-v1"));
    CHECK(base != BuildPtcFingerprint("prov", "http://e", "model-b", "anthropic", "3.11.8", "ptc-v1"));
    CHECK(base != BuildPtcFingerprint("prov", "http://e", "model-a", "responses", "3.11.8", "ptc-v1"));
    CHECK(base != BuildPtcFingerprint("prov", "http://e", "model-a", "anthropic", "3.12.1", "ptc-v1"));
    CHECK(base != BuildPtcFingerprint("prov", "http://e", "model-a", "anthropic", "3.11.8", "ptc-v2"));
}

TEST_CASE("五条硬条件: 少一条就 AllMet 假,FailureTexts 点名") {
    PtcHardConditions all;
    all.sandbox_reliable = true;
    all.python_version_ok = true;
    CHECK(all.AllMet());
    CHECK(all.FailureTexts().empty());

    PtcHardConditions none;
    CHECK_FALSE(none.AllMet());
    const auto failures = none.FailureTexts();
    REQUIRE(failures.size() == 2);  // 默认形状只缺 sandbox 与 python 两条
    CHECK(failures[0].find("沙箱") != std::string::npos);
    CHECK(failures[1].find("Python") != std::string::npos);
}

TEST_CASE("auto 门槛: verified + 硬条件 + (链>=4 或 fanout>=8) 才选 ptc") {
    PtcAutoGates gates;
    gates.profile_status = PtcStatus::Verified;
    gates.hard_conditions_met = true;
    // 短任务:不划算,走 json。
    gates.estimated_chain_depth = 1;
    gates.estimated_fanout = 2;
    CHECK(ResolveToolCalling(gates) == ToolCallingDecision::Json);
    // 长链。
    gates.estimated_chain_depth = 4;
    CHECK(ResolveToolCalling(gates) == ToolCallingDecision::Programmatic);
    gates.estimated_chain_depth = 1;
    gates.estimated_fanout = 8;
    CHECK(ResolveToolCalling(gates) == ToolCallingDecision::Programmatic);
    gates.estimated_fanout = 7;
    CHECK(ResolveToolCalling(gates) == ToolCallingDecision::Json);
    // 硬条件不齐:再长也不选。
    gates.estimated_fanout = 64;
    gates.hard_conditions_met = false;
    CHECK(ResolveToolCalling(gates) == ToolCallingDecision::Json);
    // 厂商说支持(hint)但本机没跑过探针(unknown/experimental):不得自动启用。
    gates.hard_conditions_met = true;
    gates.profile_status = PtcStatus::Experimental;
    CHECK(ResolveToolCalling(gates) == ToolCallingDecision::Json);
    gates.profile_status = PtcStatus::Unknown;
    CHECK(ResolveToolCalling(gates) == ToolCallingDecision::Json);
    CHECK(ToString(ToolCallingDecision::Programmatic) == "ptc");
    CHECK(ToString(ToolCallingDecision::Json) == "json");
}

TEST_CASE("熔断器: 语法错/RPC 错计数,工具失败不算,成功清零,3 次触发") {
    PtcCircuitBreaker breaker(3);
    CHECK_FALSE(breaker.Tripped());
    breaker.Record(MakeRun(PtcFailure::Syntax, 0, false));
    CHECK(breaker.consecutive_faults() == 1);
    // 工具层失败:不算熔断证据。
    breaker.Record(MakeRun(PtcFailure::Runtime, 3, true));
    CHECK(breaker.consecutive_faults() == 1);
    breaker.Record(MakeRun(PtcFailure::Rpc, 1, false));
    CHECK(breaker.consecutive_faults() == 2);
    // 成功清零。
    breaker.Record(MakeRun(PtcFailure::None, 2, true));
    CHECK(breaker.consecutive_faults() == 0);
    CHECK_FALSE(breaker.Tripped());
    // 连三把:触发。
    breaker.Record(MakeRun(PtcFailure::Syntax, 0, false));
    breaker.Record(MakeRun(PtcFailure::Protocol, 0, false));
    breaker.Record(MakeRun(PtcFailure::Rpc, 0, false));
    CHECK(breaker.Tripped());
    CHECK(breaker.Reason().find("3 次") != std::string::npos);
    CHECK(breaker.Reason().find("RPC 协议错") != std::string::npos);
    // 触发后不再升回:后续成功也不复位。
    breaker.Record(MakeRun(PtcFailure::None, 1, true));
    CHECK(breaker.Tripped());
    // 撞墙/取消不动计数。
    PtcCircuitBreaker walls(3);
    walls.Record(MakeRun(PtcFailure::LimitWallClock, 0, false));
    walls.Record(MakeRun(PtcFailure::Cancelled, 1, false));
    walls.Record(MakeRun(PtcFailure::Sandbox, 0, false));
    CHECK(walls.consecutive_faults() == 0);
    CHECK_FALSE(walls.Tripped());
}

TEST_CASE("画像存档: 写/读/查往返,坏文件当空,别家指纹不动") {
    TempStore temp;
    PtcProfileStore store(temp.path.string());
    CHECK(store.Load().empty());  // 文件不存在 = 空

    PtcProfile profile;
    profile.fingerprint = "fp-a";
    profile.status = PtcStatus::Experimental;
    profile.single_call_accuracy = 1.0;
    profile.max_verified_chain = 20;
    profile.verified_at = "2026-08-16";
    profile.harness_revision = kPtcHarnessRevision;
    std::string error;
    REQUIRE(store.Save(profile, &error));
    CHECK(error.empty());

    PtcProfile other;
    other.fingerprint = "fp-b";
    other.status = PtcStatus::Unknown;
    REQUIRE(store.Save(other, &error));

    const auto found = store.Find("fp-a");
    REQUIRE(found.has_value());
    CHECK(found->status == PtcStatus::Experimental);
    CHECK(found->single_call_accuracy == 1.0);
    CHECK(found->max_verified_chain == 20);
    CHECK(found->harness_revision == kPtcHarnessRevision);
    CHECK(store.Find("fp-b").has_value());
    CHECK_FALSE(store.Find("fp-missing").has_value());
    CHECK(store.Load().size() == 2);

    // 坏文件:Load 当空,不抛。
    {
        std::ofstream out(lubancode::platform::Utf8ToWide(temp.path.string()), std::ios::binary | std::ios::trunc);
        out << "not json at all";
    }
    PtcProfileStore broken(temp.path.string());
    CHECK(broken.Load().empty());
}

TEST_CASE("config: tool_calling 三档解析,认不得报错") {
    using lubancode::config::ParseToolCallingMode;
    using lubancode::config::ToolCallingMode;
    REQUIRE(ParseToolCallingMode("json").has_value());
    CHECK(*ParseToolCallingMode("json") == ToolCallingMode::Json);
    REQUIRE(ParseToolCallingMode("programmatic").has_value());
    CHECK(*ParseToolCallingMode("programmatic") == ToolCallingMode::Programmatic);
    REQUIRE(ParseToolCallingMode("auto").has_value());
    CHECK(*ParseToolCallingMode("auto") == ToolCallingMode::Auto);
    const auto bad = ParseToolCallingMode("ptc");
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().find("json / programmatic / auto") != std::string::npos);
    CHECK(lubancode::config::ToString(ToolCallingMode::Auto) == "auto");
}

TEST_CASE("config: ptc 段解析 + 合并(项目级压全局,整段回退)") {
    using namespace lubancode::config;
    const auto project = ParseFileConfigJson(R"({
        "tool_calling": "programmatic",
        "ptc": {"python": "py3.12", "wall_clock_ms": 45000, "max_calls": 50, "tools": ["read_file"]}
    })", "project.json");
    REQUIRE(project.has_value());
    CHECK(project->tool_calling == "programmatic");
    REQUIRE(project->ptc.has_value());
    CHECK(project->ptc->python == "py3.12");
    CHECK(project->ptc->wall_clock_ms.value_or(0) == 45000);
    REQUIRE(project->ptc->max_calls.has_value());
    CHECK(*project->ptc->max_calls == 50);
    REQUIRE(project->ptc->tools.has_value());
    CHECK(project->ptc->tools->size() == 1);

    const auto global = ParseFileConfigJson(R"({
        "tool_calling": "json",
        "ptc": {"cpu_ms": 9000, "max_concurrency": 4}
    })", "global.json");
    REQUIRE(global.has_value());

    const auto merged = MergeConfig(LubancodeEnvValues{}, std::optional<FileConfig>{project.value()},
                                    std::optional<FileConfig>{global.value()}, GenericEnvValues{});
    REQUIRE(merged.has_value());
    CHECK(merged->config.tool_calling == ToolCallingMode::Programmatic);
    CHECK(merged->sources.tool_calling == Source::ProjectConfigFile);
    // ptc 段整段回退:项目级那整段生效,全局的 cpu_ms/max_concurrency 不混入。
    CHECK(merged->config.ptc.python == "py3.12");
    CHECK(merged->config.ptc.wall_clock_ms == 45000);
    CHECK(merged->config.ptc.max_calls == 50);
    CHECK(merged->config.ptc.cpu_ms == 20000);       // 默认值(全局段没落到这里)
    CHECK(merged->config.ptc.max_concurrency == 8);  // 默认值
    CHECK(merged->sources.ptc == Source::ProjectConfigFile);

    // 只有全局段:全局生效。
    const auto merged_global = MergeConfig(LubancodeEnvValues{}, std::optional<FileConfig>{},
                                           std::optional<FileConfig>{global.value()}, GenericEnvValues{});
    REQUIRE(merged_global.has_value());
    CHECK(merged_global->config.tool_calling == ToolCallingMode::Json);
    CHECK(merged_global->sources.tool_calling == Source::GlobalConfigFile);
    CHECK(merged_global->config.ptc.cpu_ms == 9000);
    CHECK(merged_global->config.ptc.max_concurrency == 4);
    CHECK(merged_global->config.ptc.wall_clock_ms == 30000);  // 默认

    // 都没有:默认 json + 默认限额。
    const auto merged_none = MergeConfig(LubancodeEnvValues{}, std::optional<FileConfig>{},
                                         std::optional<FileConfig>{}, GenericEnvValues{});
    REQUIRE(merged_none.has_value());
    CHECK(merged_none->config.tool_calling == ToolCallingMode::Json);
    CHECK(merged_none->config.ptc.wall_clock_ms == 30000);
    CHECK(merged_none->config.ptc.max_calls == 100);
}

TEST_CASE("config: 坏的 tool_calling/ptc 字段报可读错误") {
    using namespace lubancode::config;
    const auto bad_mode = ParseFileConfigJson(R"({"tool_calling": "yaml"})", "x.json");
    REQUIRE_FALSE(bad_mode.has_value());
    CHECK(bad_mode.error().find("tool_calling") != std::string::npos);
    const auto bad_limit = ParseFileConfigJson(R"({"ptc": {"max_calls": 0}})", "x.json");
    REQUIRE_FALSE(bad_limit.has_value());
    CHECK(bad_limit.error().find("ptc.max_calls") != std::string::npos);
    const auto bad_type = ParseFileConfigJson(R"({"ptc": {"python": 123}})", "x.json");
    REQUIRE_FALSE(bad_type.has_value());
    CHECK(bad_type.error().find("ptc.python") != std::string::npos);
}
