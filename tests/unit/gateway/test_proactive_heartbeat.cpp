// 主动智能巡检(Proactive Heartbeat)单测与总装册:
//   1) HEARTBEAT_OK 语义静默判定与双层去重状态机(含恢复后同文复发重报);
//   2) watch_paths 边界校验、确定性内容指纹探针与 HEARTBEAT.md 清单装载;
//   3) CLI --watch / --clear-watch 解析闸门;
//   4) GatewayAutomationPump 端到端:HEARTBEAT.md 注入 + 首拍 HEARTBEAT_OK 静默 +
//      未变化零 Token 短路跳过 + 重启后文件变化唤醒告警投递。
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "app/cli_options.hpp"
#include "gateway/automation_store.hpp"
#include "gateway/heartbeat.hpp"
#include "gateway/profile.hpp"
#include "gateway/reply_outbox.hpp"
#include "platform/paths.hpp"
#include "runtime/automation_pump.hpp"
#include "tools/path_utils.hpp"
#include "tools/registry.hpp"
#include "workspace/identity.hpp"

using namespace lubancode;

namespace {

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

void WriteUtf8File(const std::filesystem::path& path, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
}

void CountCall(const std::filesystem::path& counter, const std::string& who) {
    std::error_code ec;
    std::filesystem::create_directories(counter.parent_path(), ec);
    std::ofstream stream(counter, std::ios::binary | std::ios::app);
    stream << who << "\n";
}

std::size_t CountOf(const std::filesystem::path& counter, const std::string& who) {
    std::ifstream stream(counter, std::ios::binary);
    std::string line;
    std::size_t count = 0;
    while (std::getline(stream, line)) {
        if (line == who) ++count;
    }
    return count;
}

class InspectingScriptBackend final : public api::Backend {
public:
    InspectingScriptBackend(std::filesystem::path counter,
                            std::vector<std::vector<api::StreamEvent>> scripts,
                            std::vector<std::string>* seen_user_texts = nullptr)
        : counter_(std::move(counter)),
          scripts_(std::move(scripts)),
          seen_user_texts_(seen_user_texts) {}

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        if (cancel != nullptr && cancel->load()) {
            return std::unexpected(api::Error{api::ErrorKind::Cancelled, "cancelled"});
        }
        CountCall(counter_, "model");
        if (seen_user_texts_ != nullptr) {
            std::string combined;
            for (const auto& msg : request.messages) {
                for (const auto& block : msg.content) {
                    if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                        combined += text->text;
                        combined.push_back('\n');
                    }
                }
            }
            seen_user_texts_->push_back(std::move(combined));
        }
        if (calls_ >= scripts_.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "script exhausted"});
        }
        for (const auto& event : scripts_[calls_]) {
            on_event(event);
        }
        ++calls_;
        return {};
    }

private:
    std::filesystem::path counter_;
    std::vector<std::vector<api::StreamEvent>> scripts_;
    std::vector<std::string>* seen_user_texts_ = nullptr;
    std::size_t calls_ = 0;
};

std::vector<api::StreamEvent> TextScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "test-model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

}  // namespace

