// TEST-GAP-01(Session v3 真实会话审计单):真实编排集成用例——固定参数
// 的单元测试挡不住"生产状态随 usage 反馈变化"的病。本册用真 AgentLoop +
// 假后端(带 wire 序列化,即 adapter_budget/v3 主路)连跑多个用户轮:
//   工具大结果(超帽输出,UHURZ3 现场 2MB 目录清单的脱敏缩小) → 请求 →
//   usage 反馈 → 下一请求,断言三件事:
//   1. 大结果按预算预览进请求,原文不整份上 wire(V3-REAL-05 链路行为);
//   2. 相邻请求的旧消息逐字节稳定——无动态暗裁,前缀 append-only
//      (V3-REAL-01/03 在 v3 主路的行为面);
//   3. 在线校准不参与:挂一只系数 2.5 的 TokenCalibrator(预喂样本)与
//      干净校准器各跑一遍同场景,请求逐字节一致——v3 主路的裁剪/容量
//      不吃校准系数(loop 在 rewrite_tool_results_for_history 在场时钳
//      1.0);v2 旧路的校准行为册(test_loop)另有钉,不在此重复。
// 环境说明:本册不落 v3 jsonl(rewrite 钩子按生产桥同款手接,无 writer),
// 不消费 LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS;走的是 v3 行为路径
// (adapter_budget + 提交边界预览)。
#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <variant>
#include <vector>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "agent/token_calibrator.hpp"
#include "api/backend.hpp"
#include "api/chat/request.hpp"
#include "api/model_input_snapshot.hpp"
#include "api/types.hpp"
#include "hooks/middleware_builtins.hpp"
#include "runtime/tool_trajectory_sink.hpp"  // ToolResultsCommitReceipt:预览提交回执
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "trajectory/v3/result_store.hpp"

using namespace lubancode;

namespace {

class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::vector<api::Request> captured_requests;
    bool serialize_adapter_input = false;
    std::string SerializeForDiagnostics(const api::Request& request) const override {
        return serialize_adapter_input ? api::chat::BuildRequestJson(request).dump() : std::string();
    }

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
        captured_requests.push_back(request);
        const std::size_t idx = captured_requests.size() - 1;
        if (idx >= scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "FakeBackend: 脚本用完了", 0});
        }
        for (const auto& event : scripts[idx]) {
            on_event(event);
        }
        return {};
    }
};

class FixedResultTool : public tools::Tool {
public:
    FixedResultTool(std::string name, std::string content) : name_(std::move(name)), content_(std::move(content)) {}
    std::string name() const override { return name_; }
    std::string description() const override { return "read a big listing"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return false; }
    tools::Tool::Result execute(const nlohmann::json&) override {
        ++call_count;
        return {content_, false};
    }
    int call_count = 0;

private:
    std::string name_;
    std::string content_;
};

api::Usage UsageWithInput(std::int64_t input) {
    api::Usage usage;
    usage.input_tokens = input;
    usage.output_tokens = 32;
    return usage;
}

std::vector<api::StreamEvent> TextWithUsage(const std::string& text, std::int64_t input) {
    return {api::MessageStart{"msg", "model"},
            api::TextDelta{text},
            api::ContentBlockDone{0},
            api::MessageDone{"end_turn", UsageWithInput(input)}};
}

std::vector<api::StreamEvent> ToolCallWithUsage(const std::string& call_id, const std::string& tool,
                                                std::int64_t input) {
    return {api::MessageStart{"msg", "model"},
            api::ToolUseStart{0, call_id, tool},
            api::ToolUseInputDelta{0, "{}"},
            api::ContentBlockDone{0},
            api::MessageDone{"tool_use", UsageWithInput(input)}};
}

// 一只系数 2.5 的校准器:预喂两对样本(估算 10000/字节 40000/实报 25000,
// 比率 2.5 在硬带 [0.2,5.0] 内,两对即过 kMinSamples)——Coefficient()
// 返回 2.5,非 1.0。若 v3 主路任何裁剪/容量判定吃了这枚系数,第二遍
// (干净校准器)的请求就会与第一遍逐字节分叉。TokenCalibrator 带 mutex
// 不可移动,用出参填。
void MakeHotCalibrator(agent::TokenCalibrator& calibrator) {
    for (int i = 0; i < 2; ++i) {
        agent::TokenCalibrationSample sample;
        sample.request_bytes = 40000;
        sample.estimated_tokens = 10000;  // 过 kMinRequestEstimateTokens
        sample.reported_input_tokens = 25000;
        REQUIRE(calibrator.Record("fake-provider", "test-model", sample) ==
                agent::TokenCalibrator::RecordVerdict::Accepted);
    }
}

