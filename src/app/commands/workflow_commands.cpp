// /workflow 命令终端薄壳实现(自然语言编排单第 1 批)。

#include "app/commands/workflow_commands.hpp"
#include "app/commands/command_registry.hpp"  // SlashDispatchContext(分派注册制)
#include "app/commands/agent_commands.hpp"  // ComputeAgentScanRoots:`agent:` 节点校验表的 Catalog 根
#include "app/tool_runtime.hpp"  // ResolveCustomAgentMaterial(阶段 6:agent 节点解析钉快照)
#include "package/mounting.hpp"               // MountWorkflowSources(阶段 3 包层挂载)
#include "app/model_router.hpp"
#include "cli/ask_user_prompt.hpp"             // PromptAskUser(终端交互宿主,骨架拆解反弹·问题 1 搬来)
#include "app/wirings/workflow_wiring.hpp"   // BuildWorkflowExecutors(问题 3:装配根搬 wirings)
#include "tools/path_utils.hpp"                // Utf8ToPath(catalog 锚点拼路径)
#include "tools/skill_loader.hpp"
#include "cli/agent_panel_host.hpp"
#include "cli/console_input.hpp"
#include "cli/format_utils.hpp"
#include "cli/markdown.hpp"  // RenderMarkdown(原先经 turn_runner.hpp 间接带进,显式化)
#include "cli/terminal_port.hpp"  // TermOut/TermErr:散打 std::cout 清零,统一走输出端口
#include "cli/transcript.hpp"

using lubancode::cli::TermOut;
using lubancode::cli::TermErr;

#include <atomic>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

#include "agent/agent.hpp"  // AgentProfile(批四自立门户)
#include "agent/agent_catalog.hpp"  // BuiltinGeneralPurposeDefinition:workflow default binding 的定义
#include "agent/agent_profile_resolver.hpp"  // ResolveAgentProfile:阶段 3 统一解析(workflow 绑定同源)
#include "cli/slash_commands.hpp"
#include "config/model_catalog.hpp"
#include "platform/console.hpp"
#include "platform/paths.hpp"
#include "platform/text_encoding.hpp"
#include "workflow/compiler.hpp"
#include "workflow/graph_view.hpp"