TEST_CASE("heartbeat 语义静默:HEARTBEAT_OK 词元识别与恢复后复发重报") {
    CHECK(gateway::IsHeartbeatSilentReply("HEARTBEAT_OK"));
    CHECK(gateway::IsHeartbeatSilentReply("  \n\tHEARTBEAT_OK\r\n "));
    CHECK(gateway::IsHeartbeatSilentReply("`HEARTBEAT_OK`"));
    CHECK(gateway::IsHeartbeatSilentReply("```text\nHEARTBEAT_OK\n```"));

    // 夹带任何业务文字一律宁投勿吞。
    CHECK_FALSE(gateway::IsHeartbeatSilentReply(""));
    CHECK_FALSE(gateway::IsHeartbeatSilentReply("HEARTBEAT_OK 但磁盘满了"));
    CHECK_FALSE(gateway::IsHeartbeatSilentReply("一切正常\nHEARTBEAT_OK"));
    CHECK_FALSE(gateway::IsHeartbeatSilentReply("HEARTBEAT_OK."));

    // 状态机:首拍 HEARTBEAT_OK 即静默 -> 告警投递 -> 同文抑制 -> 恢复 HEARTBEAT_OK -> 同文复发重报。
    const auto d1 = gateway::EvaluateHeartbeatReply("HEARTBEAT_OK\n", "");
    CHECK(d1.suppressed());
    CHECK(d1.suppress == gateway::HeartbeatSuppressReason::SilentToken);
    CHECK(d1.settle_detail == "heartbeat_ok_suppressed");
    CHECK_FALSE(d1.observed_sha.empty());
    CHECK_FALSE(d1.update_last_observed_on_suppress);

    const auto d2 = gateway::EvaluateHeartbeatReply("告警:CI 红了", "");
    CHECK_FALSE(d2.suppressed());
    const std::string alert_sha = d2.observed_sha;
    REQUIRE_FALSE(alert_sha.empty());

    const auto d3 = gateway::EvaluateHeartbeatReply("告警:CI 红了", alert_sha);
    CHECK(d3.suppressed());
    CHECK(d3.suppress == gateway::HeartbeatSuppressReason::UnchangedHash);
    CHECK(d3.settle_detail == "unchanged_notification_suppressed");
    CHECK_FALSE(d3.update_last_observed_on_suppress);

    const auto d4 = gateway::EvaluateHeartbeatReply("HEARTBEAT_OK", alert_sha);
    CHECK(d4.suppressed());
    CHECK(d4.suppress == gateway::HeartbeatSuppressReason::SilentToken);
    CHECK(d4.update_last_observed_on_suppress);  // 推进水位以解锁后续同文复发告警
    const std::string recovered_sha = d4.observed_sha;

    const auto d5 = gateway::EvaluateHeartbeatReply("告警:CI 红了", recovered_sha);
    CHECK_FALSE(d5.suppressed());
}

TEST_CASE("watch_paths 边界校验、目录/文件内容探针与 HEARTBEAT.md 优先级") {
    std::vector<std::string> normalized;
    CHECK(gateway::ValidateWatchPaths({"src\\gateway\\heartbeat.cpp", "status.txt", "status.txt"},
                                      &normalized)
              .empty());
    REQUIRE(normalized.size() == 2);
    CHECK(normalized[0] == "src/gateway/heartbeat.cpp");
    CHECK(normalized[1] == "status.txt");

    CHECK_FALSE(gateway::ValidateWatchPaths({""}).empty());
    CHECK_FALSE(gateway::ValidateWatchPaths({"/etc/passwd"}).empty());
    CHECK_FALSE(gateway::ValidateWatchPaths({"C:\\Windows\\System32"}).empty());
    CHECK_FALSE(gateway::ValidateWatchPaths({"sub/../../escape"}).empty());
    std::vector<std::string> too_many(gateway::kMaxHeartbeatWatchPaths + 1, "a.txt");
    for (std::size_t i = 0; i < too_many.size(); ++i) {
        too_many[i] = "f" + std::to_string(i) + ".txt";
    }
    CHECK_FALSE(gateway::ValidateWatchPaths(too_many).empty());

    const std::filesystem::path ws =
        std::filesystem::temp_directory_path() / "lubancode-proactive-hb-unit";
    std::error_code ec;
    std::filesystem::remove_all(ws, ec);
    std::filesystem::create_directories(ws, ec);

    const auto p0 = gateway::ProbeWorkspaceWatch(ws, {"status.txt"});
    CHECK(p0.missing_count == 1);
    WriteUtf8File(ws / "status.txt", "v1");
    const auto p1 = gateway::ProbeWorkspaceWatch(ws, {"status.txt"});
    CHECK(p1.existing_count == 1);
    CHECK(p1.fingerprint_sha != p0.fingerprint_sha);
    // 同一毫秒内改内容也能靠 SHA-256 检出变化。
    WriteUtf8File(ws / "status.txt", "v2");
    const auto p2 = gateway::ProbeWorkspaceWatch(ws, {"status.txt"});
    CHECK(p2.fingerprint_sha != p1.fingerprint_sha);

    // HEARTBEAT.md:仅含注释/标题判为无实质条目;.lubancode/HEARTBEAT.md 优先于根 HEARTBEAT.md。
    WriteUtf8File(ws / "HEARTBEAT.md", "# Heartbeat\n<!-- 空模板 -->\n- [ ]\n");
    const auto c_empty = gateway::LoadWorkspaceHeartbeatChecklist(ws);
    CHECK(c_empty.present);
    CHECK_FALSE(c_empty.has_actionable_items);
    CHECK(c_empty.relative_source == "HEARTBEAT.md");

    WriteUtf8File(ws / ".lubancode" / "HEARTBEAT.md", "# 巡检\n- 检查最近一次部署日志\n");
    const auto c_dot = gateway::LoadWorkspaceHeartbeatChecklist(ws);
    CHECK(c_dot.present);
    CHECK(c_dot.has_actionable_items);
    CHECK(c_dot.relative_source == ".lubancode/HEARTBEAT.md");
}