// 生产桥(V3ToolResultsCommitted)同款收尾:按 loop 派的 preview_budget_bytes
// 用 v3::BuildToolPreview 出预览,替换正文并置 preview_committed。
struct PreviewCommitHook {
    std::vector<std::string> adopted;
    runtime::ToolResultsCommitReceipt operator()(api::Message& batch) {
        for (auto& block : batch.content) {
            auto& result = std::get<api::ToolResultBlock>(block);
            if (result.content.size() <= result.preview_budget_bytes) {
                adopted.push_back(result.content);
                continue;  // 线内穿透:不硬套预览壳
            }
            trajectory::v3::PreviewRequest request;
            request.max_preview_bytes = result.preview_budget_bytes;
            trajectory::v3::PreviewChannel channel;
            channel.display_path = "artifacts/" + result.tool_use_id + ".combined.txt";
            channel.channel = "combined";
            channel.text = result.content;
            channel.output_bytes = result.content.size();
            request.channels.push_back(channel);
            const auto preview = trajectory::v3::BuildToolPreview(request);
            REQUIRE_FALSE(preview.preview_unrepresentable);
            result.content = preview.text;
            result.preview_committed = true;
            adopted.push_back(result.content);
        }
        return runtime::ToolResultsCommitReceipt{};
    }
};

// 跑一遍完整编排:两个用户轮,各带一次工具大结果 + usage 反馈,共四次
// 模型请求。返回假后端捕获的请求序列。
std::vector<api::Request> RunOrchestration(agent::TokenCalibrator* calibrator,
                                           std::vector<std::string>* adopted_previews) {
    FakeBackend backend;
    backend.serialize_adapter_input = true;
    const std::string big(200000, 'x');  // UHURZ3 现场 2MB 目录清单的缩小件
    backend.scripts = {
        ToolCallWithUsage("call-big-1", "run_list", 50000),   // 第一轮:要大结果
        TextWithUsage("第一轮读完了", 52000),                  // 第一轮:收尾
        ToolCallWithUsage("call-big-2", "run_list", 54000),   // 第二轮:再要一次
        TextWithUsage("第二轮读完了", 56000),                  // 第二轮:收尾
    };
    tools::ToolRegistry registry;
    auto tool = std::make_unique<FixedResultTool>("run_list", big);
    registry.Register(std::move(tool));

    agent::AgentProfile profile;
    profile.provider = "fake-provider";
    profile.request.model = "test-model";
    profile.system_prompt = "system";
    profile.runtime.context_window_tokens = 110000;
    profile.runtime.max_output_tokens = 4096;
    profile.runtime.max_output_tokens_source = agent::OutputBudgetSource::ConfigFile;
    agent::Agent loop(backend, registry, profile);
    agent::TurnWiring wiring;
    PreviewCommitHook hook;
    hook.calibrator = calibrator;
    wiring.rewrite_tool_results_for_history = [&hook](api::Message& batch) { return hook(batch); };
    wiring.token_calibrator = calibrator;

    REQUIRE(loop.Run("列一下目录", wiring).has_value());
    REQUIRE(loop.Run("再列一次", wiring).has_value());
    REQUIRE(backend.captured_requests.size() == 4);
    if (adopted_previews != nullptr) {
        *adopted_previews = hook.adopted;
    }
    return backend.captured_requests;
}

// 两条请求的旧消息(较短者的全部消息)是否逐字节相等(角色+块序+正文投影)。
bool OldMessagesStable(const api::Request& older, const api::Request& newer) {
    if (older.messages.size() > newer.messages.size()) {
        return false;
    }
    for (std::size_t i = 0; i < older.messages.size(); ++i) {
        if (older.messages[i].role != newer.messages[i].role) {
            return false;
        }
        if (older.messages[i].content.size() != newer.messages[i].content.size()) {
            return false;
        }
        for (std::size_t b = 0; b < older.messages[i].content.size(); ++b) {
            const auto& x = older.messages[i].content[b];
            const auto& y = newer.messages[i].content[b];
            if (x.index() != y.index()) {
                return false;
            }
            if (const auto* tx = std::get_if<api::TextBlock>(&x)) {
                if (tx->text != std::get<api::TextBlock>(y).text) return false;
            } else if (const auto* rx = std::get_if<api::ToolResultBlock>(&x)) {
                const auto& ry = std::get<api::ToolResultBlock>(y);
                if (rx->tool_use_id != ry.tool_use_id || rx->content != ry.content) return false;
            } else if (const auto* ux = std::get_if<api::ToolUseBlock>(&x)) {
                const auto& uy = std::get<api::ToolUseBlock>(y);
                if (ux->id != uy.id || ux->name != uy.name) return false;
            }
        }
    }
    return true;
}

