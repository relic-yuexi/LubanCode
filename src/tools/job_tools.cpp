// job_tools.hpp 的实现。结果一律 JSON 文本(单 §8 的接口形状),错误
// 走 Result::is_error + 稳定码;不在这里解释业务结果,只透传协调器投影。
#include "tools/job_tools.hpp"

#include <utility>

#include "tools/registry.hpp"

namespace lubancode::tools {
namespace {

nlohmann::json StatusToJson(const JobStatusView& view) {
    nlohmann::json status = nlohmann::json::object({{"jobId", view.job_id},
                                                    {"status", view.state}});
    if (view.access_denied) {
        status["accessDenied"] = true;
        status["reason"] = view.access_reason;
        return status;
    }
    if (view.cancel_requested) {
        status["cancelRequested"] = true;
    }
    if (!view.result_ref.empty()) {
        status["resultRef"] = view.result_ref;
        status["resultVersion"] = view.result_version;
    }
    if (!view.preview.empty()) {
        status["preview"] = view.preview;
        if (view.preview_truncated) {
            status["previewTruncated"] = true;
        }
    }
    if (!view.failure.empty()) {
        status["failure"] = view.failure;
    }
    return status;
}

class JobGetTool final : public Tool {
public:
    explicit JobGetTool(std::shared_ptr<ToolJobCoordinator> coordinator)
        : coordinator_(std::move(coordinator)) {}

    std::string name() const override { return "job_get"; }
    std::string description() const override {
        return "查询后台任务当前状态。终态带 resultRef 与有界预览;只读,不自动重跑。";
    }
    nlohmann::json input_schema() const override {
        return nlohmann::json::object({
            {"type", "object"},
            {"properties",
             nlohmann::json::object({
                 {"jobId", nlohmann::json::object({{"type", "string"},
                                                   {"description", "start 接单回执里的 jobId"}}),
             })},
            {"required", nlohmann::json::array({"jobId"})},
        });
    }
    Tool::Result execute(const nlohmann::json& input) override {
        if (!input.is_object() || !input.contains("jobId") || !input["jobId"].is_string()) {
            return Tool::Result{"job_get 入参缺 jobId 字符串", true};
        }
        const JobStatusView view = coordinator_->GetJob(input["jobId"].get<std::string>());
        Tool::Result result;
        result.SetText(StatusToJson(view).dump());
        result.is_error = view.state == "unknown_job";
        if (result.is_error) {
            result.error_code = "job.unknown";
        }
        return result;
    }

private:
    std::shared_ptr<ToolJobCoordinator> coordinator_;
};

class JobWaitTool final : public Tool {
public:
    explicit JobWaitTool(std::shared_ptr<ToolJobCoordinator> coordinator)
        : coordinator_(std::move(coordinator)) {}

    std::string name() const override { return "job_wait"; }
    std::string description() const override {
        return "有界等待后台任务:mode=any 任一终态即回,all 等全部。超时回 timedOut 与各任务"
               "状态快照,不宣告失败;已完成任务的业务结果排在状态之前。";
    }
    nlohmann::json input_schema() const override {
        return nlohmann::json::object({
            {"type", "object"},
            {"properties",
             nlohmann::json::object({
                 {"jobIds", nlohmann::json::object({{"type", "array"},
                                                    {"items", nlohmann::json::object({{"type", "string"}})}})},
                 {"timeout_ms",
                  nlohmann::json::object({{"type", "integer"}, {"minimum", 0},
                                          {"description", "有界等待毫秒;0 只取快照"}})},
                 {"mode", nlohmann::json::object({{"type", "string"}, {"enum", nlohmann::json::array({"any", "all"})}})},
             })},
            {"required", nlohmann::json::array({"jobIds"})},
        });
    }
    Tool::Result execute(const nlohmann::json& input) override {
        if (!input.is_object() || !input.contains("jobIds") || !input["jobIds"].is_array()) {
            return Tool::Result{"job_wait 入参缺 jobIds 数组", true};
        }
        std::vector<std::string> job_ids;
        for (const auto& id : input["jobIds"]) {
            if (id.is_string() && !id.get<std::string>().empty()) {
                job_ids.push_back(id.get<std::string>());
            }
        }
        std::uint64_t timeout_ms = 0;
        if (input.contains("timeout_ms") && input["timeout_ms"].is_number_unsigned()) {
            timeout_ms = input["timeout_ms"].get<std::uint64_t>();
        }
        bool wait_all = false;
        if (input.contains("mode") && input["mode"].is_string()) {
            wait_all = input["mode"].get<std::string>() == "all";
        }
        const JobWaitResult waited = coordinator_->WaitJobs(job_ids, timeout_ms, wait_all);
        // 单 §7:已完成的业务结果排在 wait 自身状态结果前——results 数组
        // 先落,statuses 游标压后。
        nlohmann::json results = nlohmann::json::array();
        nlohmann::json statuses = nlohmann::json::array();
        for (const JobStatusView& view : waited.statuses) {
            statuses.push_back(StatusToJson(view));
            if (!view.access_denied && view.state == "succeeded" && !view.preview.empty()) {
                results.push_back(nlohmann::json::object({{"jobId", view.job_id},
                                                          {"resultRef", view.result_ref},
                                                          {"resultVersion", view.result_version},
                                                          {"preview", view.preview}}));
            }
        }
        Tool::Result result;
        result.SetText(nlohmann::json::object({{"results", std::move(results)},
                                               {"statuses", std::move(statuses)},
                                               {"satisfied", waited.satisfied},
                                               {"timedOut", waited.timed_out}})
                           .dump());
        return result;
    }

private:
    std::shared_ptr<ToolJobCoordinator> coordinator_;
};

class JobCancelTool final : public Tool {
public:
    explicit JobCancelTool(std::shared_ptr<ToolJobCoordinator> coordinator)
        : coordinator_(std::move(coordinator)) {}