TEST_CASE("CLI 解析:gateway job --watch 与 --clear-watch 校验") {
    const auto ok_add = app::ParseCliArgs({"lubancode", "gateway", "job", "add", "巡检",
                                           "--every", "300", "--heartbeat",
                                           "--watch", "src\\gateway", "--watch", "status.txt"});
    CHECK(ok_add.action == app::CliAction::RunGateway);
    CHECK(ok_add.gateway.heartbeat);
    REQUIRE(ok_add.gateway.watch_paths.size() == 2);
    CHECK(ok_add.gateway.watch_paths[0] == "src/gateway");
    CHECK(ok_add.gateway.watch_paths[1] == "status.txt");

    const auto ok_clear = app::ParseCliArgs({"lubancode", "gateway", "job", "update", "job-1",
                                             "--rev", "2", "--clear-watch"});
    CHECK(ok_clear.action == app::CliAction::RunGateway);
    CHECK(ok_clear.gateway.clear_watch);

    const auto bad_escape = app::ParseCliArgs({"lubancode", "gateway", "job", "add", "巡检",
                                               "--watch", "../secret"});
    CHECK(bad_escape.action == app::CliAction::BadGateway);

    const auto bad_both = app::ParseCliArgs({"lubancode", "gateway", "job", "update", "job-1",
                                             "--rev", "2", "--watch", "a.txt", "--clear-watch"});
    CHECK(bad_both.action == app::CliAction::BadGateway);
}

