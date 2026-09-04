// 实验 A(工具检索退化阈值,Q2 量化评测单 §三)的 P0 最小装置:
// A2 档(40 只工具)× disabled 模式(全量直挂)× T1 任务(单步调核心
// 工具 read_file),固定假 backend,重复 5 次,逐次落一行 JSONL 原始账。
//
// P0 只钉"装置转得动、账落得下":五档×三模式×三任务的矩阵(单子 §三)
// 是 P2 的活,这里把工具表生成器与记账字段先立住。
//
// 记账(单子 §三):首 token 延迟(假 backend 首 event 回调延迟,量的是
// 宿主组装与回调路径,不是真网络)、tools/system/总请求字节、模型决策
// 正确率(T1 = 第一步调对 read_file 且执行成功)。零分母由 collect 侧按
// "unavailable 不填 0"的记账规则处理。
//
// 用法(ctest 或手动):
//   eval_tool_search_threshold [输出目录]
// 输出目录缺省取编译期 LUBANCODE_EVAL_TOOL_SEARCH_ROOT(源码树
//   tests/eval/tool_search_threshold),原始账落其 results/ 子目录。
// 退出码:装置自身坏(断言不过)非 0;正常跑完 0——就算模型决策错了也
// 照记不误(账是账,判是判)。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "api/types.hpp"
#include "fake_backend.hpp"
#include "tools/read_file.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "tools/tool_search.hpp"

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using lubancode::api::Message;
using lubancode::api::StreamEvent;
using lubancode_eval::FakeStreamingBackend;

