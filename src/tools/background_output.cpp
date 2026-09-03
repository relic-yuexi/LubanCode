#include "tools/background_output.hpp"

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>

#include "tools/background_tasks.hpp"
#include "tools/task_ledger.hpp"
#include "tools/tool_text.hpp"  // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/

namespace lubancode::tools {

namespace {

// 把一条任务的摘要拼成一行文本(List/detail 共用)。退出码不知道便写
// unknown——不借 0 冒充成功(进程生命线单 P0 的口径)。
void AppendTaskSummary(std::ostringstream& oss, const BackgroundTaskInfo& t) {
    oss << "[#" << t.task_id << "] " << BackgroundTaskStatusLabel(t.status);
    if (t.status != BackgroundTaskStatus::Running && t.status != BackgroundTaskStatus::Stopping) {
        oss << " (exit ";
        if (t.exit.exit_code.has_value()) {
            oss << *t.exit.exit_code;
        } else {
            oss << "unknown";
        }
        if (t.exit.signal.has_value()) {
            oss << ", signal " << *t.exit.signal;
        }
        oss << ")";
    }
    oss << "  PID=" << t.pid << "\n  命令: " << t.command << "\n  日志: " << t.log_path << "\n";
}

// 面板后台子代理的摘要一行(后台代理管控三连 bug 单,Bug B):口径与面板
// 坞行对齐——编号、状态(运行中/停止中/终态)、类型、title、工具与 token
// 账。stop_requested 单独标"停止中",与面板"停止中"回执同词。
void AppendAgentSummary(std::ostringstream& oss, const AgentTaskSummary& s) {
    oss << "[子代理 #" << s.id << "] ";
    if (IsAliveTaskState(s.state)) {
        oss << (s.stop_requested ? "停止中" : "运行中");
    } else {
        oss << StateShortLabel(s.state);
        if (s.outcome_reason != TaskOutcomeReason::None) {
            oss << "(" << ReasonShortLabel(s.outcome_reason) << ")";
        }
    }
    oss << "  " << s.agent_type;
    if (!s.title.empty()) {
        oss << " · " << s.title;
    }
    oss << "\n  工具调用 " << s.tool_call_count << " 次 · " << s.total_input_tokens() << " tokens(完整输入)";
    if (!s.activity.last_tool_name.empty()) {
        oss << " · 最后工具 " << s.activity.last_tool_name;
    }
    if (s.delivered) {
        oss << " · 结果已交回";
    }
    oss << "\n";
}

// 严格数字解析(全串都得是数字):面板编号是 int,"2abc" 这类不得被 atoi
// 悄悄截成 2 去冒充面板 id。
std::optional<int> ParsePanelTaskId(const std::string& task_id) {
    if (task_id.empty() || task_id.size() > 9) {
        return std::nullopt;
    }
    for (const char c : task_id) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
    }
    return std::atoi(task_id.c_str());
}

// provider 换台账快照(接了才有;没接返回空表,老行为)。
std::vector<AgentTaskSummary> AgentSummaries(const std::function<TaskLedger*()>& provider) {
    if (!provider) {
        return {};
    }
    TaskLedger* ledger = provider();
    if (ledger == nullptr) {
        return {};
    }
    return ledger->Summaries();
}

}  // namespace

std::string BackgroundOutputTool::name() const {
    return "background_output";
}

std::string BackgroundOutputTool::description() const {
    // 文案在 src/prompts/tools/<语言>/background_output.md,兜底是迁移前的原文。
    return ToolText("background_output", "description",
                    "查后台命令(run_command run_in_background:true 起的那些)的运行状态和输出。"
                    "不给 task_id 就列出全部后台任务的摘要:task_id、状态(运行中/完成/失败/已停止)、"
                    "命令、PID、日志文件路径;会话里有后台子代理(agent 工具 background 派的)也一并列出"
                    "(编号与面板/agent 回执里的 #N 一致)。给 task_id 就返回该任务的详情——后台命令带"
                    "日志尾部 tail_lines 行(默认 50),后台子代理带状态与进度摘要。"
                    "任务还在跑也能读,文件允许边写边读。"
                    "起完一个后台命令或后台子代理后,用它查进度/结果,不用自己再拼 tail 命令。");
}

