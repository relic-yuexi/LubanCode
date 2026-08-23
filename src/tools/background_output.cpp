#include "tools/background_output.hpp"

#include <cstdint>
#include <sstream>
#include <string>

#include "tools/background_tasks.hpp"
#include "tools/tool_text.hpp"  // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/

namespace lubancode::tools {

namespace {

const char* StatusLabel(BackgroundTaskStatus s) {
    switch (s) {
        case BackgroundTaskStatus::Running: return "运行中";
        case BackgroundTaskStatus::Completed: return "完成(退出码 0)";
        case BackgroundTaskStatus::Failed: return "失败(非零退出码)";
        case BackgroundTaskStatus::Stopped: return "已停止";
    }
    return "未知";
}

// 把一条任务的摘要拼成一行文本(List/detail 共用)。
void AppendTaskSummary(std::ostringstream& oss, const BackgroundTaskInfo& t) {
    oss << "[#" << t.task_id << "] " << StatusLabel(t.status);
    if (t.status != BackgroundTaskStatus::Running) {
        oss << " (exit " << t.exit_code << ")";
    }
    oss << "  PID=" << t.pid << "\n  命令: " << t.command << "\n  日志: " << t.log_path << "\n";
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
                    "命令、PID、日志文件路径。给 task_id 就返回该任务的详情 + 日志文件尾部 tail_lines 行"
                    "(默认 50)。任务还在跑也能读,文件允许边写边读。"
                    "起完一个后台命令后,用它查进度/结果,不用自己再拼 tail 命令。");
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

    // 不给 task_id:列全部。
    if (task_id.empty()) {
        const auto tasks = registry.List();
        if (tasks.empty()) {
            return {"当前没有后台任务。", false};
        }
        std::ostringstream oss;
        oss << "后台任务共 " << tasks.size() << " 个:\n\n";
        for (const auto& t : tasks) {
            AppendTaskSummary(oss, t);
            oss << "\n";
        }
        return {oss.str(), false};
    }

    // 给 task_id:查单个 + 读输出。
    const auto info = registry.Get(task_id);
    if (!info.has_value()) {
        return {"找不到 task_id=" + task_id + " 的后台任务。", true};
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
                    "停掉一个后台命令(run_command run_in_background:true 起的)。Windows 上 "
                    "TerminateProcess 根进程,POSIX 上 kill 杀整个进程组。已完成的任务不会重复杀。"
                    "长命进程(dev server、watch、build)跑够了、或者起错了想收掉,用它。");
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

    const bool found = BackgroundTaskRegistry::Instance().Stop(task_id);
    if (!found) {
        return {"找不到 task_id=" + task_id + " 的后台任务。", true};
    }
    return {"已对后台任务 #" + task_id + " 发出停止信号。", false};
}

}  // namespace lubancode::tools