std::string PathToUtf8(const fs::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// ---- 工具表生成器(最小版) -----------------------------------------------
//
// 单子 §三的档是"总 12/22/40/70/120,核心 12 直挂余延迟"。P0 只造 A2
// (40 = 12 核心 + 28 延迟):
//   - 核心 12:第 1 只用真 ReadFileTool(T1 的靶子),其余 11 只合成;
//   - 延迟 28:合成工具,deferred() = true(身份是延迟工具——disabled
//     模式下它们照样直挂,这正是"全量直挂"的题义)。
// 合成工具的 description/schema 给固定中等形状:量的是工具表规模的账,
// 不是语义质量;P2 换形状时再参数化。

class SyntheticTool : public lubancode::tools::Tool {
public:
    SyntheticTool(std::string name, bool deferred)
        : name_(std::move(name)),
          deferred_(deferred),
          description_(name_ + ": Q2 实验 A 合成工具,校验工具表规模的记账路。"),
          schema_(json{{"type", "object"},
                       {"properties",
                        json{{"target", json{{"type", "string"}, {"description", "目标标识"}}},
                             {"detail", json{{"type", "integer"}, {"description", "详略档"}}}}},
                       {"required", json::array({"target"})}}) {}

    std::string name() const override { return name_; }
    std::string description() const override { return description_; }
    nlohmann::json input_schema() const override { return schema_; }
    bool needs_confirm() const override { return false; }
    bool deferred() const override { return deferred_; }
    lubancode::tools::Tool::Result execute(const nlohmann::json&) override {
        return {name_ + " 合成结果", false};
    }

private:
    std::string name_;
    bool deferred_;
    std::string description_;
    nlohmann::json schema_;
};

struct ToolTable {
    lubancode::tools::ToolRegistry registry;
    std::size_t core = 0;
    std::size_t deferred = 0;
    std::size_t total() const { return core + deferred; }
};

ToolTable BuildToolTable(std::size_t core_count, std::size_t deferred_count) {
    ToolTable table;
    table.core = core_count;
    table.deferred = deferred_count;
    // 核心 12:真 read_file 打头,其余合成。
    table.registry.Register(std::make_unique<lubancode::tools::ReadFileTool>());
    for (std::size_t i = 1; i < core_count; ++i) {
        std::ostringstream name;
        name << "core_" << std::setw(2) << std::setfill('0') << i << "_probe";
        table.registry.Register(std::make_unique<SyntheticTool>(name.str(), false));
    }
    for (std::size_t i = 0; i < deferred_count; ++i) {
        std::ostringstream name;
        name << "deferred_" << std::setw(2) << std::setfill('0') << i << "_probe";
        table.registry.Register(std::make_unique<SyntheticTool>(name.str(), true));
    }
    return table;
}

// ---- 请求字节的计量 --------------------------------------------------------
//
// 实验记账要"tools/system/总请求字节"。api::Request 是 wire 无关的中立
// 形状,四家 wire 序列化各有方言,这里立的是中立口径:同一投影函数对同一
// Request 恒同一字节数——档与模式之间可比。P2 报告沿用此口径并注明。

json MessageToJson(const Message& message) {
    json out;
    out["role"] = message.role == lubancode::api::Role::User
                      ? "user"
                      : (message.role == lubancode::api::Role::Assistant ? "assistant" : "other");
    json blocks = json::array();
    for (const auto& block : message.content) {
        if (const auto* text = std::get_if<lubancode::api::TextBlock>(&block)) {
            blocks.push_back(json{{"type", "text"}, {"text", text->text}});
        } else if (const auto* thinking = std::get_if<lubancode::api::ThinkingBlock>(&block)) {
            blocks.push_back(json{{"type", "thinking"}, {"text", thinking->text}});
        } else if (const auto* use = std::get_if<lubancode::api::ToolUseBlock>(&block)) {
            blocks.push_back(json{{"type", "tool_use"}, {"id", use->id}, {"name", use->name},
                                  {"input", use->input}});
        } else if (const auto* result = std::get_if<lubancode::api::ToolResultBlock>(&block)) {
            blocks.push_back(json{{"type", "tool_result"}, {"tool_use_id", result->tool_use_id},
                                  {"content", result->content}, {"is_error", result->is_error}});
        } else {
            blocks.push_back(json{{"type", "opaque"}});
        }
    }
    out["content"] = std::move(blocks);
    return out;
}

struct RequestBytes {
    std::size_t tools_bytes = 0;
    std::size_t system_bytes = 0;
    std::size_t request_bytes = 0;
};

RequestBytes MeasureRequest(const lubancode::api::Request& request) {
    RequestBytes out;
    json tools = json::array();
    for (const auto& tool : request.tools) {
        tools.push_back(json{{"name", tool.name},
                             {"description", tool.description},
                             {"input_schema", tool.input_schema}});
    }
    out.tools_bytes = tools.dump().size();
    out.system_bytes = request.system.size();
    json whole;
    whole["model"] = request.model;
    whole["system"] = request.system;
    whole["tools"] = std::move(tools);
    json messages = json::array();
    for (const auto& message : request.messages) {
        messages.push_back(MessageToJson(message));
    }
    whole["messages"] = std::move(messages);
    out.request_bytes = whole.dump().size();
    return out;
}

// ---- T1 一跑 ---------------------------------------------------------------

struct RepetitionRecord {
    json ToJson() const { return payload; }
    json payload;
};

// 跑一遍 T1:A2 档 disabled 模式,单步调 read_file 后收口。
// 返回 nullopt 表示装置自身坏(不是模型决策错——那是账)。
std::optional<RepetitionRecord> RunT1Once(std::size_t repeat, const fs::path& probe_file,
                                          const std::string& commit) {
    // disabled(全量直挂):枚数闸口径先记一笔——A2=40 只、阈值取默认 20 时
    // 判定本该启用延迟;disabled 模式即装配层不设 tool_filter、40 只全进
    // tools 数组(与 Agent::BuildToolDefinitions 的"没设谓词就是全量"同一
    // 条路)。判定值随账落盘,P2 对三模式时口径可复算。
    ToolTable table = BuildToolTable(12, 28);
    const bool should_defer_at_default_threshold =
        lubancode::tools::ShouldDeferTools(table.total(), 20, 0, 0);

    FakeStreamingBackend backend;
    const std::string probe_path = PathToUtf8(probe_file);
    const lubancode::api::Usage step1_usage{1200, 48, 900, 0, 0};
    const lubancode::api::Usage step2_usage{1400, 24, 1100, 0, 0};
    backend.scripts = {
        lubancode_eval::ToolUseScript("t1-call-1", "read_file",
                                      json{{"path", probe_path}}.dump(), step1_usage),
        lubancode_eval::TextScript("T1 收口:read_file 已调,结果在案。", step2_usage),
    };

    // disabled:不设 tool_filter → BuildToolDefinitions 全量直挂。
    lubancode::agent::AgentProfile profile;
    profile.provider = "fake";
    profile.request.model = "fake-eval-model";
    profile.system_prompt = "Q2 实验 A 装置系统提示(P0 最小版)。";
    lubancode::agent::Agent agent(backend, table.registry, std::move(profile));

    lubancode::agent::TurnWiring wiring;  // 全空:eval 驱动无 UI、无审批
    const auto outcome = agent.Run("T1:请调用 read_file 读取探针文件。", wiring);
    if (!outcome.has_value()) {
        std::fprintf(stderr, "repeat %zu: Run 报错: %s\n", repeat, outcome.error().c_str());
        return std::nullopt;
    }

    // ---- 装置断言(不是模型决策账):请求两步、第一步 40 只工具全量直挂 ----
    if (backend.captured_requests.size() != 2) {
        std::fprintf(stderr, "repeat %zu: 期望两步请求,实得 %zu\n", repeat,
                     backend.captured_requests.size());
        return std::nullopt;
    }
    const auto& first_request = backend.captured_requests[0];
    if (first_request.tools.size() != table.total()) {
        std::fprintf(stderr, "repeat %zu: disabled 应全量直挂 %zu 只,实得 %zu\n", repeat,
                     table.total(), first_request.tools.size());
        return std::nullopt;
    }

    // ---- 模型决策账:T1 = 第一步调对 read_file 且历史里 tool_result 非 error ----
    // 第一步的决策体现在第二步请求的历史里(assistant tool_use + user
    // tool_result),直接翻第二轮请求的消息。
    bool called_read_file = false;
    bool tool_result_ok = false;
    std::string last_result_text;
    for (const auto& message : backend.captured_requests[1].messages) {
        for (const auto& block : message.content) {
            if (const auto* use = std::get_if<lubancode::api::ToolUseBlock>(&block);
                use != nullptr && use->name == "read_file") {
                called_read_file = true;
            }
            if (const auto* result = std::get_if<lubancode::api::ToolResultBlock>(&block);
                result != nullptr && !result->is_error) {
                tool_result_ok = true;
                last_result_text = result->content;
            }
        }
    }
    const bool decision_correct = called_read_file && tool_result_ok;

    const RequestBytes bytes = MeasureRequest(first_request);
    const auto first_latency = backend.FirstEventLatencyMs(0);

    json record;
    record["experiment"] = "tool_search_threshold";
    record["tier"] = "A2";
    record["total_tools"] = table.total();
    record["core_tools"] = table.core;
    record["deferred_tools"] = table.deferred;
    record["mode"] = "disabled";
    record["task"] = "T1";
    record["repeat"] = repeat;
    record["steps_used"] = outcome->steps_used;
    record["tools_in_request"] = first_request.tools.size();
    record["should_defer_at_default_threshold"] = should_defer_at_default_threshold;
    record["tools_bytes"] = bytes.tools_bytes;
    record["system_bytes"] = bytes.system_bytes;
    record["request_bytes"] = bytes.request_bytes;
    record["first_event_latency_ms"] = first_latency.has_value() ? json(*first_latency) : json(nullptr);
    record["decision_correct"] = decision_correct;
    record["read_file_result_preview"] = last_result_text.substr(0, 80);
    record["input_tokens"] = backend.usage_log.empty() ? 0 : backend.usage_log[0].usage.input_tokens;
    record["output_tokens"] = backend.usage_log.empty() ? 0 : backend.usage_log[0].usage.output_tokens;
    record["cache_read_tokens"] =
        backend.usage_log.empty() ? 0 : backend.usage_log[0].usage.cache_read_tokens;
    record["commit"] = commit;
    record["model"] = "fake-eval-model";
    record["provider"] = "fake";
    return RepetitionRecord{std::move(record)};
}

std::string NowIso8601Utc() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

}  // namespace