namespace lubancode::app {

WorkflowPanelOutput FormatWorkflowPanelOutput(const std::string& raw) {
    WorkflowPanelOutput out;
    const nlohmann::json parsed = nlohmann::json::parse(raw, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        out.markdown = raw;
        return out;
    }
    out.structured = true;

    const auto string_field = [&parsed](const char* key) {
        const auto found = parsed.find(key);
        return found != parsed.end() && found->is_string() ? found->get<std::string>() : std::string();
    };
    const auto first_line = [](std::string value) {
        const std::size_t end = value.find('\n');
        if (end != std::string::npos) value.resize(end);
        return value;
    };
    auto append_list = [](std::ostringstream& text, const std::string& title,
                          const nlohmann::json& values) {
        if (!values.is_array() || values.empty()) return;
        text << "\n\n### " << title << "\n";
        for (const auto& value : values) {
            text << "\n- ";
            if (value.is_string()) text << value.get<std::string>();
            else text << value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        }
    };
    auto append_dispatches = [&append_list](std::ostringstream& text, const nlohmann::json& dispatches) {
        if (!dispatches.is_array()) return;
        for (const auto& dispatch : dispatches) {
            if (!dispatch.is_object()) continue;
            const std::string ministry = dispatch.value("ministry", std::string("差遣"));
            const std::string objective = dispatch.value("objective", std::string());
            text << "\n\n### " << ministry;
            if (!objective.empty()) text << "：" << objective;
            append_list(text, "准许范围", dispatch.value("allowed_scope", nlohmann::json::array()));
            append_list(text, "禁域", dispatch.value("forbidden_scope", nlohmann::json::array()));
            append_list(text, "验收", dispatch.value("acceptance", nlohmann::json::array()));
            const std::string ambiguity = dispatch.value("ambiguity_action", std::string());
            if (!ambiguity.empty()) text << "\n\n**遇疑：** " << ambiguity;
        }
    };

    out.summary = string_field("summary");
    if (out.summary.empty()) out.summary = string_field("name");

    if (const std::string memorial = string_field("memorial"); !memorial.empty()) {
        out.markdown = memorial;
        if (out.summary.empty()) out.summary = first_line(memorial);
        return out;
    }

    if (!string_field("name").empty() || !string_field("strategy").empty()) {
        std::ostringstream text;
        const std::string name = string_field("name");
        text << "## " << (name.empty() ? "候选方案" : name);
        const std::string strategy = string_field("strategy");
        if (!strategy.empty()) text << "\n\n" << strategy;
        append_list(text, "改动范围", parsed.value("scope", nlohmann::json::array()));
        append_list(text, "影响面", parsed.value("impact", nlohmann::json::array()));
        append_list(text, "验收", parsed.value("acceptance", nlohmann::json::array()));
        append_list(text, "尚待裁定", parsed.value("unknowns", nlohmann::json::array()));
        append_list(text, "取舍", parsed.value("tradeoffs", nlohmann::json::array()));
        out.markdown = text.str();
        return out;
    }

    if (parsed.contains("approved") && parsed["approved"].is_boolean()) {
        const bool approved = parsed["approved"].get<bool>();
        out.summary = approved ? "门下通过" : "门下驳回";
        std::ostringstream text;
        text << "## 门下判词\n\n**" << (approved ? "准" : "驳") << "**";
        append_list(text, "判词", parsed.value("reasons", nlohmann::json::array()));
        const std::string question = string_field("question");
        if (!question.empty()) text << "\n\n### 待明确处\n\n" << question;
        out.markdown = text.str();
        return out;
    }

    if (parsed.contains("dispatches") && parsed["dispatches"].is_array()) {
        std::ostringstream text;
        const std::string roster = string_field("roster");
        text << "## 差遣清单";
        if (!roster.empty()) text << "\n\n" << roster;
        append_dispatches(text, parsed["dispatches"]);
        out.markdown = text.str();
        if (out.summary.empty()) {
            out.summary = !roster.empty() ? first_line(roster)
                                          : std::to_string(parsed["dispatches"].size()) + " 封差遣";
        }
        return out;
    }

    if (const std::string report = string_field("report"); !report.empty()) {
        out.markdown = report;
        if (out.summary.empty()) out.summary = first_line(report);
        return out;
    }

    static const std::map<std::string, std::string> labels = {
        {"selected_strategy", "所取路线"}, {"scope", "改动范围"},
        {"impact", "影响面"}, {"acceptance", "验收"},
        {"known_unknowns", "已知未知"}, {"unknowns", "尚待裁定"},
        {"tradeoffs", "取舍"}, {"reasons", "判词"}, {"question", "待明确处"},
        {"executions", "办差回报"}, {"objective", "差事"},
    };
    std::ostringstream text;
    for (const auto& [key, value] : parsed.items()) {
        const auto label = labels.find(key);
        text << (text.tellp() > 0 ? "\n\n" : "") << "### "
             << (label == labels.end() ? key : label->second) << "\n\n";
        if (value.is_array()) {
            for (const auto& item : value) {
                text << "- " << (item.is_string() ? item.get<std::string>()
                                                   : item.dump(2, ' ', false,
                                                               nlohmann::json::error_handler_t::replace))
                     << "\n";
            }
        } else if (value.is_string()) {
            text << value.get<std::string>();
        } else if (value.is_boolean()) {
            text << (value.get<bool>() ? "是" : "否");
        } else {
            text << value.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
        }
    }
    out.markdown = text.str();
    return out;
}

std::string FormatWorkflowPanelInput(const nlohmann::json& input) {
    if (!input.is_object()) {
        return input.is_string() ? input.get<std::string>()
                                 : input.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    std::ostringstream text;
    auto add = [&text](const std::string& label, const std::string& value) {
        if (value.empty()) return;
        if (text.tellp() > 0) text << "\n\n";
        text << "**" << label << "：** " << value;
    };
    if (const auto requirement = input.find("requirement");
        requirement != input.end() && requirement->is_string()) {
        add("任务", requirement->get<std::string>());
    }
    if (const auto item = input.find("item"); item != input.end()) {
        if (item->is_string()) {
            add("本路立场", item->get<std::string>());
        } else if (item->is_object()) {
            add("署部", item->value("ministry", std::string()));
            add("差事", item->value("objective", std::string()));
        }
    }
    if (const auto iteration = input.find("iteration");
        iteration != input.end() && iteration->is_number_integer()) {
        add("轮次", std::to_string(iteration->get<int>()));
    }
    if (text.tellp() > 0) return text.str();

    for (const auto& [key, value] : input.items()) {
        std::string preview;
        if (value.is_string()) {
            preview = value.get<std::string>();
            if (preview.size() > 240) preview.resize(240), preview += "...";
        } else if (value.is_array()) {
            preview = "已收到 " + std::to_string(value.size()) + " 项";
        } else if (value.is_object()) {
            preview = "已收到结构化材料";
        } else {
            preview = value.dump();
        }
        add(key, preview);
    }
    return text.str();
}

std::string FormatWorkflowRunResult(const nlohmann::json& result) {
    if (result.is_object()) {
        const auto report = result.find("report");
        if (report != result.end() && report->is_string() && !report->get_ref<const std::string&>().empty()) {
            return report->get<std::string>();
        }
    }

    return result.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
}

namespace {

class ReadyWorkflowInteractionFuture final : public lubancode::runtime::InteractionFuture {
public:
    explicit ReadyWorkflowInteractionFuture(std::optional<lubancode::runtime::ApprovalResponse> approval)
        : approval_(std::move(approval)) {}
    explicit ReadyWorkflowInteractionFuture(std::optional<lubancode::runtime::QuestionResponse> question)
        : question_(std::move(question)) {}

    std::optional<lubancode::runtime::ApprovalResponse> WaitApproval() override { return approval_; }
    std::optional<lubancode::runtime::QuestionResponse> WaitQuestion() override { return question_; }

private:
    std::optional<lubancode::runtime::ApprovalResponse> approval_;
    std::optional<lubancode::runtime::QuestionResponse> question_;
};

class TerminalWorkflowInteractionBroker final : public lubancode::runtime::InteractionBroker {
public:
    explicit TerminalWorkflowInteractionBroker(const lubancode::cli::Theme& theme) : theme_(theme) {}

    std::shared_ptr<lubancode::runtime::InteractionFuture> AskApproval(
        const lubancode::runtime::ApprovalRequest& request) override {
        lubancode::tools::AskUserQuestion question;
        question.header = "Workflow 审批";
        question.question = request.reason.empty() ? "要放行这一步吗?" : request.reason;
        question.options = {{"批准", "本次放行"}, {"拒绝", "不跑这一步"}};
        auto answer = lubancode::cli::PromptAskUser(question, theme_);
        if (!answer.has_value()) {
            return std::make_shared<ReadyWorkflowInteractionFuture>(
                std::optional<lubancode::runtime::ApprovalResponse>{});
        }
        lubancode::runtime::ApprovalResponse response;
        if (answer->kind == lubancode::tools::AskUserResponseKind::Answered &&
            !answer->answers.empty() && answer->answers.front() == "批准") {
            response.decision = lubancode::runtime::InteractionDecision::Accept;
        } else if (answer->kind == lubancode::tools::AskUserResponseKind::Declined) {
            response.decision = lubancode::runtime::InteractionDecision::Cancel;
        } else {
            response.decision = lubancode::runtime::InteractionDecision::Decline;
            response.reason = "用户拒绝";
        }
        return std::make_shared<ReadyWorkflowInteractionFuture>(
            std::optional<lubancode::runtime::ApprovalResponse>{std::move(response)});
    }

    std::shared_ptr<lubancode::runtime::InteractionFuture> AskQuestion(
        const lubancode::runtime::QuestionRequest& request) override {
        lubancode::tools::AskUserQuestion question;
        question.header = request.header;
        question.question = request.question;
        question.multi_select = request.multi_select;
        for (const auto& option : request.options) {
            question.options.push_back({option.label, option.description});
        }
        auto answer = lubancode::cli::PromptAskUser(question, theme_);
        if (!answer.has_value() || answer->kind == lubancode::tools::AskUserResponseKind::Declined) {
            return std::make_shared<ReadyWorkflowInteractionFuture>(
                std::optional<lubancode::runtime::QuestionResponse>{});
        }
        lubancode::runtime::QuestionResponse response;
        response.answers = answer->answers;
        if (answer->kind == lubancode::tools::AskUserResponseKind::Discuss && !answer->message.empty()) {
            response.answers.push_back(answer->message);
        }
        return std::make_shared<ReadyWorkflowInteractionFuture>(
            std::optional<lubancode::runtime::QuestionResponse>{std::move(response)});
    }

    bool ResolveApproval(const lubancode::runtime::InteractionRequestId&,
                         const lubancode::runtime::ApprovalResponse&) override { return false; }
    bool AnswerQuestion(const lubancode::runtime::InteractionRequestId&,
                        const lubancode::runtime::QuestionResponse&) override { return false; }

private:
    const lubancode::cli::Theme& theme_;
};

// workflow 直呼会同步占住会话线程，不能等主循环回来才重开 composer。
// 执行期借普通 turn 的 footer 与输入监听器：框常驻、消息可排队，Esc 也
// 直连图运行的 cancel token。析构先停读键线程，再擦 footer。
class WorkflowTerminalRunScope final {
public:
    WorkflowTerminalRunScope(const lubancode::cli::Theme& theme, bool interactive,
                             std::atomic<bool>& cancel)
        : theme_(theme),
          interactive_(interactive),
          footer_enabled_(interactive && lubancode::platform::SupportsScreenRepaint()),
          cancel_(cancel) {}

    void Start() {
        if (started_) return;
        started_ = true;
        started_at_ = std::chrono::steady_clock::now();
        lubancode::cli::BeginStreamFooter(theme_, footer_enabled_);
        if (footer_enabled_) {
            lubancode::cli::BeginTurnActivity(
                "流程候旨",
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
            heartbeat_ = std::make_unique<lubancode::cli::StreamFooterHeartbeat>(
                true, started_at_, &cancel_);
        }
        if (interactive_) {
            listener_ = std::make_unique<lubancode::cli::TurnInputListener>(cancel_, theme_);
        }
    }

    void BeginStage(const std::string& label) {
        if (!footer_enabled_ || !heartbeat_) return;
        started_at_ = std::chrono::steady_clock::now();
        heartbeat_->ResetElapsed(started_at_);
        lubancode::cli::BeginTurnActivity(
            label.empty() ? std::string("流程办理中") : label,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    ~WorkflowTerminalRunScope() {
        if (!started_) return;
        if (listener_) listener_->Stop();
        if (heartbeat_) heartbeat_->Stop();
        if (footer_enabled_) lubancode::cli::EndTurnActivity();
        lubancode::cli::EndStreamFooter();
    }

    WorkflowTerminalRunScope(const WorkflowTerminalRunScope&) = delete;
    WorkflowTerminalRunScope& operator=(const WorkflowTerminalRunScope&) = delete;

    const std::atomic<bool>* cancel_token() const { return &cancel_; }

private:
    const lubancode::cli::Theme& theme_;
    bool interactive_ = false;
    bool footer_enabled_ = false;
    bool started_ = false;
    std::chrono::steady_clock::time_point started_at_{};
    std::atomic<bool>& cancel_;
    std::unique_ptr<lubancode::cli::TurnInputListener> listener_;
    std::unique_ptr<lubancode::cli::StreamFooterHeartbeat> heartbeat_;
};

// "run-x-mouyi-i1-a2" -> "run-x-mouyi-i1"（去掉结尾的重试尾 -aN）。
// 结尾不是 -a<数字> 的原样返回——map/loop 的账都靠这只键归位。
std::string StripAttemptSuffix(const std::string& node_run_id) {
    const std::size_t dash = node_run_id.rfind("-a");
    if (dash == std::string::npos || dash + 2 >= node_run_id.size()) return node_run_id;
    for (std::size_t i = dash + 2; i < node_run_id.size(); ++i) {
        if (node_run_id[i] < '0' || node_run_id[i] > '9') return node_run_id;
    }
    return node_run_id.substr(0, dash);
}

// 键尾的 map 路号（"-i<下标>"，下标从 0 计）：返回路数（下标+1）；
// 不是 map 路键返回 0。node_id 自带 "-i<数字>" 结尾的会误认——别这么起名。
int LaneOfEntryKey(const std::string& key) {
    const std::size_t dash = key.rfind("-i");
    if (dash == std::string::npos || dash + 2 >= key.size()) return 0;
    int value = 0;
    for (std::size_t i = dash + 2; i < key.size(); ++i) {
        if (key[i] < '0' || key[i] > '9') return 0;
        value = value * 10 + (key[i] - '0');
    }
    return value + 1;
}

// workflow 里的 llm/agent 节点也是在干活的 Agent。它们没走 AgentTool
// 任务台，便用这本小账把节点事件投影成现成的 Agent panel 条目。
class WorkflowAgentPanelSink final : public lubancode::runtime::EventSink {
public:
    explicit WorkflowAgentPanelSink(const lubancode::cli::Theme& theme,
                                    std::function<void(const std::string&)> on_stage_started = {})
        : theme_(theme), on_stage_started_(std::move(on_stage_started)) {}

    void Emit(const lubancode::runtime::ServerEvent& event) override {
        if (!event.payload.is_object()) return;
        const std::string type = event.payload.value("type", std::string());
        if (type == lubancode::workflow::kEventNodeStarted ||
            type == lubancode::workflow::kEventNodeRetrying ||
            type == lubancode::workflow::kEventNodeCompleted) {
            ConsumeNodeEvent(type, event.payload);
            return;
        }
        ConsumeAgentEvent(event);
    }

    std::vector<lubancode::cli::AgentPanelEntry> Entries() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<lubancode::cli::AgentPanelEntry> out;
        out.reserve(nodes_.size());
        std::vector<const TrackedNode*> visible;
        visible.reserve(nodes_.size());
        for (const auto& [_, node] : nodes_) {
            if (!node.hidden) visible.push_back(&node);
        }
        std::sort(visible.begin(), visible.end(), [](const TrackedNode* left, const TrackedNode* right) {
            return left->order < right->order;
        });
        const auto now = std::chrono::steady_clock::now();
        for (const TrackedNode* node : visible) {
            lubancode::cli::AgentPanelEntry entry = node->entry;
            if (!node->result_summary.empty()) {
                entry.title = node->base_title + " · " + node->result_summary;
            }
            const auto end = entry.running ? now : node->ended_at;
            const double seconds = std::chrono::duration<double>(end - node->started_at).count();
            entry.state = node->phase;
            if (node->tool_calls > 0) {
                entry.state += " · " + std::to_string(node->tool_calls) + " 次工具";
            }
            if (node->tokens > 0) {
                entry.state += " · " + lubancode::cli::FormatTokenCount(node->tokens) + " tokens";
            } else if (entry.running) {
                entry.state += " · tokens 未报告";
            }
            entry.state += " · " + lubancode::cli::FormatSeconds((std::max)(0.0, seconds));
            out.push_back(std::move(entry));
        }
        return out;
    }

    bool OwnsTask(int task_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return FindByTaskId(task_id) != nullptr;
    }

    std::optional<std::vector<std::string>> TranscriptLines(int task_id, int width, bool expanded) const {
        TrackedNode node;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const TrackedNode* found = FindByTaskId(task_id);
            if (found == nullptr) return std::nullopt;
            node = *found;
        }

        std::vector<std::string> lines;
        lines.push_back(lubancode::cli::TruncateUtf8ToDisplayWidth(
            "── " + node.entry.name + " · " + node.entry.title + " ──", std::max(0, width - 1)));
        const auto end = node.entry.running ? std::chrono::steady_clock::now() : node.ended_at;
        const double seconds = std::chrono::duration<double>(end - node.started_at).count();
        std::string state = node.phase;
        if (node.tool_calls > 0) state += " · " + std::to_string(node.tool_calls) + " 次工具";
        state += node.tokens > 0 ? " · " + lubancode::cli::FormatTokenCount(node.tokens) + " tokens"
                                : " · tokens 未报告";
        state += " · " + lubancode::cli::FormatSeconds((std::max)(0.0, seconds));
        lines.push_back("  " + theme_.stats + state + theme_.reset);

        int next_item_id = 1;
        for (const DetailItem& detail : node.details) {
            switch (detail.kind) {
                case DetailKind::User:
                    AppendMarkdown(lines, theme_.confirm + "> 用户交办" + theme_.reset, detail.text, width);
                    break;
                case DetailKind::Steering:
                    AppendMarkdown(lines, theme_.confirm + "> 用户补充（已送达）" + theme_.reset,
                                   detail.text, width);
                    break;
                case DetailKind::Text:
                    AppendMarkdown(lines, theme_.banner + "● 产出" + theme_.reset,
                                   !detail.completed && detail.text.find_first_not_of(" \t\r\n") != std::string::npos &&
                                           detail.text[detail.text.find_first_not_of(" \t\r\n")] == '{'
                                       ? "结构化结果尚在誊写，办结后展开。"
                                       : detail.text,
                                   width);
                    break;
                case DetailKind::Thinking: {
                    const auto item = lubancode::cli::MakeAgentTaskThinkingItem(
                        next_item_id++, detail.text, !detail.completed);
                    AppendRendered(lines, lubancode::cli::FormatTranscriptItem(
                                              item, theme_, width, expanded && !detail.completed));
                    break;
                }
                case DetailKind::Tool: {
                    // 投影坐标(同构渲染单 P0):workflow 节点查看页的根是
                    // 节点自己,它调用的工具是这张面板的顶层 Tool,不再缩四格。
                    const auto item = lubancode::cli::MakeAgentTaskToolItem(
                        next_item_id++, detail.tool_name, detail.input_json, detail.completed,
                        detail.is_error, detail.result, lubancode::cli::TranscriptKind::Tool);
                    AppendRendered(lines,
                                   lubancode::cli::FormatTranscriptItem(item, theme_, width, false));
                    break;
                }
                case DetailKind::Notice:
                    lines.push_back("  " + theme_.stats + detail.text + theme_.reset);
                    break;
            }
        }

        const auto pending = lubancode::cli::SessionSteeringQueue().Snapshot();
        std::vector<std::string> waiting;
        for (const auto& message : pending) {
            if (message.target == lubancode::cli::MessageTarget::Agent(task_id) &&
                message.state == lubancode::cli::QueueItemState::Queued) {
                waiting.push_back(message.text);
            }
        }
        if (!waiting.empty()) {
            lines.push_back(theme_.stats + "待下一轮送达 " + std::to_string(waiting.size()) + " 条" + theme_.reset);
            for (const auto& text : waiting) {
                lines.push_back("  * " + lubancode::cli::TruncateUtf8ToDisplayWidth(text, std::max(0, width - 5)));
            }
        }
        return lines;
    }

    std::optional<lubancode::workflow::NodeSteeringBatch> TakeSteering(
        const lubancode::workflow::NodeExecRequest& request) {
        int task_id = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto run = node_runs_.find(request.node_run_id);
            if (run == node_runs_.end()) return std::nullopt;
            const auto found = nodes_.find(run->second);
            if (found == nodes_.end()) return std::nullopt;
            task_id = found->second.entry.task_id;
        }
        auto queued = lubancode::cli::SessionSteeringQueue().TakeDeliverable(
            lubancode::cli::MessageTarget::Agent(task_id));
        if (queued.empty()) return std::nullopt;

        std::ostringstream input;
        input << "[用户从 Agent 面板补充]\n";
        {
            std::lock_guard<std::mutex> lock(mutex_);
            TrackedNode* node = FindByTaskId(task_id);
            for (std::size_t i = 0; i < queued.size(); ++i) {
                if (i > 0) input << '\n';
                input << queued[i].text;
                if (node != nullptr) node->details.push_back(DetailItem{DetailKind::Steering, queued[i].text});
            }
            if (node != nullptr) {
                node->phase = "收到补充 · 继续办理";
                ++node->entry.content_revision;
            }
        }

        auto restore_items = std::make_shared<std::vector<lubancode::cli::QueuedMessage>>(std::move(queued));
        lubancode::workflow::NodeSteeringBatch batch;
        batch.input = input.str();
        batch.restore = [restore_items]() {
            for (auto it = restore_items->rbegin(); it != restore_items->rend(); ++it) {
                lubancode::cli::SessionSteeringQueue().ReturnToFront(std::move(*it));
            }
            restore_items->clear();
        };
        return batch;
    }

    void ClosePendingMessages() const {
        std::set<int> task_ids;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [_, node] : nodes_) task_ids.insert(node.entry.task_id);
        }
        for (const auto& message : lubancode::cli::SessionSteeringQueue().Snapshot()) {
            if (message.target.kind == lubancode::cli::MessageTarget::Kind::Subagent &&
                task_ids.contains(message.target.task_id)) {
                lubancode::cli::SessionSteeringQueue().MarkTargetGone(
                    message.id, "workflow Agent 已收场，消息没有送出");
            }
        }
    }

    bool CancelTask(int task_id, std::atomic<bool>& cancel) {
        std::lock_guard<std::mutex> lock(mutex_);
        TrackedNode* node = FindByTaskId(task_id);
        if (node == nullptr || !node->entry.running) return false;
        node->phase = "停止中";
        ++node->entry.content_revision;
        cancel.store(true);
        return true;
    }

    bool ClearTask(int task_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        TrackedNode* node = FindByTaskId(task_id);
        if (node == nullptr || node->entry.running) return false;
        node->hidden = true;
        ++node->entry.content_revision;
        return true;
    }

    int CancelAll(std::atomic<bool>& cancel) {
        std::lock_guard<std::mutex> lock(mutex_);
        int running = 0;
        for (auto& [_, node] : nodes_) {
            if (!node.entry.running) continue;
            ++running;
            node.phase = "停止中";
            ++node.entry.content_revision;
        }
        if (running > 0) cancel.store(true);
        return running;
    }

private:
    enum class DetailKind { User, Steering, Text, Thinking, Tool, Notice };

    struct DetailItem {
        DetailKind kind = DetailKind::Notice;
        std::string text;
        std::string tool_name;
        std::string input_json;
        std::string result;
        bool completed = false;
        bool is_error = false;
    };

    struct TrackedNode {
        lubancode::cli::AgentPanelEntry entry;
        std::string base_title;
        std::string result_summary;
        std::string phase = "等待模型";
        std::uint64_t order = 0;
        std::chrono::steady_clock::time_point started_at{};
        std::chrono::steady_clock::time_point ended_at{};
        std::int64_t tokens = 0;
        int tool_calls = 0;
        bool hidden = false;
        std::vector<DetailItem> details;
        std::map<std::string, std::size_t> item_indexes;
    };

    static void AppendRendered(std::vector<std::string>& lines, const std::string& rendered) {
        std::istringstream in(rendered);
        std::string line;
        while (std::getline(in, line)) lines.push_back(std::move(line));
    }

    void AppendMarkdown(std::vector<std::string>& lines, const std::string& header,
                        const std::string& text, int width) const {
        lines.push_back(header);
        for (const auto& line : lubancode::cli::RenderMarkdown(text, theme_, width)) lines.push_back(line);
    }

    static int NextTaskId() {
        static std::atomic<int> next{1000000000};
        return next.fetch_add(1);
    }

    static std::pair<std::string, std::string> SplitLabel(const std::string& label,
                                                           const std::string& node_id) {
        const std::string text = label.empty() ? node_id : label;
        const std::size_t split = text.find(' ');
        if (split == std::string::npos) return {text, node_id};
        return {text.substr(0, split), text.substr(split + 1)};
    }

    TrackedNode* FindByTaskId(int task_id) const {
        for (auto& [_, node] : nodes_) {
            if (node.entry.task_id == task_id) return &node;
        }
        return nullptr;
    }

    void ConsumeNodeEvent(const std::string& type, const nlohmann::json& payload) {
        if (type == lubancode::workflow::kEventNodeStarted && on_stage_started_) {
            on_stage_started_(payload.value("label", payload.value("node_id", std::string())));
        }
        const std::string kind = payload.value("kind", std::string());
        if (kind != "agent" && kind != "llm") return;
        const std::string node_id = payload.value("node_id", std::string());
        if (node_id.empty()) return;

        // 记账键:node_run_id 去掉重试尾(-aN)。"run-x-mouyi-i1-a2" 归
        // "run-x-mouyi-i1"——map/foreach 各路各一条(并发同跑不串账);loop
        // 每轮 attempt 都从 1 起,同键复用,清账重来。旧事件不带
        // node_run_id 时退回 node_id,行为照旧。
        const std::string node_run_id = payload.value("node_run_id", std::string());
        const std::string entry_key = !node_run_id.empty() ? StripAttemptSuffix(node_run_id) : node_id;

        std::lock_guard<std::mutex> lock(mutex_);
        if (type == lubancode::workflow::kEventNodeStarted) {
            TrackedNode& node = nodes_[entry_key];
            const bool fresh_entry = node.entry.task_id == 0;
            if (fresh_entry) node.entry.task_id = NextTaskId();
            const auto [name, title] = SplitLabel(payload.value("label", std::string()), node_id);
            node.entry.name = name;
            // map 多路同名:新条目行尾挂路号分彼此;重跑(loop/重试)不换题。
            node.entry.title =
                fresh_entry && LaneOfEntryKey(entry_key) > 0
                    ? title + " · 第" + std::to_string(LaneOfEntryKey(entry_key)) + "路"
                    : title;
            node.base_title = node.entry.title;
            node.result_summary.clear();
            if (fresh_entry) node.order = next_order_++;
            node.entry.running = true;
            node.entry.failed = false;
            node.entry.cancelled = false;
            node.entry.done_delivered = false;
            node.hidden = false;
            node.phase = "已交办 · 等待模型";
            node.started_at = std::chrono::steady_clock::now();
            node.ended_at = node.started_at;
            node.tokens = 0;
            node.tool_calls = 0;
            const int attempt = payload.value("attempt", 1);
            if (attempt <= 1) {
                node.details.clear();
                node.item_indexes.clear();
            } else {
                node.details.push_back(
                    DetailItem{DetailKind::Notice, "第 " + std::to_string(attempt) + " 次办理"});
            }
            if (payload.contains("input")) {
                const auto& input = payload["input"];
                node.details.push_back(DetailItem{
                    DetailKind::User, FormatWorkflowPanelInput(input)});
            }
            ++node.entry.content_revision;
            if (!node_run_id.empty()) node_runs_[node_run_id] = entry_key;
            return;
        }

        const auto found = nodes_.find(entry_key);
        if (found == nodes_.end()) return;
        TrackedNode& node = found->second;
        if (type == lubancode::workflow::kEventNodeRetrying) {
            node.phase = "退回重办 · 等待下一试";
            node.details.push_back(DetailItem{DetailKind::Notice, node.phase});
            ++node.entry.content_revision;
            return;
        }

        const std::string outcome = payload.value("outcome", std::string());
        node.entry.running = false;
        node.entry.failed = outcome != "success" && outcome != "empty" && outcome != "cancelled";
        node.entry.cancelled = outcome == "cancelled";
        node.ended_at = std::chrono::steady_clock::now();
        node.tokens = payload.value("tokens", node.tokens);
        if (outcome == "success") node.phase = "已办结";
        else if (outcome == "empty") node.phase = "已办结 · 无输出";
        else if (outcome == "cancelled") node.phase = "已取消";
        else node.phase = "出错 " + payload.value("code", std::string());
        node.details.push_back(DetailItem{DetailKind::Notice, node.phase});
        ++node.entry.content_revision;
    }

    void ConsumeAgentEvent(const lubancode::runtime::ServerEvent& event) {
        const std::string node_run_id = event.payload.value("workflow_node_run_id", std::string());
        if (node_run_id.empty()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto run = node_runs_.find(node_run_id);
        if (run == node_runs_.end()) return;
        const auto found = nodes_.find(run->second);
        if (found == nodes_.end()) return;
        TrackedNode& node = found->second;

        auto find_item = [&node, &event]() -> DetailItem* {
            const auto item = node.item_indexes.find(event.item_id);
            if (item == node.item_indexes.end() || item->second >= node.details.size()) return nullptr;
            return &node.details[item->second];
        };

        switch (event.kind) {
            case lubancode::runtime::ServerEventKind::ModelStepStarted:
                node.phase = "思考中";
                break;
            case lubancode::runtime::ServerEventKind::ItemStarted:
                if (!event.item_id.empty() && !node.item_indexes.contains(event.item_id)) {
                    DetailItem detail;
                    if (event.item_kind == lubancode::runtime::ItemKind::Tool) {
                        detail.kind = DetailKind::Tool;
                        detail.tool_name = event.payload.value("tool_name", std::string("tool"));
                        if (event.payload.contains("input")) detail.input_json = event.payload["input"].dump();
                    } else if (event.item_kind == lubancode::runtime::ItemKind::Thinking) {
                        detail.kind = DetailKind::Thinking;
                    } else {
                        detail.kind = DetailKind::Text;
                    }
                    node.item_indexes[event.item_id] = node.details.size();
                    node.details.push_back(std::move(detail));
                }
                if (event.item_kind == lubancode::runtime::ItemKind::Tool) {
                    ++node.tool_calls;
                    node.phase = "调用工具 " + event.payload.value("tool_name", std::string());
                } else if (event.item_kind == lubancode::runtime::ItemKind::Thinking) {
                    node.phase = "思考中";
                } else if (event.item_kind == lubancode::runtime::ItemKind::Text) {
                    node.phase = "整理答复";
                }
                break;
            case lubancode::runtime::ServerEventKind::ItemDelta:
                if (DetailItem* detail = find_item(); detail != nullptr) detail->text += event.text;
                if (event.item_kind == lubancode::runtime::ItemKind::Thinking) node.phase = "思考中";
                if (event.item_kind == lubancode::runtime::ItemKind::Text) node.phase = "整理答复";
                break;
            case lubancode::runtime::ServerEventKind::ItemCompleted:
                if (DetailItem* detail = find_item(); detail != nullptr) {
                    detail->completed = true;
                    detail->is_error = event.outcome.has_value() &&
                                       *event.outcome != lubancode::runtime::Outcome::Succeeded;
                    detail->result = event.payload.value("result", std::string());
                    // llm 节点的正文常是机器 JSON(方案书、差遣单)。运行账
                    // 仍留原 JSON；面板在收口时投成人话，并把短摘要挂到导航行。
                    if (detail->kind == DetailKind::Text && !detail->text.empty()) {
                        WorkflowPanelOutput formatted = FormatWorkflowPanelOutput(detail->text);
                        if (formatted.structured) {
                            detail->text = std::move(formatted.markdown);
                            node.result_summary = std::move(formatted.summary);
                        }
                    }
                }
                break;
            case lubancode::runtime::ServerEventKind::UsageUpdated:
                node.tokens += event.payload.value("input_tokens", std::int64_t{0}) +
                               event.payload.value("cache_read_tokens", std::int64_t{0}) +
                               event.payload.value("cache_creation_tokens", std::int64_t{0}) +
                               event.payload.value("output_tokens", std::int64_t{0});
                break;
            case lubancode::runtime::ServerEventKind::QuestionRequested:
            case lubancode::runtime::ServerEventKind::ApprovalRequested:
                node.phase = "等待用户";
                break;
            case lubancode::runtime::ServerEventKind::Error:
                node.phase = "出错";
                node.details.push_back(DetailItem{DetailKind::Notice, node.phase});
                break;
            default:
                break;
        }
        ++node.entry.content_revision;
    }

    mutable std::mutex mutex_;
    mutable std::map<std::string, TrackedNode> nodes_;
    std::map<std::string, std::string> node_runs_;
    std::uint64_t next_order_ = 1;
    const lubancode::cli::Theme& theme_;
    std::function<void(const std::string&)> on_stage_started_;
};

// main 视图的滤网:workflow 节点的流式内幕(正文增量、思考、工具条目)不进
// 会话账——与子代理同规矩,main 只看阶段页脚与收官摘要,细节在 Agent 面板
// 与 run 账里。UsageUpdated 放行(token 记账不缺斤短两);不带节点标记的
// 事件(ask_user 菜单等交互宿主自己的动静)原样过。
class WorkflowMainViewSink final : public lubancode::runtime::EventSink {
public:
    explicit WorkflowMainViewSink(lubancode::runtime::EventSink& inner) : inner_(inner) {}

    void Emit(const lubancode::runtime::ServerEvent& event) override {
        if (event.payload.is_object() &&
            !event.payload.value("workflow_node_run_id", std::string()).empty() &&
            event.kind != lubancode::runtime::ServerEventKind::UsageUpdated) {
            return;
        }
        inner_.Emit(event);
    }

private:
    lubancode::runtime::EventSink& inner_;
};

// workflow 跑着时叠一层 provider/actions，不把会话里本有的子 Agent 面板拆掉。
// 析构时原样挂回；声明顺序保证 footer 先停，回调才失效。
class WorkflowPanelOverlay final {
public:
    WorkflowPanelOverlay(WorkflowAgentPanelSink& tracker, std::atomic<bool>& cancel)
        : tracker_(tracker), cancel_(cancel) {
        auto& host = lubancode::cli::SessionAgentPanelHost();
        previous_provider_ = host.provider();
        previous_actions_ = host.actions();
        previous_transcript_provider_ = host.transcript_provider();
        host.SetProvider([this]() {
            std::vector<lubancode::cli::AgentPanelEntry> entries =
                previous_provider_ ? previous_provider_() : std::vector<lubancode::cli::AgentPanelEntry>{};
            auto workflow_entries = tracker_.Entries();
            entries.insert(entries.end(), workflow_entries.begin(), workflow_entries.end());
            return entries;
        });

        lubancode::cli::AgentPanelActions actions;
        actions.cancel_task = [this](int task_id) {
            if (tracker_.OwnsTask(task_id)) return tracker_.CancelTask(task_id, cancel_);
            return previous_actions_.cancel_task ? previous_actions_.cancel_task(task_id) : false;
        };
        actions.clear_task = [this](int task_id) {
            if (tracker_.OwnsTask(task_id)) return tracker_.ClearTask(task_id);
            return previous_actions_.clear_task ? previous_actions_.clear_task(task_id) : false;
        };
        actions.cancel_all = [this]() {
            int count = tracker_.CancelAll(cancel_);
            if (previous_actions_.cancel_all) count += previous_actions_.cancel_all();
            return count;
        };
        host.SetActions(std::move(actions));
        host.SetTranscriptProvider([this](int task_id, int width, bool expanded) {
            auto lines = tracker_.TranscriptLines(task_id, width, expanded);
            if (lines.has_value()) return lines;
            return previous_transcript_provider_
                       ? previous_transcript_provider_(task_id, width, expanded)
                       : std::optional<std::vector<std::string>>{};
        });
    }

    ~WorkflowPanelOverlay() {
        auto& host = lubancode::cli::SessionAgentPanelHost();
        host.SetProvider(std::move(previous_provider_));
        host.SetActions(std::move(previous_actions_));
        host.SetTranscriptProvider(std::move(previous_transcript_provider_));
    }

    WorkflowPanelOverlay(const WorkflowPanelOverlay&) = delete;
    WorkflowPanelOverlay& operator=(const WorkflowPanelOverlay&) = delete;

private:
    WorkflowAgentPanelSink& tracker_;
    std::atomic<bool>& cancel_;
    lubancode::cli::AgentPanelProvider previous_provider_;
    lubancode::cli::AgentPanelActions previous_actions_;
    lubancode::cli::AgentPanelTranscriptProvider previous_transcript_provider_;
};

std::vector<std::string> BuiltinSlashWords() {
    std::vector<std::string> words;
    words.reserve(64);
    for (const auto& info : lubancode::cli::AllSlashCommands()) {
        words.push_back(info.name);
    }
    return words;
}

lubancode::workflow::Catalog LoadCheckedCatalog(const WorkflowCommandContext& context) {
    lubancode::workflow::Catalog catalog = lubancode::workflow::LoadCatalog(
        context.project_root, context.user_root, context.packaged_workflows);
    lubancode::workflow::DetectAliasConflicts(catalog, context.skill_names, BuiltinSlashWords());
    return catalog;
}

std::string TrimWord(const std::string& s, std::size_t& pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
    const std::size_t start = pos;
    while (pos < s.size() && s[pos] != ' ' && s[pos] != '\t') ++pos;
    return s.substr(start, pos - start);
}

std::string TrimWorkflowRunArgs(const std::string& raw) {
    std::size_t begin = 0;
    while (begin < raw.size() && (raw[begin] == ' ' || raw[begin] == '\t')) ++begin;
    // 终端里常有人把提示符/视觉分隔写成 `/alias > 需求`。`>` 不是一份
    // 有用的需求，旧解析器却拿它填唯一必填项，后文全丢。这里宽容这一种
    // 明确形状；真正以 `>` 起头且后面不跟空白的正文原样保留。
    if (begin < raw.size() && raw[begin] == '>' && begin + 1 < raw.size() &&
        (raw[begin + 1] == ' ' || raw[begin + 1] == '\t')) {
        ++begin;
        while (begin < raw.size() && (raw[begin] == ' ' || raw[begin] == '\t')) ++begin;
    }
    std::size_t end = raw.size();
    while (end > begin && (raw[end - 1] == ' ' || raw[end - 1] == '\t')) --end;
    return raw.substr(begin, end - begin);
}

nlohmann::json CoerceWorkflowInput(const nlohmann::json& properties, const std::string& name,
                                   const std::string& raw) {
    if (!properties.is_object()) return raw;
    const auto property = properties.find(name);
    if (property == properties.end() || !property->is_object()) return raw;
    const auto type = property->find("type");
    if (type == property->end() || !type->is_string() || *type == "string") return raw;

    const auto parsed = nlohmann::json::parse(raw, nullptr, false);
    if (parsed.is_discarded()) return raw;
    if (*type == "integer" && parsed.is_number_integer()) return parsed;
    if (*type == "number" && parsed.is_number()) return parsed;
    if (*type == "boolean" && parsed.is_boolean()) return parsed;
    if (*type == "array" && parsed.is_array()) return parsed;
    if (*type == "object" && parsed.is_object()) return parsed;
    return raw;
}

lubancode::workflow::CapabilityTable BuildCapabilities(const WorkflowCommandContext& context) {
    lubancode::workflow::CapabilityTable caps;
    if (context.registry != nullptr) {
        for (const auto& tool : context.registry->All()) {
            caps.tools.push_back(tool->name());
        }
    }
    caps.skills = context.skill_names;
    caps.agent_names = context.agent_names;  // 阶段 5:`agent:` 节点的编译期引用校验
    return caps;
}

void PrintUsage(const lubancode::cli::Theme& theme) {
    TermOut() << "用法: /workflow list [project|home|package|all] | show <id> | graph <id> [ascii|mermaid|json]"
              << " | validate <id> | doctor\n";
    TermOut() << "  运行与恢复: /workflow run <id> [参数...] | resume <run_id> | cancel <run_id>"
              << " | history <id>\n";
    (void)theme;
}

void PrintIssues(const std::string& title, const std::vector<lubancode::workflow::ParseIssue>& issues,
                 const lubancode::cli::Theme& theme) {
    TermOut() << theme.error << title << theme.reset << "\n";
    for (const auto& issue : issues) {
        TermOut() << "  " << theme.error << (issue.location.empty() ? "" : issue.location + ": ") << issue.message
                  << theme.reset << "\n";
    }
}

void PrintValidation(const std::string& id, const lubancode::workflow::ValidationResult& result,
                     const lubancode::cli::Theme& theme) {
    if (result.ok()) {
        TermOut() << theme.stats << "workflow " << id << ": 校验通过" << theme.reset << "\n";
        return;
    }
    TermOut() << theme.error << "workflow " << id << ": " << result.issues.size() << " 处问题" << theme.reset
              << "\n";
    for (const auto& issue : result.issues) {
        TermOut() << "  " << theme.error << (issue.path.empty() ? "" : issue.path + ": ") << issue.message
                  << theme.reset << "\n";
    }
}

}  // namespace

ParsedWorkflowCommand ParseWorkflowCommand(const std::string& args) {
    ParsedWorkflowCommand parsed;
    std::size_t pos = 0;
    const std::string verb = TrimWord(args, pos);
    if (verb == "list") {
        parsed.action = WorkflowCommandAction::List;
        parsed.scope = TrimWord(args, pos);
        if (parsed.scope != "project" && parsed.scope != "home" && parsed.scope != "all" &&
            parsed.scope != "package" && !parsed.scope.empty()) {
            parsed.action = WorkflowCommandAction::Invalid;
        }
        return parsed;
    }
    if (verb == "show" || verb == "graph" || verb == "validate") {
        parsed.id = TrimWord(args, pos);
        if (parsed.id.empty()) {
            return parsed;  // Invalid,usage 兜底
        }
        parsed.action = verb == "show"
                            ? WorkflowCommandAction::Show
                            : (verb == "graph" ? WorkflowCommandAction::Graph : WorkflowCommandAction::Validate);
        if (verb == "graph") {
            parsed.format = TrimWord(args, pos);
            if (parsed.format.empty()) parsed.format = "ascii";
            if (parsed.format != "ascii" && parsed.format != "mermaid" && parsed.format != "json") {
                parsed.action = WorkflowCommandAction::Invalid;
            }
        } else if (!TrimWord(args, pos).empty()) {
            parsed.action = WorkflowCommandAction::Invalid;
        }
        return parsed;
    }
    if (verb == "doctor") {
        parsed.action = WorkflowCommandAction::Doctor;
        return parsed;
    }
    if (verb == "run") {
        parsed.id = TrimWord(args, pos);
        if (parsed.id.empty()) return parsed;
        parsed.action = WorkflowCommandAction::Run;
        while (pos < args.size() && (args[pos] == ' ' || args[pos] == '\t')) ++pos;
        parsed.rest = args.substr(pos);
        return parsed;
    }
    if (verb == "resume" || verb == "cancel") {
        parsed.id = TrimWord(args, pos);
        if (parsed.id.empty()) return parsed;
        parsed.action = verb == "resume" ? WorkflowCommandAction::Resume : WorkflowCommandAction::Cancel;
        return parsed;
    }
    if (verb == "history") {
        parsed.action = WorkflowCommandAction::History;
        const std::string sub = TrimWord(args, pos);
        if (sub == "delete") {
            parsed.id = TrimWord(args, pos);
            const std::string yes = TrimWord(args, pos);
            parsed.confirm = yes == "yes";
            if (parsed.id.empty()) parsed.action = WorkflowCommandAction::Invalid;
        } else if (!sub.empty()) {
            parsed.id = sub;
        }
        return parsed;
    }
    if (verb == "enable" || verb == "disable") {
        parsed.id = TrimWord(args, pos);
        if (parsed.id.empty()) return parsed;
        parsed.action = WorkflowCommandAction::Enable;
        parsed.rest = verb;  // 复用:enable/disable 词
        return parsed;
    }
    if (verb == "remove") {
        parsed.id = TrimWord(args, pos);
        if (parsed.id.empty()) return parsed;
        const std::string yes = TrimWord(args, pos);
        parsed.confirm = yes == "yes";
        parsed.action = WorkflowCommandAction::Remove;
        return parsed;
    }
    if (verb == "create") {
        parsed.action = WorkflowCommandAction::Create;
        while (pos < args.size() && (args[pos] == ' ' || args[pos] == '\t')) ++pos;
        parsed.rest = args.substr(pos);
        return parsed;
    }
    if (verb == "alias") {
        parsed.action = WorkflowCommandAction::Alias;
        return parsed;
    }
    // edit/export/import 后续接线(第 6 批/app-server 合同)。
    if (!verb.empty()) {
        parsed.id = verb;
    }
    return parsed;
}

std::string ResolveWorkflowAlias(const WorkflowCommandContext& context, const std::string& alias) {
    const lubancode::workflow::Catalog catalog = LoadCheckedCatalog(context);
    const lubancode::workflow::CatalogEntry* entry = catalog.FindByAlias(alias);
    return entry != nullptr ? entry->definition.id : std::string();
}

std::vector<lubancode::cli::CompletionCandidate> BuildWorkflowSlashCompletionCandidates(
    const WorkflowCommandContext& context) {
    const lubancode::workflow::Catalog catalog = LoadCheckedCatalog(context);
    std::vector<lubancode::cli::CompletionCandidate> candidates;
    for (const auto& entry : catalog.entries) {
        // 包层(阶段 3)不抢裸 alias:packaged workflow 只走
        // /workflow run <canonical id> 正门,补全不列。
        if (entry.scope == lubancode::workflow::WorkflowScope::Package) continue;
        const auto& def = entry.definition;
        if (entry.broken || !def.enabled || def.alias.empty() ||
            catalog.disabled_aliases.count(def.alias) > 0) {
            continue;
        }
        std::string description = "Workflow · " + def.name;
        if (!def.description.empty()) description += " · " + def.description;
        candidates.push_back({"/" + def.alias, std::move(description)});
    }
    return candidates;
}

bool HandleWorkflowCommand(const std::string& args, const WorkflowCommandContext& context) {
    const ParsedWorkflowCommand parsed = ParseWorkflowCommand(args);
    const lubancode::cli::Theme& theme = *context.theme;

    if (parsed.action == WorkflowCommandAction::Invalid) {
        PrintUsage(theme);
        return true;
    }

    const lubancode::workflow::Catalog catalog = LoadCheckedCatalog(context);

    switch (parsed.action) {
        case WorkflowCommandAction::List: {
            const std::string& scope = parsed.scope.empty() ? "all" : parsed.scope;
            // scope 过滤(阶段 3 起 package 是第三档;home 只认用户级)。
            const auto scope_matches = [&](const lubancode::workflow::CatalogEntry& entry) {
                if (scope == "all") return true;
                if (scope == "package") return entry.scope == lubancode::workflow::WorkflowScope::Package;
                if (scope == "project") return entry.scope == lubancode::workflow::WorkflowScope::Project;
                return entry.scope == lubancode::workflow::WorkflowScope::User;
            };
            std::size_t shown = 0;
            for (const auto& entry : catalog.entries) {
                if (!scope_matches(entry)) {
                    continue;
                }
                ++shown;
                const std::string source = lubancode::workflow::ToString(entry.scope);
                if (entry.broken) {
                    TermOut() << theme.error << "  " << entry.definition.id << " [损坏] (" << source << ")"
                              << theme.reset << "\n";
                    for (const auto& issue : entry.issues) {
                        TermOut() << "      " << issue.location << ": " << issue.message << "\n";
                    }
                    continue;
                }
                TermOut() << "  " << entry.definition.name << "  [" << entry.definition.id << " v"
                          << entry.definition.version << "] (" << source << ")";
                // 包层(阶段 3)不抢裸 alias:列表不显示直呼名,免得许一个
                // 死的 /<alias>;canonical id 正门(上面的 [id])才通。
                if (!entry.definition.alias.empty() &&
                    entry.scope != lubancode::workflow::WorkflowScope::Package) {
                    TermOut() << "  /" << entry.definition.alias;
                    if (catalog.disabled_aliases.count(entry.definition.alias) > 0) {
                        TermOut() << theme.error << "(禁用:" << catalog.disabled_aliases.at(entry.definition.alias)
                                  << ")" << theme.reset;
                    }
                }
                if (!entry.definition.enabled) TermOut() << "  [已停用]";
                TermOut() << "\n      " << entry.definition.description << "\n";
            }
            if (shown == 0) {
                TermOut() << theme.stats << "(没有" << (scope == "all" ? "" : " " + scope)
                          << " workflow;.lubancode/workflows/ 下装一份就有)" << theme.reset << "\n";
            }
            for (const auto& conflict : catalog.conflicts) {
                TermOut() << theme.stats << "[冲突] " << conflict.alias << ": " << conflict.owner << " ("
                          << conflict.kind << ")" << theme.reset << "\n";
            }
            break;
        }
        case WorkflowCommandAction::Show: {
            const lubancode::workflow::CatalogEntry* entry = catalog.Find(parsed.id);
            if (entry == nullptr) {
                TermOut() << theme.error << "找不到 workflow: " << parsed.id << theme.reset << "\n";
                return true;
            }
            const auto& def = entry->definition;
            TermOut() << def.name << " [" << def.id << " v" << def.version << "] 来源:"
                      << lubancode::workflow::ToString(entry->scope) << "\n";
            TermOut() << "  " << def.description << "\n";
            if (!def.alias.empty()) {
                TermOut() << "  alias: /" << def.alias
                          << (catalog.disabled_aliases.count(def.alias) > 0 ? "(禁用)" : "") << "\n";
            }
            TermOut() << "  hash: " << entry->content_hash.substr(0, 12) << "  目录: "
                      << lubancode::platform::PathToUtf8(entry->dir) << "\n";
            TermOut() << "  limits: 并发 " << def.limits.max_concurrency << " · 节点 " << def.nodes.size() << "/"
                      << def.limits.max_nodes << " · 步数 " << def.limits.max_steps << " · 时限 "
                      << def.limits.timeout_secs << "s\n";
            TermOut() << "  节点:\n";
            for (const auto& node : def.nodes) {
                TermOut() << "    " << lubancode::workflow::NodeSummaryLine(node) << "\n";
            }
            break;
        }
        case WorkflowCommandAction::Graph: {
            const lubancode::workflow::CatalogEntry* entry = catalog.Find(parsed.id);
            if (entry == nullptr) {
                TermOut() << theme.error << "找不到 workflow: " << parsed.id << theme.reset << "\n";
                return true;
            }
            if (parsed.format == "mermaid") {
                TermOut() << lubancode::workflow::RenderMermaidGraph(entry->definition);
            } else if (parsed.format == "json") {
                TermOut() << entry->definition.normalized.dump(2) << "\n";
            } else {
                TermOut() << lubancode::workflow::RenderAsciiGraph(entry->definition);
            }
            break;
        }
        case WorkflowCommandAction::Validate: {
            const lubancode::workflow::CatalogEntry* entry = catalog.Find(parsed.id);
            if (entry == nullptr) {
                TermOut() << theme.error << "找不到 workflow: " << parsed.id << theme.reset << "\n";
                return true;
            }
            if (entry->broken) {
                PrintIssues("workflow " + parsed.id + " 定义解析失败", entry->issues, theme);
                return true;
            }
            const lubancode::workflow::CapabilityTable caps = BuildCapabilities(context);
            PrintValidation(parsed.id,
                            lubancode::workflow::ValidateDefinition(entry->definition, caps), theme);
            break;
        }
        case WorkflowCommandAction::Run: {
            // 执行器由会话层经 ResolveWorkflowRunContext 注入(第 4 批的
            // 宿主执行器);这里只做编排。没注入(如测试)给一句明话。
            TermOut() << theme.stats << "run <id> 的执行器装配由会话层注入;本路径给测试与 "
                      << "app-server 用" << theme.reset << "\n";
            break;
        }
        case WorkflowCommandAction::Resume: {
            if (context.home_lubancode.has_value()) {
                const std::filesystem::path run_dir =
                    *context.home_lubancode / "workflow-runs" / parsed.id;
                std::error_code ec;
                if (!std::filesystem::exists(run_dir, ec)) {
                    TermOut() << theme.error << "run 不存在: " << parsed.id << theme.reset << "\n";
                    break;
                }
                // 恢复也走会话层执行器装配;这里先给账面。
                TermOut() << theme.stats << "run " << parsed.id << " 可恢复;执行入口与 run 同一道装配"
                          << theme.reset << "\n";
            }
            break;
        }
        case WorkflowCommandAction::Cancel: {
            TermOut() << theme.error << "cancel:跑动中的 run 经 ESC/打断通道取消(首版同步 run,"
                      << "命令返回时 run 已终态)" << theme.reset << "\n";
            break;
        }
        case WorkflowCommandAction::History: {
            if (context.home_lubancode.has_value()) {
                const std::filesystem::path runs_root = *context.home_lubancode / "workflow-runs";
                const auto runs = lubancode::workflow::ListRuns(runs_root);
                std::size_t shown = 0;
                for (const auto& run : runs) {
                    if (!parsed.id.empty() && run.workflow_id != parsed.id) continue;
                    ++shown;
                    TermOut() << "  " << run.run_id << "  " << run.workflow_id << " v" << run.workflow_version
                              << "  " << (run.final_state.empty() ? "(未完成)" : run.final_state) << "  "
                              << run.started_at << "\n";
                }
                if (shown == 0) {
                    TermOut() << theme.stats << "(没有运行账)" << theme.reset << "\n";
                }
            }
            break;
        }
        case WorkflowCommandAction::Enable: {
            const lubancode::workflow::CatalogEntry* entry = catalog.Find(parsed.id);
            if (entry == nullptr) {
                TermOut() << theme.error << "找不到 workflow: " << parsed.id << theme.reset << "\n";
                break;
            }
            const bool enable = parsed.rest == "enable";
            auto result = lubancode::workflow::SetWorkflowEnabled(entry->dir, enable);
            if (result.has_value()) {
                TermOut() << theme.stats << (enable ? "已启用" : "已停用") << "(直呼 alias "
                          << (enable ? "恢复" : "不再响应") << ")" << theme.reset << "\n";
            } else {
                TermOut() << theme.error << result.error() << theme.reset << "\n";
            }
            break;
        }
        case WorkflowCommandAction::Remove: {
            const lubancode::workflow::CatalogEntry* entry = catalog.Find(parsed.id);
            if (entry == nullptr) {
                TermOut() << theme.error << "找不到 workflow: " << parsed.id << theme.reset << "\n";
                break;
            }
            if (!parsed.confirm) {
                TermOut() << theme.error << "remove 要确认(只删定义,不动运行账): /workflow remove "
                          << parsed.id << " yes" << theme.reset << "\n";
                break;
            }
            auto result = lubancode::workflow::RemoveWorkflow(entry->dir);
            if (result.has_value()) {
                TermOut() << theme.stats << "已移除定义: " << parsed.id << "(运行账另走 /workflow history)"
                          << theme.reset << "\n";
            } else {
                TermOut() << theme.error << result.error() << theme.reset << "\n";
            }
            break;
        }
        case WorkflowCommandAction::Create: {
            // 第 5 批:向导走会话层的模型装配(意图提取经 IntentCompiler
            // 注入);这里先给用法骨架,第 6 批接模型。
            TermOut() << "用法: /workflow create <自然语言描述>\n"
                      << "  例: /workflow create 论文检索:四路并行查 arXiv/DBLP/Scholar/AnySearch,"
                      << "去重排序写成 Markdown\n"
                      << "  向导会追问缺口,预览图与将写文件,确认后落进项目或用户目录。\n";
            break;
        }
        case WorkflowCommandAction::Alias: {
            for (const auto& entry : catalog.entries) {
                // 包层(阶段 3)不抢裸 alias:直呼名册只列 standalone;
                // packaged workflow 走 /workflow run <canonical id> 正门。
                if (entry.scope == lubancode::workflow::WorkflowScope::Package) continue;
                if (entry.broken || entry.definition.alias.empty()) continue;
                const bool disabled = catalog.disabled_aliases.count(entry.definition.alias) > 0;
                TermOut() << "  /" << entry.definition.alias << " -> " << entry.definition.id << " ("
                          << lubancode::workflow::ToString(entry.scope) << ")"
                          << (disabled ? theme.error + " [禁用:跨类撞名]" + theme.reset : "") << "\n";
            }
            if (catalog.disabled_aliases.empty() && catalog.entries.empty()) {
                TermOut() << theme.stats << "(没有可直呼的 workflow alias)" << theme.reset << "\n";
            }
            break;
        }
        case WorkflowCommandAction::Doctor: {
            std::size_t broken = 0;
            std::size_t ok = 0;
            for (const auto& entry : catalog.entries) {
                if (entry.broken) {
                    ++broken;
                    PrintIssues(entry.definition.id + " 解析失败", entry.issues, theme);
                    continue;
                }
                ++ok;
                PrintValidation(entry.definition.id,
                                lubancode::workflow::ValidateDefinition(entry.definition, BuildCapabilities(context)),
                                theme);
            }
            TermOut() << theme.stats << "workflow doctor: " << ok << " 份可用, " << broken << " 份损坏, "
                      << catalog.conflicts.size() << " 处冲突, " << catalog.disabled_aliases.size()
                      << " 个 alias 被禁用" << theme.reset << "\n";
            for (const auto& [alias, reason] : catalog.disabled_aliases) {
                TermOut() << "  /" << alias << ": " << reason << "\n";
            }
            break;
        }
    }
    return true;
}

std::string RunWorkflowById(const WorkflowCommandContext& context, const std::string& id,
                            const std::string& raw_args,
                            const std::map<lubancode::workflow::NodeKind,
                                           std::shared_ptr<lubancode::workflow::NodeExecutor>>& executors,
                            const std::atomic<bool>* cancel_token) {
    const lubancode::cli::Theme& theme = *context.theme;
    const lubancode::workflow::Catalog catalog = LoadCheckedCatalog(context);
    const lubancode::workflow::CatalogEntry* entry = catalog.Find(id);
    if (entry == nullptr) {
        return theme.error + "找不到 workflow: " + id + theme.reset;
    }

    // 参数解析:按 input_schema 的 properties 逐个 --name value / 位置参。
    // 直呼别名没带必填项时，终端按字段 description 补问；无交互宿主
    // 才把缺项留给 runtime，照旧返回结构化错。
    nlohmann::json inputs = nlohmann::json::object();
    const nlohmann::json props = entry->definition.inputs.contains("properties")
                                     ? entry->definition.inputs["properties"]
                                     : nlohmann::json::object();
    const std::string clean_args = TrimWorkflowRunArgs(raw_args);
    // 只有一项必填 string 时，alias 后面通常就是整段旨意。只要不是以
    // `--` 起头的显式参数写法，便一字不拆地交给它；需求正文里的
    // `--dry-run`、引号与连续空格都不该被命令行解析器吃掉。
    std::optional<std::string> natural_language_field;
    if (entry->definition.inputs.contains("required") &&
        entry->definition.inputs["required"].is_array() &&
        entry->definition.inputs["required"].size() == 1 &&
        entry->definition.inputs["required"][0].is_string()) {
        const std::string candidate = entry->definition.inputs["required"][0].get<std::string>();
        const auto property = props.is_object() ? props.find(candidate) : props.end();
        if (property == props.end() || !property->is_object() ||
            !property->contains("type") || (*property)["type"] == "string") {
            natural_language_field = candidate;
        }
    }
    const bool explicit_named_args = clean_args.rfind("--", 0) == 0;
    const bool natural_language_bound =
        natural_language_field.has_value() && !clean_args.empty() && !explicit_named_args;
    if (natural_language_bound) {
        inputs[*natural_language_field] = clean_args;
    }
    std::vector<std::string> positional;
    std::size_t pos = 0;
    while (!natural_language_bound && pos < clean_args.size()) {
        while (pos < clean_args.size() && (clean_args[pos] == ' ' || clean_args[pos] == '\t')) ++pos;
        if (pos >= clean_args.size()) break;
        if (clean_args.compare(pos, 2, "--") == 0) {
            pos += 2;
            const std::size_t name_start = pos;
            while (pos < clean_args.size() && clean_args[pos] != ' ' && clean_args[pos] != '\t' &&
                   clean_args[pos] != '=') {
                ++pos;
            }
            std::string name = clean_args.substr(name_start, pos - name_start);
            if (pos < clean_args.size() && clean_args[pos] == '=') ++pos;
            while (pos < clean_args.size() && (clean_args[pos] == ' ' || clean_args[pos] == '\t')) ++pos;
            const std::size_t value_start = pos;
            const bool quoted = pos < clean_args.size() && clean_args[pos] == '"';
            if (quoted) {
                ++pos;
                while (pos < clean_args.size() && clean_args[pos] != '"') ++pos;
                if (pos < clean_args.size()) ++pos;
            } else {
                while (pos < clean_args.size() && clean_args[pos] != ' ' && clean_args[pos] != '\t') ++pos;
            }
            std::string value = clean_args.substr(value_start, pos - value_start);
            if (quoted && value.size() >= 2) value = value.substr(1, value.size() - 2);
            if (!name.empty()) inputs[name] = CoerceWorkflowInput(props, name, value);
            continue;
        }
        const std::size_t start = pos;
        const bool quoted = clean_args[pos] == '"';
        if (quoted) {
            ++pos;
            while (pos < clean_args.size() && clean_args[pos] != '"') ++pos;
            if (pos < clean_args.size()) ++pos;
        } else {
            while (pos < clean_args.size() && clean_args[pos] != ' ' && clean_args[pos] != '\t') ++pos;
        }
        std::string token = clean_args.substr(start, pos - start);
        if (quoted && token.size() >= 2) token = token.substr(1, token.size() - 2);
        positional.push_back(token);
    }
    // 位置参依序填 required。只有一个 string 必填项时，它通常就是自然
    // 语言任务；把所有位置词重新连成整句，不能只取第一个词。
    if (entry->definition.inputs.contains("required") && entry->definition.inputs["required"].is_array()) {
        std::vector<std::string> missing_required;
        for (const auto& field : entry->definition.inputs["required"]) {
            if (field.is_string() && !inputs.contains(field.get<std::string>())) {
                missing_required.push_back(field.get<std::string>());
            }
        }
        if (missing_required.size() == 1 && !positional.empty()) {
            const std::string& name = missing_required.front();
            const auto property = props.is_object() ? props.find(name) : props.end();
            const bool string_field = property == props.end() || !property->is_object() ||
                                      !property->contains("type") || (*property)["type"] == "string";
            if (string_field) {
                std::string joined;
                for (const auto& token : positional) {
                    if (!joined.empty()) joined += ' ';
                    joined += token;
                }
                inputs[name] = joined;
                positional.clear();
            }
        }
        std::size_t arg_index = 0;
        for (const auto& field : entry->definition.inputs["required"]) {
            if (!field.is_string()) continue;
            const std::string name = field.get<std::string>();
            if (inputs.contains(name) || arg_index >= positional.size()) continue;
            inputs[name] = CoerceWorkflowInput(props, name, positional[arg_index++]);
        }
        if (context.request_input) {
            for (const auto& field : entry->definition.inputs["required"]) {
                if (!field.is_string()) continue;
                const std::string name = field.get<std::string>();
                if (inputs.contains(name)) continue;
                const nlohmann::json schema = props.is_object() && props.contains(name) && props[name].is_object()
                                                  ? props[name]
                                                  : nlohmann::json::object();
                const auto answer = context.request_input(name, schema);
                if (!answer.has_value() || answer->empty()) {
                    return theme.stats + "workflow 已取消：没有收到 " + name + theme.reset + "\n";
                }
                inputs[name] = CoerceWorkflowInput(props, name, *answer);
            }
        }
    }

    lubancode::workflow::RuntimeOptions options;
    options.executors = executors;
    options.event_sink = context.event_sink;
    options.thread_id = context.thread_id;
    options.id_authority = context.id_authority;
    if (context.home_lubancode.has_value()) {
        options.runs_root = *context.home_lubancode / "workflow-runs";
    }
    if (context.on_run_start) context.on_run_start();
    lubancode::workflow::WorkflowRuntime runtime(std::move(options));
    const lubancode::workflow::WorkflowRunSummary summary =
        runtime.Run(entry->definition, lubancode::workflow::RunInputs(inputs), cancel_token);

    std::ostringstream out;
    const char* run_mark = summary.state == lubancode::workflow::RunState::Succeeded ? "✓" : "×";
    out << run_mark << " " << entry->definition.name << " [" << entry->definition.id << "] " << theme.stats
        << lubancode::workflow::ToString(summary.state) << theme.reset << " · "
        << summary.duration_ms / 1000 << "." << (summary.duration_ms % 1000) / 100 << "s · tokens "
        << summary.tokens_used << "\n";
    for (const auto& [node_id, record] : summary.nodes) {
        const char* mark = record.state == lubancode::workflow::NodeState::Succeeded  ? "✓"
                           : record.state == lubancode::workflow::NodeState::Failed    ? "×"
                           : record.state == lubancode::workflow::NodeState::Skipped   ? "-"
                                                                                       : "…";
        const auto node = entry->definition.node_map.find(node_id);
        const std::string label = node != entry->definition.node_map.end() && !node->second.label.empty()
                                      ? node->second.label
                                      : node_id;
        out << "  " << mark << " " << label;
        if (label != node_id) out << theme.stats << "  " << node_id << theme.reset;
        if (!record.error_code.empty()) {
            out << "  " << record.error_code << " " << record.error_message.substr(0, 120);
        }
        out << "\n";
    }
    if (!summary.unavailable_sources.empty()) {
        out << theme.error << "  缺失来源: ";
        for (const auto& source : summary.unavailable_sources) out << source << " ";
        out << theme.reset << "\n";
    }
    if (!summary.result.empty()) {
        std::string rendered = FormatWorkflowRunResult(summary.result);
        constexpr std::size_t kResultPreviewBytes = 8000;
        if (rendered.size() > kResultPreviewBytes) {
            rendered.resize(lubancode::platform::Utf8PrefixBoundary(rendered, kResultPreviewBytes));
            rendered += "\n…(结果过长，已截断；完整账见 /workflow history)";
        }
        out << "\n奏报\n" << rendered << "\n";
    } else if (!entry->definition.result.empty() &&
               summary.state == lubancode::workflow::RunState::Succeeded) {
        out << theme.error << "  结果为空：请检查 workflow 的 result 映射" << theme.reset << "\n";
    }
    if (!summary.error_message.empty() && summary.state != lubancode::workflow::RunState::Succeeded) {
        out << theme.error << "  " << summary.error_code << ": " << summary.error_message << theme.reset << "\n";
    }
    return out.str();
}


// ---------------------------------------------------------------------------
// 命令分派注册制(会话终章):workflow 域的分派位。/workflow 正门与
// /<alias> 直呼(Unknown 兜底)共用同一份 catalog/执行器装配。
// ---------------------------------------------------------------------------
namespace {

// catalog 上下文的现场装配(两案共用):project_root 现取 cwd,user_root/
// home_lubancode 按主目录,能力表取当前主表,技能名做撞名检查。
lubancode::app::WorkflowCommandContext BuildWorkflowCatalogContext(SlashDispatchContext& ctx) {
    lubancode::app::WorkflowCommandContext wf_ctx;
    wf_ctx.project_root = std::filesystem::current_path();
    wf_ctx.user_root = ctx.home_dir->has_value()
                           ? std::optional<std::filesystem::path>(
                                 lubancode::tools::Utf8ToPath(**ctx.home_dir))
                           : std::nullopt;
    wf_ctx.home_lubancode = ctx.home_lubancode->has_value()
                                ? std::optional<std::filesystem::path>(
                                      lubancode::tools::Utf8ToPath(**ctx.home_lubancode))
                                : std::nullopt;
    // 统一 Package 封装单阶段 3:包层成品件从会话钉快照折来(canonical id
    // 登册;快照缺席 = 空表,行为与从前一致)。
    if (ctx.package_mount != nullptr) {
        wf_ctx.packaged_workflows = lubancode::package::MountWorkflowSources(*ctx.package_mount);
    }
    wf_ctx.registry = ctx.registry;
    for (const auto& skill : *ctx.skills) {
        wf_ctx.skill_names.push_back(skill.name);
    }
    // 阶段 5:`agent:` 节点的编译期引用校验表——AgentCatalog 现扫(与
    // agent 工具派发口同一套根),可用条目名(canonical+裸名)进表。
    if (ctx.agent_tool != nullptr) {
        const lubancode::agent::AgentCatalog catalog = lubancode::agent::LoadAgentCatalog(
            ComputeAgentScanRoots(ctx.package_mount != nullptr
                                      ? lubancode::package::MountAgentEntries(*ctx.package_mount)
                                      : std::vector<lubancode::agent::PackagedAgentEntry>{}));
        for (const lubancode::agent::AgentCatalogEntry* entry : catalog.Available()) {
            wf_ctx.agent_names.push_back(entry->name);
        }
    }
    wf_ctx.theme = ctx.theme;
    wf_ctx.request_input = [theme = ctx.theme](const std::string& field,
                                               const nlohmann::json& schema)
        -> std::optional<std::string> {
        // workflow 的忙时 footer 已开着；补问必填项时暂让整块屏面，答完
        // 再由作用域画回，提交后的 composer 不会只剩一枚裸光标。
        const lubancode::cli::StreamFooterSuspendScope footer_suspend;
        const std::string question = schema.value("description", "请补充 " + field + "：");
        {
            std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
            TermOut() << "\n" << question << "\n";
            TermOut().flush();
        }
        lubancode::cli::ReadExitReason reason = lubancode::cli::ReadExitReason::Cancel;
        return lubancode::cli::ReadLine(theme->prompt + "> " + theme->reset, *theme,
                                        /*esc_rejects=*/true, /*composer=*/true, &reason);
    };
    return wf_ctx;
}

// run/alias 直呼共用的执行器装配(终端接线收尾单收口,与正门同源)。
lubancode::app::WorkflowExecutorContext BuildWorkflowExecutorContext(SlashDispatchContext& ctx) {
    lubancode::app::WorkflowExecutorContext wf_exec;
    wf_exec.registry = ctx.registry;
    wf_exec.backend = ctx.real_backend;
    wf_exec.build_tool_options = ctx.build_workflow_tool_options;
    wf_exec.build_agent_callbacks = ctx.build_workflow_agent_callbacks;
    wf_exec.provider = *ctx.active_provider;
    wf_exec.model = *ctx.current_model;
    wf_exec.effort = *ctx.current_think;
    wf_exec.model_catalog = ctx.model_catalog;
    wf_exec.agent_profile = ctx.main_agent->runtime_profile();
    wf_exec.event_sink = ctx.session_events;
    wf_exec.thread_id = ctx.session_runtime->thread_id();
    wf_exec.id_authority = &ctx.session_runtime->ids();
    wf_exec.interaction_broker = std::make_shared<TerminalWorkflowInteractionBroker>(*ctx.theme);
    wf_exec.skills = ctx.skills;
    // 阶段 5:`agent: <name>` 节点的解析口来源——会话级 agent 工具(查名
    // 与环境账都是它的同一只)。系统提示材料照 AgentTool 的 setter 同源
    // 折(prompts_dir/project_instructions/skills_segment 与会话栈同一份,
    // 项目层根与包层根同式现算)——两路系统提示逐字节一致的前提。
    // 阶段 6:Package 快照跑一趟钉一份(半场 reload 不换这趟的账)。
    wf_exec.agent_tool = ctx.agent_tool;
    if (ctx.package_snapshot_provider != nullptr) {
        wf_exec.package_snapshot = ctx.package_snapshot_provider();
    }
    wf_exec.subagent_prompt_material.cwd =
        ctx.prompt_options != nullptr ? ctx.prompt_options->cwd : std::string();
    wf_exec.subagent_prompt_material.prompts_dir = ctx.prompts_dir != nullptr ? *ctx.prompts_dir : std::string();
    wf_exec.subagent_prompt_material.project_prompts_dir = lubancode::app::ComputeProjectPromptsRoot();
    wf_exec.subagent_prompt_material.project_instructions =
        ctx.prompt_options != nullptr ? ctx.prompt_options->project_instructions : std::string();
    wf_exec.subagent_prompt_material.skills_segment =
        ctx.prompt_options != nullptr ? ctx.prompt_options->skills_segment : std::string();
    if (ctx.package_mount != nullptr) {
        wf_exec.subagent_prompt_material.package_profile_roots =
            lubancode::package::MountProfileRoots(*ctx.package_mount);
    }
    wf_exec.resolve_llm_binding = [router = ctx.model_router](
                                      const lubancode::workflow::WorkflowNode& node)
        -> std::optional<lubancode::workflow::LlmExecutor::Binding> {
        if (router == nullptr || node.model_role.empty() || node.model_role == "normal") {
            return std::nullopt;
        }
        lubancode::agent::TaskKind kind;
        if (node.model_role == "lao" || node.model_role == "plan") {
            kind = lubancode::agent::TaskKind::Plan;
        } else if (node.model_role == "cheap") {
            kind = lubancode::agent::TaskKind::Classification;
        } else {
            return std::nullopt;
        }
        auto routed = router->RouteDetached(kind);
        lubancode::workflow::LlmExecutor::Binding binding;
        binding.model = routed.route.model;
        binding.reasoning_effort = routed.route.effort;
        if (routed.backend) {
            binding.owned_backend = std::shared_ptr<lubancode::api::Backend>(std::move(routed.backend));
            binding.backend = binding.owned_backend.get();
        }
        return binding;
    };
    return wf_exec;
}

std::string RunWorkflowFromTerminal(SlashDispatchContext& ctx,
    const lubancode::app::WorkflowCommandContext& wf_ctx,
    const std::string& workflow_id, const std::string& raw_args) {
    lubancode::app::WorkflowExecutorContext wf_exec = BuildWorkflowExecutorContext(ctx);
    std::atomic<bool> workflow_cancel{false};
    WorkflowTerminalRunScope terminal_run(*ctx.theme, ctx.spinner_enabled, workflow_cancel);
    WorkflowAgentPanelSink workflow_panel(
        *ctx.theme, [&terminal_run](const std::string& label) { terminal_run.BeginStage(label); });
    WorkflowPanelOverlay panel_overlay(workflow_panel, workflow_cancel);
    lubancode::runtime::FanoutEventSink workflow_events;
    workflow_events.Add(&workflow_panel);
    std::optional<WorkflowMainViewSink> main_view;
    if (ctx.session_events != nullptr) {
        main_view.emplace(*ctx.session_events);
        workflow_events.Add(&*main_view);
    }

    lubancode::app::WorkflowCommandContext run_ctx = wf_ctx;
    run_ctx.on_run_start = [&terminal_run] { terminal_run.Start(); };
    run_ctx.event_sink = &workflow_events;
    run_ctx.thread_id = ctx.session_runtime->thread_id();
    run_ctx.id_authority = &ctx.session_runtime->ids();
    // agent 子回合与 workflow 节点事件走同一扇分线器；面板账收全量,
    // 会话账经 main_view 滤网——节点内幕留在面板,main 不再倒原始 JSON。
    wf_exec.event_sink = &workflow_events;
    wf_exec.steering = [&workflow_panel](const lubancode::workflow::NodeExecRequest& request) {
        return workflow_panel.TakeSteering(request);
    };
    std::string rendered = lubancode::app::RunWorkflowById(
        run_ctx, workflow_id, raw_args,
        lubancode::app::BuildWorkflowExecutors(run_ctx, wf_exec, workflow_id),
        terminal_run.cancel_token());
    workflow_panel.ClosePendingMessages();
    return rendered;
}

}  // namespace

CommandFlow HandleSlashWorkflow(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    // Workflows 自然语言编排单:正门 /workflow。catalog 现扫现用,不占会话
    // 状态;能力表取自当前主表(此刻挂着的工具)。
    lubancode::app::WorkflowCommandContext wf_ctx = BuildWorkflowCatalogContext(ctx);
    const lubancode::app::ParsedWorkflowCommand wf_parsed = lubancode::app::ParseWorkflowCommand(parsed.args);
    if (wf_parsed.action == lubancode::app::WorkflowCommandAction::Run) {
        // run:执行器装配与 alias 直呼共用一份；footer 收妥后摘要再落屏。
        TermOut() << RunWorkflowFromTerminal(ctx, wf_ctx, wf_parsed.id, wf_parsed.rest);
        return CommandFlow::Continue;
    }
    HandleWorkflowCommand(parsed.args, wf_ctx);
    if (ctx.refresh_workflow_completions) ctx.refresh_workflow_completions();
    return CommandFlow::Continue;
}

CommandFlow HandleSlashUnknown(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    // Workflows 单:不认得的 / 词先查 WorkflowCatalog——查着了是 /<alias>
    // 直呼(整行参数按 input_schema 解析,不当一坨 prompt),查不着才打
    // "不认得"。内建词永远居首,撞名禁用的 alias 也不接(只留 /workflow
    // run 正门)。
    if (!parsed.alias_word.empty()) {
        lubancode::app::WorkflowCommandContext wf_ctx = BuildWorkflowCatalogContext(ctx);
        const std::string wf_id = ResolveWorkflowAlias(wf_ctx, parsed.alias_word);
        if (!wf_id.empty()) {
            // 与 /workflow run 同一道终端外壳和执行器装配。
            TermOut() << RunWorkflowFromTerminal(ctx, wf_ctx, wf_id, parsed.args);
            return CommandFlow::Continue;
        }
    }
    TermOut() << trf("error.unknown_command", parsed.raw_word) << "\n";
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
