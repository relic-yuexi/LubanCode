// CommandService 的实现(显示系统剥离单第七步)。
//
// 三条 typed API 的执行体,业务规则原文自 app/commands 的 HandleModelCommand
// 与 ResumeSession 搬来(清单拼装、条目应用、序号解析、回放接管),剥掉
// 打印与交互问话——那半留在终端适配层。依赖铁律:不 include cli/app,
// 不碰标准流。

#include "runtime/command_service.hpp"

#include <algorithm>
#include <utility>

namespace lubancode::runtime {

CommandService::CommandService(Options options) : options_(std::move(options)) {}

CommandService::~CommandService() = default;

// ---- SetModel ---------------------------------------------------------------

ModelQueryResult CommandService::QueryModels() const {
    ModelQueryResult out;
    out.current_model = options_.current_model ? *options_.current_model : std::string();
    out.config_file_path = options_.config_file_path.value_or(std::string());
    if (options_.fetch_models) {
        auto fetched = options_.fetch_models();
        if (fetched.has_value()) {
            for (auto& [id, display] : *fetched) {
                ModelListEntry entry;
                entry.id = id;
                entry.display_name = display;
                entry.current = id == out.current_model;
                out.models.push_back(std::move(entry));
            }
        } else {
            out.fetch_failed = true;
            out.fetch_error = fetched.error();
        }
    } else {
        // 没注取数口:至少报当前项(裸敲也能看"现在是谁")。
        if (!out.current_model.empty()) {
            ModelListEntry entry;
            entry.id = out.current_model;
            entry.display_name = out.current_model;
            entry.current = true;
            out.models.push_back(std::move(entry));
        }
    }
    return out;
}

SetModelResult CommandService::SetModel(const std::string& model_id, bool write_config) {
    SetModelResult out;
    if (model_id.empty()) {
        out.error = "empty_model";
        return out;
    }
    if (options_.current_model == nullptr || options_.config == nullptr) {
        out.error = "not_configured";
        return out;
    }
    out.model = model_id;
    *options_.current_model = model_id;
    options_.config->model = model_id;
    out.switched = true;

    // 模型目录应用(主动切换:目录声明了就用,用户显式配过的不动——两个
    // explicit 都 false,与终端 HandleModelCommand 同一规矩)。
    if (options_.model_catalog != nullptr) {
        const config::CatalogApplication application =
            config::ComputeCatalogApplication(*options_.model_catalog, model_id,
                                              /*think_explicitly_configured=*/false,
                                              /*window_explicitly_configured=*/false);
        if (application.think.has_value() && options_.current_think != nullptr) {
            *options_.current_think = *application.think;
        }
        if (application.context_window_tokens.has_value()) {
            options_.context_window_tokens = *application.context_window_tokens;
        }
        if (options_.current_think != nullptr) {
            out.think = *options_.current_think;
        }
    }

    // 写回配置文件是显式一笔(终端问过才传 true;GUI 分立按钮)。
    if (write_config && options_.config_file_path.has_value()) {
        const auto updated = config::UpdateModelInConfigFile(*options_.config_file_path, model_id);
        if (updated.has_value()) {
            out.config_written = true;
        } else {
            out.error = "config_write_failed: " + updated.error();
        }
    }
    return out;
}

// ---- ResumeThread -------------------------------------------------------------

std::vector<ThreadListEntry> CommandService::ListThreads(std::size_t limit) const {
    std::vector<ThreadListEntry> out;
    if (options_.sessions_dir.empty()) {
        return out;  // 没主目录:空清单,不冒充
    }
    // cwd 过滤不做(远端前端看得见全部场子,自己按 cwd 列组);resume 的
    // 序号以这份清单为准。
    const std::vector<agent::SessionListEntry> entries = agent::ListSessions(options_.sessions_dir, limit);
    out.reserve(entries.size());
    std::size_t index = 1;
    for (const auto& entry : entries) {
        ThreadListEntry item;
        item.id = entry.id;
        item.started_at = entry.started_at;
        item.cwd = entry.cwd;
        item.title = entry.title;
        item.first_user_text = entry.first_user_text;
        item.message_count = entry.message_count;
        item.index = index++;
        out.push_back(std::move(item));
    }
    return out;
}

ResumeResult CommandService::ResumeThread(agent::AgentLoop& loop, SessionRuntime& runtime,
                                          const std::string& thread_ref, const std::string& cwd) {
    ResumeResult out;
    if (runtime.sessions_dir().empty()) {
        out.error = "no_home";
        return out;
    }
    // 目标解析:空串 = 最近一场;纯数字 = 列表序号(倒序,1 起);其余按
    // id 找,不在前列就拼路径兜底(与终端 ResumeSession 同规矩,只是这里
    // 不限"本目录"——远端前端看得见全部场子)。
    const std::vector<agent::SessionListEntry> entries =
        agent::ListSessions(runtime.sessions_dir(), 200, /*cwd_filter=*/std::string());
    std::string id;
    std::string file_path;
    bool all_digits = !thread_ref.empty();
    for (const char c : thread_ref) {
        if (c < '0' || c > '9') {
            all_digits = false;
            break;
        }
    }
    if (thread_ref.empty()) {
        if (entries.empty()) {
            out.error = "none";
            return out;
        }
        id = entries.front().id;
        file_path = entries.front().file_path;
    } else if (all_digits) {
        std::size_t n = 0;
        try {
            n = static_cast<std::size_t>(std::stoul(thread_ref));
        } catch (...) {
            n = 0;
        }
        if (n < 1 || n > entries.size()) {
            out.error = "out_of_range";
            return out;
        }
        id = entries[n - 1].id;
        file_path = entries[n - 1].file_path;
    } else {
        for (const auto& entry : entries) {
            if (entry.id == thread_ref) {
                id = entry.id;
                file_path = entry.file_path;
                break;
            }
        }
        if (id.empty()) {
            id = thread_ref;
            file_path = runtime.sessions_dir() + "/" + thread_ref + ".jsonl";
        }
    }

    const auto content = agent::ReadSessionFileBytes(file_path);
    if (!content.has_value()) {
        out.error = "read_failed";
        return out;
    }
    auto session = agent::ParseSessionFile(*content);
    if (!session.has_value()) {
        out.error = "bad_meta";
        return out;
    }

    loop.ReplaceHistory(session->messages);
    runtime.persisted_count() = session->messages.size();
    (void)runtime.store().ResumeAt(file_path, id);
    runtime.meta() = session->meta;
    runtime.title() = session->title;
    runtime.compact_epoch() = session->compact_epoch;
    (void)cwd;

    out.resumed = true;
    out.id = id;
    out.restored_messages = session->messages.size();
    out.total_lines = session->all_messages.size();
    out.compact_epoch = session->compact_epoch;
    out.title = session->title;
    return out;
}

// ---- 审批/提问回答 -------------------------------------------------------------

CommandService::InteractionAnswerResult CommandService::ResolveApproval(
    InteractionBroker* broker, const std::string& request_id, const ApprovalResponse& response) const {
    InteractionAnswerResult out;
    if (broker == nullptr) {
        out.error_code = "no_broker";
        out.error_message = "这台前端没有悬起审批的实现(终端当场问完)";
        return out;
    }
    InteractionRequestId parsed;
    parsed.value = request_id;
    if (!broker->ResolveApproval(parsed, response)) {
        out.error_code = kStaleRequestId;
        out.error_message = "请求已失效(答完/收口/不认识)";
    } else {
        out.ok = true;
    }
    return out;
}

CommandService::InteractionAnswerResult CommandService::AnswerQuestion(
    InteractionBroker* broker, const std::string& request_id, const QuestionResponse& response) const {
    InteractionAnswerResult out;
    if (broker == nullptr) {
        out.error_code = "no_broker";
        out.error_message = "这台前端没有悬起提问的实现(终端当场问完)";
        return out;
    }
    InteractionRequestId parsed;
    parsed.value = request_id;
    if (!broker->AnswerQuestion(parsed, response)) {
        out.error_code = kStaleRequestId;
        out.error_message = "请求已失效(答完/收口/不认识)";
    } else {
        out.ok = true;
    }
    return out;
}

}  // namespace lubancode::runtime