nlohmann::json BackgroundOutputTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();

    nlohmann::json task_id_prop = nlohmann::json::object();
    task_id_prop["type"] = "string";
    task_id_prop["description"] =
        ToolText("background_output", "param.task_id",
                 "要查的后台任务 id(run_command 后台返回的那个编号字符串)。"
                 "不给就列出所有后台任务的摘要。");
    properties["task_id"] = task_id_prop;

    nlohmann::json tail_prop = nlohmann::json::object();
    tail_prop["type"] = "integer";
    tail_prop["description"] = ToolText("background_output", "param.tail_lines",
                                        "读日志文件的末尾几行,默认 50。给 task_id 时才用;<=0 表示读全文(上限 64KB)。");
    properties["tail_lines"] = tail_prop;

    schema["properties"] = properties;
    return schema;
}

Tool::Result BackgroundOutputTool::execute(const nlohmann::json& input) {
    std::string task_id;
    if (auto it = input.find("task_id"); it != input.end() && !it->is_null()) {
        if (!it->is_string()) {
            return {"task_id 得是字符串", true};
        }
        task_id = it->get<std::string>();
    }

    int tail_lines = 50;
    if (auto it = input.find("tail_lines"); it != input.end() && !it->is_null()) {
        // 64 位取值 + 范围检查:JSON 装得下超大整数,直接 get<int> 会窄化
        // 走样;负值语义是"读全文",超大的正值夹到单任务读取上限(64KB 语义
        // 上限,再大也没东西可读)。
        if (!it->is_number_integer()) {
            return {"tail_lines 得是整数", true};
        }
        const std::int64_t raw = it->get<std::int64_t>();
        if (raw < 0) {
            tail_lines = 0;  // 与"<=0 读全文(上限 64KB)"的既有语义对齐
        } else if (raw > 1000000) {
            tail_lines = 1000000;  // 64KB 读档里塞不下更多行,夹紧即可
        } else {
            tail_lines = static_cast<int>(raw);
        }
    }

    auto& registry = BackgroundTaskRegistry::Instance();
    const std::vector<AgentTaskSummary> agents = AgentSummaries(agent_ledger_provider_);

    // 不给 task_id:列全部——后台命令与后台子代理两本账合并(Bug B:面板有
    // 活代理却答"当前没有后台任务"就是两本账裂开的症状)。
    if (task_id.empty()) {
        const auto tasks = registry.List();
        if (tasks.empty() && agents.empty()) {
            return {"当前没有后台任务。", false};
        }
        std::ostringstream oss;
        bool first_section = true;
        if (!tasks.empty()) {
            oss << "后台命令共 " << tasks.size() << " 个:\n\n";
            for (const auto& t : tasks) {
                AppendTaskSummary(oss, t);
                oss << "\n";
            }
            first_section = false;
        }
        if (!agents.empty()) {
            if (!first_section) {
                oss << "\n";
            }
            oss << "后台子代理共 " << agents.size() << " 只(agent 工具 background 派的,编号与面板显示一致):\n\n";
            for (const auto& s : agents) {
                AppendAgentSummary(oss, s);
                oss << "\n";
            }
        }
        return {oss.str(), false};
    }

    // 给 task_id:查单个 + 读输出。先认命令登记簿,miss 再认面板后台子代理。
    const auto info = registry.Get(task_id);
    if (!info.has_value()) {
        const auto panel_id = ParsePanelTaskId(task_id);
        if (panel_id.has_value()) {
            for (const auto& s : agents) {
                if (s.id == *panel_id) {
                    std::ostringstream oss;
                    AppendAgentSummary(oss, s);
                    oss << "  说明: 编号 " << s.id
                        << " 是后台子代理(无进程日志可读);面板与 agent_view 可看实时消息账。\n";
                    return {oss.str(), false};
                }
            }
        }
        return {"找不到 task_id=" + task_id + " 的后台任务(后台命令与后台子代理两本账都没有这个编号)。", true};
    }

    std::ostringstream oss;
    AppendTaskSummary(oss, *info);
    oss << "\n";
    const std::string output = registry.ReadOutput(task_id, tail_lines);
    if (output.empty()) {
        oss << "(日志文件暂时没有内容,进程可能还没开始写。)";
    } else {
        oss << "日志末尾 " << tail_lines << " 行:\n" << output;
    }
    return {oss.str(), false};
}