const api::ToolResultBlock* FindResult(const api::Request& request, const std::string& tool_use_id) {
    for (const auto& message : request.messages) {
        for (const auto& block : message.content) {
            if (const auto* result = std::get_if<api::ToolResultBlock>(&block);
                result != nullptr && result->tool_use_id == tool_use_id) {
                return result;
            }
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("TEST-GAP-01: 多轮编排——大结果走预览、前缀稳定、校准系数不参与 v3 主路") {
    // 第一遍:热校准器(系数 2.5)。
    agent::TokenCalibrator hot;
    MakeHotCalibrator(hot);
    CHECK(hot.Coefficient("fake-provider", "test-model") == doctest::Approx(2.5));
    std::vector<std::string> adopted;
    const std::vector<api::Request> hot_requests = RunOrchestration(&hot, &adopted);

    // 第二遍:干净校准器(系数 1.0)。两遍请求必须逐字节一致——v3 主路的
    // 预览、裁剪、容量判定全不吃校准系数;吃了,第二遍就会分叉。
    agent::TokenCalibrator clean;
    CHECK(clean.Coefficient("fake-provider", "test-model") == doctest::Approx(1.0));
    const std::vector<api::Request> clean_requests = RunOrchestration(&clean, nullptr);
    REQUIRE(hot_requests.size() == clean_requests.size());
    for (std::size_t i = 0; i < hot_requests.size(); ++i) {
        CHECK(api::chat::BuildRequestJson(hot_requests[i]).dump() ==
              api::chat::BuildRequestJson(clean_requests[i]).dump());
    }

    // 1. 大结果按预算预览进请求:两次调用的结果都不是 200k 原文,且互相
    //    各自稳定(第二轮请求里第一枚结果的表示不再变化)。
    REQUIRE(adopted.size() == 2);
    const api::ToolResultBlock* first_preview = FindResult(hot_requests[1], "call-big-1");
    REQUIRE(first_preview != nullptr);
    CHECK(first_preview->content.size() < 200000);
    CHECK(first_preview->content == adopted[0]);
    const api::ToolResultBlock* second_preview = FindResult(hot_requests[3], "call-big-2");
    REQUIRE(second_preview != nullptr);
    CHECK(second_preview->content.size() < 200000);

    // 2. 前缀稳定:相邻请求旧消息逐字节相等(无动态暗裁),usage 反馈
    //    (input 50k→52k→54k→56k)不追改已发表示。
    for (std::size_t i = 1; i < hot_requests.size(); ++i) {
        CHECK_MESSAGE(OldMessagesStable(hot_requests[i - 1], hot_requests[i]),
                      "请求 " << (i - 1) << " -> " << i << " 旧消息应逐字节稳定");
    }
    // 第一枚大结果的预览在后续所有请求里一字不变。
    for (std::size_t i = 2; i < hot_requests.size(); ++i) {
        const api::ToolResultBlock* replayed = FindResult(hot_requests[i], "call-big-1");
        REQUIRE(replayed != nullptr);
        CHECK(replayed->content == adopted[0]);
    }
}

TEST_CASE("TEST-GAP-01: 同一请求的 bytes/4 估算不随 usage 反馈变化") {
    // UHURZ3 病理的另一面:估算若吃校准样本,同一份快照在 usage 反馈前后
    // 会给出不同数字。这里对同一份捕获请求现算 bytes/4,校准器热/冷两遍
    // 的同一请求快照估算必须相等(计量对象冻结,估算只由输入决定)。
    agent::TokenCalibrator hot;
    MakeHotCalibrator(hot);
    const std::vector<api::Request> hot_requests = RunOrchestration(&hot, nullptr);
    agent::TokenCalibrator clean;
    const std::vector<api::Request> clean_requests = RunOrchestration(&clean, nullptr);
    for (std::size_t i = 0; i < hot_requests.size(); ++i) {
        const auto hot_snapshot = api::ModelInputSnapshotFromWire(
            api::chat::BuildRequestJson(hot_requests[i]).dump());
        const auto clean_snapshot = api::ModelInputSnapshotFromWire(
            api::chat::BuildRequestJson(clean_requests[i]).dump());
        REQUIRE(hot_snapshot.has_value());
        REQUIRE(clean_snapshot.has_value());
        const auto hot_estimate = hooks::middleware::ComputeUtf8BytesDiv4Estimate(*hot_snapshot);
        const auto clean_estimate = hooks::middleware::ComputeUtf8BytesDiv4Estimate(*clean_snapshot);
        CHECK(hot_estimate.at("estimatedInputTokens") == clean_estimate.at("estimatedInputTokens"));
        CHECK(hot_estimate.at("inputUtf8Bytes") == clean_estimate.at("inputUtf8Bytes"));
        CHECK(hot_estimate.at("estimator") == "utf8_bytes_div4");
    }
}