    std::string name() const override { return "job_cancel"; }
    std::string description() const override {
        return "请求取消后台任务。回取消请求状态(cancel_requested|already_terminal),不保证已终止。";
    }
    nlohmann::json input_schema() const override {
        return nlohmann::json::object({
            {"type", "object"},
            {"properties",
             nlohmann::json::object({
                 {"jobId", nlohmann::json::object({{"type", "string"}})},
                 {"reason", nlohmann::json::object({{"type", "string"}})},
             })},
            {"required", nlohmann::json::array({"jobId"})},
        });
    }
    Tool::Result execute(const nlohmann::json& input) override {
        if (!input.is_object() || !input.contains("jobId") || !input["jobId"].is_string()) {
            return Tool::Result{"job_cancel 入参缺 jobId 字符串", true};
        }
        std::string reason;
        if (input.contains("reason") && input["reason"].is_string()) {
            reason = input["reason"].get<std::string>();
        }
        const JobCancelResult cancelled =
            coordinator_->CancelJob(input["jobId"].get<std::string>(), reason);
        Tool::Result result;
        result.SetText(nlohmann::json::object({{"jobId", input["jobId"]},
                                               {"status", cancelled.status},
                                               {"terminal", cancelled.terminal}})
                           .dump());
        result.is_error = !cancelled.ok;
        if (result.is_error) {
            result.error_code = "job.cancel." + cancelled.status;
        }
        return result;
    }

private:
    std::shared_ptr<ToolJobCoordinator> coordinator_;
};

}  // namespace

std::unique_ptr<Tool> MakeJobGetTool(std::shared_ptr<ToolJobCoordinator> coordinator) {
    return std::make_unique<JobGetTool>(std::move(coordinator));
}

std::unique_ptr<Tool> MakeJobWaitTool(std::shared_ptr<ToolJobCoordinator> coordinator) {
    return std::make_unique<JobWaitTool>(std::move(coordinator));
}

std::unique_ptr<Tool> MakeJobCancelTool(std::shared_ptr<ToolJobCoordinator> coordinator) {
    return std::make_unique<JobCancelTool>(std::move(coordinator));
}

void RegisterJobTools(ToolRegistry& registry, std::shared_ptr<ToolJobCoordinator> coordinator) {
    ToolRegistration get;
    get.tool = MakeJobGetTool(coordinator);
    get.effect_class = EffectClass::ReadOnlyLocal;
    registry.Register(std::move(get));
    ToolRegistration wait;
    wait.tool = MakeJobWaitTool(std::move(coordinator));
    wait.effect_class = EffectClass::ReadOnlyLocal;
    registry.Register(std::move(wait));
    ToolRegistration cancel;
    cancel.tool = MakeJobCancelTool(std::move(coordinator));
    cancel.effect_class = EffectClass::ReadOnlyLocal;
    registry.Register(std::move(cancel));
}

}  // namespace lubancode::tools