// ---------------------------------------------------------------------------

std::string StopBackgroundTool::name() const {
    return "stop_background";
}

std::string StopBackgroundTool::description() const {
    // 文案在 src/prompts/tools/<语言>/stop_background.md,兜底是迁移前的原文。
    return ToolText("stop_background", "description",
                    "停掉一个后台命令(run_command run_in_background:true 起的)或一只后台子代理"
                    "(agent 工具 background 派的——task_id 用面板与 agent 回执里显示的那个编号)。"
                    "命令:Windows 上 TerminateProcess 根进程,POSIX 上 kill 杀整个进程组;"
                    "子代理:发停止信号,整棵子树级联收口。已完成的任务不会重复杀。"
                    "长命进程(dev server、watch、build)跑够了、或者后台代理烧 token 要止损,用它。");
}

nlohmann::json StopBackgroundTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();
    nlohmann::json task_id_prop = nlohmann::json::object();
    task_id_prop["type"] = "string";
    task_id_prop["description"] =
        ToolText("stop_background", "param.task_id", "要停的后台任务 id(run_command 后台返回的那个编号字符串)。");
    properties["task_id"] = task_id_prop;

    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"task_id"});
    return schema;
}

Tool::Result StopBackgroundTool::execute(const nlohmann::json& input) {
    if (!input.contains("task_id") || !input.at("task_id").is_string()) {
        return {"缺少必填参数 task_id(字符串)", true};
    }
    const std::string task_id = input.at("task_id").get<std::string>();
    if (task_id.empty()) {
        return {"task_id 不能是空字符串", true};
    }

    // 第一本账:后台命令登记簿(老语义一字不动——认得就发停止,已终态不重复杀)。
    if (BackgroundTaskRegistry::Instance().Stop(task_id)) {
        return {"已对后台任务 #" + task_id + " 发出停止信号。", false};
    }

    // 第二本账:面板后台子代理(Bug B 收口)——命令登记簿查无此号,拿面板
    // 同款编号问会话台账。活态发停止信号(向下级联整棵子树);终态如实说
    // 停无可停,不冒充成功也不报错。两本账都没有才"找不到"。
    const auto panel_id = ParsePanelTaskId(task_id);
    if (panel_id.has_value() && agent_ledger_provider_) {
        TaskLedger* ledger = agent_ledger_provider_();
        if (ledger != nullptr) {
            for (const auto& s : ledger->Summaries()) {
                if (s.id != *panel_id) {
                    continue;
                }
                if (!IsAliveTaskState(s.state)) {
                    return {"后台子代理 #" + std::to_string(s.id) + " 已收场(" +
                            StateShortLabel(s.state) + "),无需停止。", false};
                }
                const bool accepted = ledger->CancelTask(s.id);
                if (!accepted) {
                    // 状态机竞态:刚翻终态。再读一次如实报,不硬盖章。
                    for (const auto& again : ledger->Summaries()) {
                        if (again.id == s.id && !IsAliveTaskState(again.state)) {
                            return {"后台子代理 #" + std::to_string(s.id) + " 已收场(" +
                                    StateShortLabel(again.state) + "),无需停止。", false};
                        }
                    }
                }
                std::ostringstream oss;
                oss << "已对后台子代理 #" << s.id;
                if (!s.title.empty()) {
                    oss << " (" << s.agent_type << " · " << s.title << ")";
                }
                oss << " 发出停止信号(整棵子树级联收口,面板行随后显\"停止中\")。";
                return {oss.str(), false};
            }
        }
    }
    return {"找不到 task_id=" + task_id + " 的后台任务(后台命令与后台子代理两本账都没有这个编号)。", true};
}

}  // namespace lubancode::tools