TEST_CASE("GatewayAutomationPump 主动巡检闭环:HEARTBEAT.md + 零 Token 短路 + 变化唤醒告警") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "lubancode-proactive-pump-e2e";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    const std::filesystem::path ws_root = root / "workspace";
    std::filesystem::create_directories(ws_root, ec);
    WriteUtf8File(ws_root / ".lubancode" / "HEARTBEAT.md",
                  "# 常驻巡检清单\n- 检查 status.txt 是否出现 red 故障\n");
    WriteUtf8File(ws_root / "status.txt", "green");

    const auto paths = gateway::ResolveGatewayProfilePaths(root / "gateway", "default");
    const std::filesystem::path workspaces_root = root / "workspaces";
    const std::filesystem::path counter_file = root / "calls.log";
    std::vector<std::string> seen_prompts;

    auto make_pump_options = [&]() {
        runtime::GatewayAutomationPump::Options options;
        options.paths = paths;
        options.workspaces_root = workspaces_root;
        options.cwd_utf8 = tools::PathToUtf8(ws_root);
        options.lubancode_version = "0.26.283-test";
        options.wire_name = "test-wire";
        options.model = "test-model";
        options.workspace_identity = workspace::MakeFallbackIdentity(ws_root);
        return options;
    };

    // 阶段 1:建主动巡检任务 -> 拍 1 模型回 HEARTBEAT_OK(静默不入箱) -> 拍 2 文件未变(零模型调用短路)。
    {
        InspectingScriptBackend backend(counter_file, {TextScript("HEARTBEAT_OK")}, &seen_prompts);
        tools::ToolRegistry registry;
        runtime::GatewayAutomationPump pump;
        REQUIRE(runtime::GatewayAutomationPump::Open(&pump, backend, registry, make_pump_options())
                    .ok);
        pump.set_owner_epoch("hb-epoch-1");

        gateway::GatewayJobAddCommand add;
        add.job_id = "hb-watch-1";
        add.prompt = "巡检构建状态";
        add.idempotency_key = "idem-hb-watch-1";
        add.schedule.set_interval = true;
        add.schedule.interval_seconds = 60;
        add.schedule.set_notify_on_change = true;
        add.schedule.notify_on_change = true;
        add.schedule.set_watch_paths = true;
        add.schedule.watch_paths = {"status.txt"};
        REQUIRE(gateway::WriteJobAddCommand(paths.control_dir, add).empty());

        REQUIRE(pump.TickOnce(1000));    // 建 job
        REQUIRE(pump.TickOnce(61000));   // 拍 1:首次无水位 -> 跑模型 -> HEARTBEAT_OK 静默
        REQUIRE(pump.TickOnce(121000));  // 拍 2:status.txt 未变 -> 零 Token 短路跳过!

        // 盘上 job.prompt 原文不被包装词污染。
        const auto stored_job = pump.store()->FindJob("hb-watch-1");
        REQUIRE(stored_job.has_value());
        CHECK(stored_job->prompt == "巡检构建状态");
        CHECK(stored_job->watch_paths == std::vector<std::string>{"status.txt"});
        CHECK_FALSE(stored_job->last_watch_sha.empty());
    }

    CHECK(CountOf(counter_file, "model") == 1);  // 拍 2 真没调模型!
    REQUIRE(seen_prompts.size() == 1);
    CHECK(seen_prompts[0].find("检查 status.txt 是否出现 red 故障") != std::string::npos);
    CHECK(seen_prompts[0].find("HEARTBEAT_OK") != std::string::npos);
    CHECK(gateway::ReadOutboxProjection(paths.outbox_log).items.empty());

    // 阶段 2:模拟 Gateway 重启 + 监控文件发生变化 -> 拍 3 唤醒模型并投递告警 -> 拍 4 再次未变短路。
    WriteUtf8File(ws_root / "status.txt", "red: CI failed on main");
    {
        InspectingScriptBackend backend(
            counter_file, {TextScript("告警:status.txt 报告 CI 失败,请及时处理。")});
        tools::ToolRegistry registry;
        runtime::GatewayAutomationPump pump;
        REQUIRE(runtime::GatewayAutomationPump::Open(&pump, backend, registry, make_pump_options())
                    .ok);
        pump.set_owner_epoch("hb-epoch-2");

        REQUIRE(pump.TickOnce(181000));  // 拍 3:watch 变化 -> 调模型 -> 告警入箱投递!
        REQUIRE(pump.TickOnce(241000));  // 拍 4:新内容已定格且未再改 -> 再次零 Token 短路!
    }

    CHECK(CountOf(counter_file, "model") == 2);  // 四拍总共只调了 2 次模型!
    const auto outbox = gateway::ReadOutboxProjection(paths.outbox_log);
    REQUIRE(outbox.items.size() == 1);
    CHECK(outbox.items.begin()->second.state == "delivered");

    const auto projection = gateway::ReadAutomationProjection(paths.automation_log);
    REQUIRE(projection.occurrences.size() == 4);
    std::vector<gateway::AutomationOccurrence> by_slot;
    for (const auto& [id, occ] : projection.occurrences) {
        by_slot.push_back(occ);
    }
    std::sort(by_slot.begin(), by_slot.end(),
              [](const gateway::AutomationOccurrence& a, const gateway::AutomationOccurrence& b) {
                  return a.slot_ms < b.slot_ms;
              });
    CHECK(by_slot[0].detail == "heartbeat_ok_suppressed");
    CHECK_FALSE(by_slot[0].watch_skipped);
    CHECK(by_slot[1].detail == "watch_unchanged_skipped");
    CHECK(by_slot[1].watch_skipped);
    CHECK(by_slot[2].detail == "delivered");
    CHECK_FALSE(by_slot[2].watch_skipped);
    CHECK(by_slot[3].detail == "watch_unchanged_skipped");
    CHECK(by_slot[3].watch_skipped);
}