int main(int argc, char** argv) {
    fs::path out_dir;
    if (argc > 1) {
        out_dir = fs::path(argv[1]);
    } else {
#ifdef LUBANCODE_EVAL_TOOL_SEARCH_ROOT
        out_dir = fs::path(LUBANCODE_EVAL_TOOL_SEARCH_ROOT);
#else
        std::fprintf(stderr, "eval_tool_search_threshold: 缺输出目录(参数 1 或编译期根)\n");
        return 2;
#endif
    }
    out_dir /= "results";
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    if (ec) {
        std::fprintf(stderr, "建不了输出目录 %s: %s\n", PathToUtf8(out_dir).c_str(),
                     ec.message().c_str());
        return 2;
    }

    const std::string commit =
#ifdef LUBANCODE_GIT_COMMIT
        LUBANCODE_GIT_COMMIT;
#else
        "unknown";
#endif

    // T1 探针文件:read_file 真执行读它(装置走的是真工具,不是假执行)。
    const fs::path probe_file = out_dir / "t1_probe.txt";
    {
        std::ofstream out(probe_file, std::ios::binary);
        out << "Q2 eval T1 probe line 1: the quick brown fox\n";
        out << "Q2 eval T1 probe line 2: jumps over the lazy dog\n";
    }

    constexpr std::size_t kRepeats = 5;  // 单子 §三:每格 5 次
    const fs::path raw_path = out_dir / "raw_a2_disabled_t1.jsonl";
    {
        std::ofstream raw(raw_path, std::ios::binary);
        if (!raw.is_open()) {
            std::fprintf(stderr, "写不开原始账 %s\n", PathToUtf8(raw_path).c_str());
            return 2;
        }
        for (std::size_t repeat = 1; repeat <= kRepeats; ++repeat) {
            const auto record = RunT1Once(repeat, probe_file, commit);
            if (!record.has_value()) {
                return 1;  // 装置断言失败已在 stderr 报点
            }
            json line = record->ToJson();
            line["recorded_at"] = NowIso8601Utc();
            raw << line.dump() << "\n";
        }
    }

    std::printf("eval_tool_search_threshold: A2 disabled T1 x%zu 落 %s\n", kRepeats,
                PathToUtf8(raw_path).c_str());
    return 0;
}
