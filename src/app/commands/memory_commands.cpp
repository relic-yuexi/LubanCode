// /memory 命令 presenter 实现(合同见 memory_commands.hpp)。函数体自
// interactive_session 的 HandleMemoryCommand 原文搬家(改道:project_memory
// 与 theme 走 ctx、EnsureMemoryTool 走 ensure_tool 回调、输出走 TerminalPort),
// 行为一字不差——注释一并随行。

#include "app/commands/memory_commands.hpp"
#include "app/commands/command_registry.hpp"  // SlashDispatchContext(分派注册制)

#include <algorithm>
#include <cctype>
#include <chrono>
#include <memory>
#include <sstream>
#include <utility>

#include "accounting/purpose.hpp"  // RequestPurpose(Token 账本单 A1)
#include "app/memory_extract.hpp"  // ClassifyTaskType/BuildTurnTranscript 一族
#include "app/model_router.hpp"
#include "cli/console_input.hpp"
#include "cli/i18n.hpp"
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "memory/project_memory.hpp"
#include "runtime/trajectory_session.hpp"  // TrajectorySessionLedger(旁路桥)
#include "tools/path_utils.hpp"

namespace lubancode::app {

using lubancode::cli::TermOut;
using lubancode::cli::tr;
using lubancode::cli::trf;

namespace {

std::string TrimAscii(std::string value) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// 层级词解析(P0-4 §8.3):project|global 是正名,user 是过渡期别名。
// 返回 <层, 剩余参数流的下一词已否被吃掉>;层为空 = 没写层级(旧写法)。
std::string ParseLayerWord(std::istringstream& words) {
    std::string word;
    const std::streampos back = words.tellg();
    words >> word;
    word = LowerAscii(std::move(word));
    if (word == "project") return "project";
    if (word == "global" || word == "user") return "user";
    words.seekg(back);  // 不是层级词,还回去
    return std::string();
}

// 逐次确认合同(P0-4 §6.1):全局层的每次写入/删除都要用户点头。
bool ConfirmGlobalAction(const lubancode::cli::Theme& theme, const std::string& question) {
    const auto answer = lubancode::cli::ReadLine(theme.confirm + question + theme.reset, theme,
                                                 /*esc_rejects=*/true);
    return answer.has_value() && (*answer == "y" || *answer == "Y");
}

void PrintMemoryUsage() {
    TermOut() << tr("cmd.memory.usage");
}

// 入队结果的统一呈现(修复单 §五 B):排队成功只报纯 job_id(不混启动
// 说明);启动失败另起一行短提示附诊断入口,不吞队列成功值。
void PrintEnqueueResult(const lubancode::memory::MemoryEnqueueResult& result) {
    TermOut() << trf("cmd.memory.queued", result.job_id) << "\n";
    if (result.worker_state == lubancode::memory::MemoryWorkerLaunchState::StartFailed) {
        TermOut() << trf("cmd.memory.worker_failed",
                         result.worker_error.empty() ? result.worker_error_code : result.worker_error)
                  << "\n";
    }
}

// (抽取的本地超时预算 kMemoryExtractTimeoutSecs:原先住这,回合总结
// 异步化单起公开进 app/memory_extract.hpp——后台执行器的看门狗与收账
// 点的终端提示同用一把尺。)

}  // namespace

void HandleMemoryCommand(const MemoryCommandContext& ctx, const std::string& raw_args) {
    lubancode::memory::ProjectMemory* project_memory = ctx.project_memory;
    const lubancode::cli::Theme& theme = *ctx.theme;
    const auto ensure_tool = [&ctx]() {
        if (ctx.ensure_tool) {
            ctx.ensure_tool();
        }
    };
    if (project_memory == nullptr) {
        TermOut() << tr("cmd.memory.unavailable") << "\n";
        return;
    }

    std::istringstream words(raw_args);
    std::string action;
    words >> action;
    std::transform(action.begin(), action.end(), action.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (action.empty() || action == "status") {
        const auto status = project_memory->Status();
        const auto toggle_word = [](bool enabled) { return enabled ? tr("cmd.memory.on") : tr("cmd.memory.off"); };
        TermOut() << trf("cmd.memory.global", toggle_word(status.global_allowed)) << "\n"
                  << trf("cmd.memory.status", toggle_word(status.enabled), toggle_word(status.use),
                         toggle_word(status.generate))
                  << "\n"
                  << trf("cmd.memory.learn_status", status.learn) << "\n"
                  << trf("cmd.memory.project", status.workspace_key) << "\n"
                  << trf("cmd.memory.directory", lubancode::tools::PathToUtf8(status.memory_dir)) << "\n"
                  << trf("cmd.memory.counts", status.entry_count, status.pending_jobs, status.failed_jobs) << "\n";
        if (status.user_enabled) {
            TermOut() << trf("cmd.memory.user_status", status.user_entry_count,
                             lubancode::tools::PathToUtf8(status.user_memory_dir))
                      << "\n";
        }
        TermOut() << trf("cmd.memory.candidates", status.pending_candidates) << "\n";
        return;
    }
    if (action == "jobs") {
        // 任务台账(修复单 §五 C):当前工作区的待写/失败任务,按工作区
        // 隔离;retry 唤醒 pending,重试 failed 须显式点名。
        std::string sub;
        words >> sub;
        std::transform(sub.begin(), sub.end(), sub.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (sub == "retry") {
            std::string target;
            words >> target;
            if (!target.empty()) {
                const auto retried = project_memory->RetryFailedJob(target);
                TermOut() << (retried.has_value()
                                  ? trf("cmd.memory.jobs.retry_queued", *retried)
                                  : trf("cmd.memory.jobs.retry_rejected", retried.error()))
                          << "\n";
                return;
            }
            const auto woken = project_memory->WakePendingWorker();
            switch (woken.state) {
                case lubancode::memory::MemoryWorkerLaunchState::Started:
                    TermOut() << tr("cmd.memory.jobs.retry_started") << "\n";
                    break;
                case lubancode::memory::MemoryWorkerLaunchState::StartFailed:
                    TermOut() << trf("cmd.memory.worker_failed", woken.error) << "\n";
                    break;
                case lubancode::memory::MemoryWorkerLaunchState::Unavailable:
                    TermOut() << tr("cmd.memory.jobs.retry_unavailable") << "\n";
                    break;
                case lubancode::memory::MemoryWorkerLaunchState::Idle:
                    TermOut() << tr("cmd.memory.jobs.retry_idle") << "\n";
                    break;
                case lubancode::memory::MemoryWorkerLaunchState::AlreadyRunning:
                    TermOut() << tr("cmd.memory.jobs.retry_running") << "\n";
                    break;
            }
            return;
        }
        if (!sub.empty()) {
            PrintMemoryUsage();
            return;
        }
        const auto jobs = project_memory->ListWorkspaceJobs();
        if (jobs.empty()) {
            TermOut() << tr("cmd.memory.jobs.empty") << "\n";
            return;
        }
        TermOut() << tr("cmd.memory.jobs.header") << "\n";
        for (const auto& job : jobs) {
            TermOut() << trf("cmd.memory.jobs.line", job.job_id, job.state, job.operation, job.layer,
                             job.wait_hint, job.worker_state)
                      << "\n";
            if (!job.title.empty()) {
                TermOut() << trf("cmd.memory.jobs.title_line", job.title) << "\n";
            }
            if (!job.error.empty()) {
                TermOut() << trf("cmd.memory.jobs.error_line", job.error) << "\n";
            }
        }
        if (!jobs.empty() && !jobs.front().worker_log.empty()) {
            TermOut() << trf("cmd.memory.jobs.log_line", jobs.front().worker_log) << "\n";
        }
        TermOut() << tr("cmd.memory.jobs.hint") << "\n";
        return;
    }
    if (action == "on" || action == "off") {
        // 授权闸:全局未授权时 /memory on 只会得到"去哪改全局配置"的指引,
        // 不能凭本场命令翻开能力(规格"授权与本场状态分开")。
        const auto toggled = project_memory->set_enabled(action == "on");
        if (!toggled.has_value()) {
            TermOut() << tr("cmd.memory.denied") << "\n";
            return;
        }
        if (action == "on") ensure_tool();
        TermOut() << trf("cmd.memory.master", action == "on" ? tr("cmd.memory.on") : tr("cmd.memory.off"))
                  << "\n";
        return;
    }
    if (action == "use") {
        std::string value;
        words >> value;
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (value != "on" && value != "off") {
            PrintMemoryUsage();
            return;
        }
        const bool enabled = value == "on";
        if (enabled && !project_memory->global_allowed()) {
            TermOut() << tr("cmd.memory.denied") << "\n";
            return;
        }
        project_memory->set_use(enabled);
        TermOut() << trf("cmd.memory.toggle", tr("cmd.memory.retrieval"),
                         enabled ? tr("cmd.memory.on") : tr("cmd.memory.off"))
                  << "\n";
        return;
    }
    if (action == "learn") {
        std::string value;
        words >> value;
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        // 兼容老写法:learn on = review,learn off = off。
        if (value == "on") value = "review";
        auto mode = lubancode::memory::ParseLearnMode(value);
        if (!mode.has_value()) {
            PrintMemoryUsage();
            return;
        }
        const auto switched = project_memory->set_learn(*mode);
        if (!switched.has_value()) {
            // 全局未授权(auto 上限之外的降档仍允许),给出指引。
            if (!project_memory->global_allowed()) {
                TermOut() << tr("cmd.memory.denied") << "\n";
            } else {
                TermOut() << trf("cmd.memory.learn_denied", switched.error()) << "\n";
            }
            return;
        }
        ensure_tool();
        TermOut() << trf("cmd.memory.learn_set", lubancode::memory::LearnModeName(*mode)) << "\n";
        return;
    }
    if (action == "review") {
        const auto candidates = project_memory->ListCandidates();
        if (candidates.empty()) {
            TermOut() << tr("cmd.memory.review.empty") << "\n";
            return;
        }
        TermOut() << tr("cmd.memory.review.header") << "\n";
        for (const auto& candidate : candidates) {
            TermOut() << "- " << candidate.id << " [" << lubancode::memory::MemoryKindName(candidate.kind)
                      << "/" << candidate.confidence << "] " << candidate.title;
            if (!candidate.summary.empty() && candidate.summary != candidate.title) {
                TermOut() << " - " << candidate.summary;
            }
            TermOut() << "\n";
        }
        TermOut() << tr("cmd.memory.review.hint") << "\n";
        return;
    }
    if (action == "accept" || action == "reject") {
        std::string id;
        words >> id;
        if (id.empty()) {
            PrintMemoryUsage();
            return;
        }
        std::string reason;
        std::getline(words, reason);
        reason = TrimAscii(std::move(reason));
        if (action == "accept") {
            const auto queued = project_memory->AcceptCandidate(id);
            if (queued.has_value()) {
                PrintEnqueueResult(*queued);
            } else {
                TermOut() << trf("cmd.memory.queue_failed", queued.error()) << "\n";
            }
        } else {
            const auto rejected = project_memory->RejectCandidate(id, std::move(reason));
            TermOut() << (rejected.has_value() ? tr("cmd.memory.reject.done")
                                               : trf("cmd.memory.queue_failed", rejected.error()))
                      << "\n";
        }
        return;
    }
    if (action == "edit") {
        std::string id;
        words >> id;
        std::string remainder;
        std::getline(words, remainder);
        remainder = TrimAscii(std::move(remainder));
        if (id.empty() || remainder.empty()) {
            PrintMemoryUsage();
            return;
        }
        const std::size_t separator = remainder.find("::");
        std::string title = TrimAscii(remainder.substr(0, separator));
        std::string content = separator == std::string::npos
                                  ? std::string()
                                  : TrimAscii(remainder.substr(separator + 2));
        const auto edited = project_memory->EditCandidate(id, title, content);
        TermOut() << (edited.has_value() ? tr("cmd.memory.edit.done")
                                         : trf("cmd.memory.queue_failed", edited.error()))
                  << "\n";
        return;
    }
    if (action == "why") {
        std::string id;
        words >> id;
        const auto trace = project_memory->LastTrace();
        if (!trace.valid) {
            TermOut() << tr("cmd.memory.why.none") << "\n";
            return;
        }
        TermOut() << trf("cmd.memory.why.header", trace.at) << "\n";
        TermOut() << trf("cmd.memory.why.origin", trace.query_origin) << "\n";
        if (trace.skipped) {
            TermOut() << tr("cmd.memory.why.skipped_turn") << "\n";
            return;
        }
        // 检索词带词路与权重:word=整词/词典实体,gram=中文二元,虚词碎片
        // 拿低权重——用户要看得出为何命中,不只见一把碎字。
        std::ostringstream joined_terms;
        for (std::size_t i = 0; i < trace.terms.size(); ++i) {
            if (i != 0) joined_terms << " ";
            joined_terms << trace.terms[i].text << "[" << trace.terms[i].kind << "/"
                         << trace.terms[i].source << " ×" << trace.terms[i].weight << "]";
        }
        TermOut() << trf("cmd.memory.why.terms", joined_terms.str()) << "\n";
        bool matched_id = id.empty();
        for (const auto& entry : trace.entries) {
            if (!id.empty() && entry.id != id) continue;
            matched_id = true;
            // 命中来自哪一层:用户层带标注,项目层不打扰(规格"/memory why
            // 须写清命中来自 user 还是某个 project key")。
            const std::string shown_id =
                entry.layer == "user" ? entry.id + tr("cmd.memory.why.layer_user") : entry.id;
            if (entry.injected) {
                if (entry.weak) {
                    TermOut() << trf("cmd.memory.why.weak_hit", shown_id, entry.score,
                                     entry.hard_hits, entry.term_hits, entry.bytes, entry.cooccur)
                              << "\n";
                } else {
                    TermOut() << trf("cmd.memory.why.hit", shown_id, entry.score, entry.hard_hits,
                                     entry.term_hits, entry.bytes)
                              << "\n";
                }
                continue;
            }
            std::string reason;
            if (entry.expired) reason = tr("cmd.memory.why.expired");
            else if (entry.scope_blocked) reason = tr("cmd.memory.why.scope");
            else if (entry.stale_blocked) reason = tr("cmd.memory.why.stale");
            else if (entry.snapshot_failed) reason = tr("cmd.memory.why.snapshot_failed");
            else if (entry.layer_superseded) reason = tr("cmd.memory.why.superseded");
            else if (entry.duplicate_dropped) reason = tr("cmd.memory.why.duplicate");
            else if (entry.weak_dropped) reason = tr("cmd.memory.why.weak_dropped");
            else if (entry.below_threshold) reason = tr("cmd.memory.why.below_threshold");
            else if (entry.budget_dropped) reason = tr("cmd.memory.why.budget");
            else reason = tr("cmd.memory.why.skipped");
            TermOut() << trf("cmd.memory.why.miss", shown_id, entry.score, entry.hard_hits,
                             entry.term_hits, reason)
                      << "\n";
        }
        if (!matched_id) {
            TermOut() << trf("cmd.memory.why.missing", id) << "\n";
        }
        TermOut() << trf("cmd.memory.why.total", trace.injected_count, trace.injected_bytes) << "\n";
        return;
    }
    if (action == "list") {
        const std::string layer = ParseLayerWord(words);
        std::string error;
        // 两层合并列:项目层在前,全局层带标注。显式 global 只列全局层
        //(管理读口,不看召回授权)。
        const auto entries = layer == "user" ? std::vector<lubancode::memory::MemoryEntry>{}
                                             : project_memory->ListEntries(&error);
        if (!error.empty()) TermOut() << trf("cmd.memory.catalog_warning", error) << "\n";
        const auto user_entries =
            layer == "user" ? project_memory->ListGlobalEntriesForManagement(&error)
                            : project_memory->ListUserEntries(&error);
        if (!error.empty()) TermOut() << trf("cmd.memory.catalog_warning", error) << "\n";
        if (entries.empty() && user_entries.empty()) {
            TermOut() << tr("cmd.memory.empty") << "\n";
            const auto status = project_memory->Status();
            if (status.pending_jobs > 0 || status.failed_jobs > 0) {
                TermOut() << trf("cmd.memory.pending_hint", status.pending_jobs) << "\n";
                if (const auto woken = project_memory->EnsureWorkerRunning();
                    woken.state == lubancode::memory::MemoryWorkerLaunchState::StartFailed) {
                    TermOut() << trf("cmd.memory.worker_failed", woken.error) << "\n";
                }
            }
            return;
        }
        for (const auto& entry : entries) {
            TermOut() << "- " << entry.id << " [" << lubancode::memory::MemoryKindName(entry.kind) << "] "
                      << entry.title;
            if (!entry.summary.empty() && entry.summary != entry.title) {
                TermOut() << " - " << entry.summary;
            }
            TermOut() << "\n";
        }
        for (const auto& entry : user_entries) {
            TermOut() << "- " << entry.id << " [" << lubancode::memory::MemoryKindName(entry.kind) << "] "
                      << entry.title << " (" << tr("cmd.memory.global_layer") << ")";
            if (!entry.summary.empty() && entry.summary != entry.title) {
                TermOut() << " - " << entry.summary;
            }
            TermOut() << "\n";
        }
        return;
    }
    if (action == "remember") {
        // P0-4(§8.3):显式层级——remember project|global <kind> ...;旧的不带
        // 层级写法过渡期默认 project,每场提示一次迁移。
        const std::string layer = ParseLayerWord(words);
        std::string kind_text;
        words >> kind_text;
        auto kind = lubancode::memory::ParseMemoryKind(LowerAscii(kind_text));
        std::string remainder;
        std::getline(words, remainder);
        remainder = TrimAscii(std::move(remainder));
        if (!kind.has_value() || remainder.empty()) {
            PrintMemoryUsage();
            return;
        }
        const bool to_global = layer == "user";
        if (to_global && *kind == lubancode::memory::MemoryKind::Fact) {
            TermOut() << tr("cmd.memory.global.no_fact") << "\n";
            return;
        }
        const std::size_t separator = remainder.find("::");
        lubancode::memory::SaveRequest request;
        request.kind = *kind;
        if (to_global) {
            request.scope.level = "user";
            request.scope.kind = "user";
            request.confidence = "user-stated";
        }
        request.title = TrimAscii(remainder.substr(0, separator));
        request.content = separator == std::string::npos
                              ? request.title
                              : TrimAscii(remainder.substr(separator + 2));
        request.summary = request.content;
        if (request.title.empty() || request.content.empty()) {
            PrintMemoryUsage();
            return;
        }
        if (layer.empty()) {
            static bool hinted = false;  // 每进程一次
            if (!hinted) {
                hinted = true;
                TermOut() << tr("cmd.memory.remember.legacy_hint") << "\n";
            }
        }
        // 全局层逐次确认(§6.1:写入永远须用户主动授权与主动命令)。
        if (to_global &&
            !ConfirmGlobalAction(theme, trf("cmd.memory.global.confirm", request.title))) {
            TermOut() << tr("cmd.memory.global.cancelled") << "\n";
            return;
        }
        const auto queued = project_memory->EnqueueSave(request, /*user_initiated=*/true,
                                                        lubancode::memory::MemoryWriteSource::ExplicitCommandSave);
        if (queued.has_value()) {
            PrintEnqueueResult(*queued);
        } else {
            TermOut() << trf("cmd.memory.queue_failed", queued.error()) << "\n";
        }
        return;
    }
    if (action == "forget") {
        const std::string layer = ParseLayerWord(words);
        std::string id;
        words >> id;
        if (id.empty()) {
            PrintMemoryUsage();
            return;
        }
        // 全局层删除是破坏性动作:逐次确认(§6.4 只认用户级命令)。
        if (layer == "user" &&
            !ConfirmGlobalAction(theme, trf("cmd.memory.global.confirm_forget", id))) {
            TermOut() << tr("cmd.memory.global.cancelled") << "\n";
            return;
        }
        const auto queued = project_memory->EnqueueForget(id, layer);
        if (queued.has_value()) {
            PrintEnqueueResult(*queued);
        } else {
            TermOut() << trf("cmd.memory.queue_failed", queued.error()) << "\n";
        }
        return;
    }
    if (action == "rebuild") {
        const auto queued = project_memory->EnqueueRebuild();
        if (queued.has_value()) {
            PrintEnqueueResult(*queued);
        } else {
            TermOut() << trf("cmd.memory.queue_failed", queued.error()) << "\n";
        }
        return;
    }
    if (action == "stale") {
        const auto stale = project_memory->ListStaleEntries();
        if (stale.empty()) {
            TermOut() << tr("cmd.memory.stale.empty") << "\n";
            return;
        }
        TermOut() << tr("cmd.memory.stale.header") << "\n";
        for (const auto& item : stale) {
            TermOut() << "- " << item.entry.id << " [" << item.reason << "] " << item.entry.title;
            if (item.reason == "fingerprint") {
                TermOut() << " (" << tr("cmd.memory.stale.fingerprint") << ")";
            } else {
                TermOut() << " (" << tr("cmd.memory.stale.expired") << ": " << item.entry.expires_at << ")";
            }
            TermOut() << "\n";
        }
        TermOut() << tr("cmd.memory.stale.hint") << "\n";
        return;
    }
    if (action == "verify" || action == "refresh") {
        const std::string layer = ParseLayerWord(words);
        std::string id;
        words >> id;
        if (id.empty()) {
            PrintMemoryUsage();
            return;
        }
        const auto queued = project_memory->EnqueueVerify(id, action == "refresh", layer);
        if (queued.has_value()) {
            PrintEnqueueResult(*queued);
        } else {
            TermOut() << trf("cmd.memory.queue_failed", queued.error()) << "\n";
        }
        return;
    }
    if (action == "show") {
        const std::string layer = ParseLayerWord(words);
        std::string id;
        words >> id;
        if (id.empty()) {
            PrintMemoryUsage();
            return;
        }
        const auto topic = project_memory->ReadTopicForShow(id);
        if (!topic.has_value()) {
            TermOut() << trf("cmd.memory.queue_failed", topic.error()) << "\n";
            return;
        }
        const auto& [text, dir] = *topic;
        if (layer == "user" && dir != project_memory->user_memory_dir()) {
            TermOut() << trf("cmd.memory.queue_failed", tr("cmd.memory.global.layer_mismatch")) << "\n";
            return;
        }
        TermOut() << trf("cmd.memory.show.header", id, lubancode::tools::PathToUtf8(dir)) << "\n" << text;
        if (!text.empty() && text.back() != '\n') TermOut() << "\n";
        return;
    }
    if (action == "open") {
        std::string id;
        words >> id;
        const auto edited = id.empty() ? project_memory->OpenIndexInEditor()
                                       : project_memory->EditTopicInEditor(id);
        TermOut() << (edited.has_value() ? tr("cmd.memory.open.done")
                                         : trf("cmd.memory.queue_failed", edited.error()))
                  << "\n";
        return;
    }
    if (action == "migrate") {
        // 先列将改/跳过/警告几份,经确认才动盘;原件备进
        // .state/migration-backup/<时间>/,全部写妥、重建成功才报完成。
        const auto plan = project_memory->PlanMigration();
        if (plan.to_migrate == 0) {
            TermOut() << trf("cmd.memory.migrate.none", plan.to_skip, plan.warnings) << "\n";
            return;
        }
        TermOut() << trf("cmd.memory.migrate.plan", plan.to_migrate, plan.to_skip, plan.warnings) << "\n";
        for (const auto& item : plan.items) {
            if (item.action == "migrate") {
                TermOut() << "  - " << item.id << " (" << item.file << "; " << item.reason << ")\n";
            } else if (item.action == "warn") {
                TermOut() << "  [warn] " << item.reason << "\n";
            }
        }
        const auto answer = lubancode::cli::ReadLine(theme.confirm + tr("cmd.memory.migrate.confirm") + theme.reset,
                                                     theme, /*esc_rejects=*/true);
        if (!answer.has_value() || (*answer != "y" && *answer != "Y")) {
            TermOut() << tr("cmd.memory.migrate.cancelled") << "\n";
            return;
        }
        const auto result = project_memory->RunMigration();
        TermOut() << (result.has_value()
                          ? trf("cmd.memory.migrate.done", result->migrated, result->backup_dir)
                          : trf("cmd.memory.queue_failed", result.error()))
                  << "\n";
        return;
    }
    PrintMemoryUsage();
}

// ---- 会话尾款的 memory 接线(终端接线收尾单自大类搬出;原文随行) -------

TurnMemoryDispatch ExtractTurnMemory(const SessionTailContext& ctx, const std::string& user_text,
                                     std::size_t history_before) {
    // 回合总结异步化单:前置门(这一整段)留在前台——全纯本地(词法
    // 判定/历史切片/转写与提示拼装),微秒级;门过即后台起飞,收口不等
    // 网络。起飞后的收账见 SettleTurnMemory(主线程空闲拍)。
    lubancode::memory::ProjectMemory* project_memory = ctx.project_memory;
    const lubancode::cli::Theme& theme = *ctx.theme;
    lubancode::app::ModelRouterService& model_router = *ctx.model_router;
    lubancode::app::MemoryTurnLedger* memory_turns = ctx.memory_turns;
    // 记忆写入调度单 P0:前置门的每个早退都记一笔稳定 reason(§10.1 漏斗
    // 的 skipped_* 分子)。P1 起前置门添了真闸:同轮去重(§7.1 案二)与
    // 必跳层文本门(案三至案六 + §7.3 门槛)——这两处拦下就真不发请求,
    // 不只是记账；耐久信号也会在发请求前拦下无长期价值的回合。
    if (project_memory == nullptr || !project_memory->generate_enabled()) {
        if (memory_turns != nullptr) {
            memory_turns->NoteExtractionSkipped(lubancode::app::ExtractionSkipReason::Disabled);
        }
        return TurnMemoryDispatch::Skipped;
    }

    const auto& history = ctx.agent->History();
    if (history_before >= history.size()) {
        if (memory_turns != nullptr) {
            memory_turns->NoteExtractionSkipped(lubancode::app::ExtractionSkipReason::NoNewHistory);
        }
        return TurnMemoryDispatch::Skipped;
    }
    if (memory_turns != nullptr) memory_turns->NoteHistoryGrew();
    std::vector<api::Message> slice(history.begin() + static_cast<std::ptrdiff_t>(history_before),
                                    history.end());

    // 工具名清单喂给分型器;转写按用户、最终答复、工具摘录分配预算，整段不超 8 KiB。顺手记一枚
    // P1(§7.1)的工具证据位:工具调用或工具结果任一在场即算——ack 门
    // 与耐久信号都看它。
    std::vector<std::string> tool_names;
    std::string assistant_text;
    bool has_tool_evidence = false;
    for (const auto& message : slice) {
        for (const auto& block : message.content) {
            if (message.role == api::Role::Assistant) {
                if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                    assistant_text += text->text + "\n";
                }
            }
            if (const auto* use = std::get_if<api::ToolUseBlock>(&block)) {
                tool_names.push_back(use->name);
                has_tool_evidence = true;
            } else if (std::get_if<api::ToolResultBlock>(&block) != nullptr) {
                has_tool_evidence = true;
            }
        }
    }
    if (memory_turns != nullptr) memory_turns->NoteGateContext(has_tool_evidence);

    // 记忆写入调度单 P1(§7.1 案二·同轮去重):本轮已有成功的 save/forget/
    // accept(§6.2 回执账,P0 起就在记),回合尾立即收手——显式保存一轮
    // 只走一条写路。放在文本门之前:§7.1 的次序里案二压过案三至案六。
    // (存过东西的回合 history 必然增长过,与 no_new_history 无冲突。)
    if (memory_turns != nullptr && memory_turns->turn_mutated()) {
        memory_turns->NoteExtractionSkipped(lubancode::app::ExtractionSkipReason::AlreadyMutated);
        return TurnMemoryDispatch::Skipped;
    }

    // P1(§7.1 案三至案六 + §7.3 门槛):必跳层的文本侧判定。纯词法、
    // 不构造 prompt——"好""继续""/help"一类不再叫模型。
    const MeaningfulTextStats text_stats = ComputeMeaningfulTextStats(user_text);
    const auto durable_signals = EvaluateTurnDurableSignals(user_text, assistant_text, has_tool_evidence);
    if (memory_turns != nullptr) memory_turns->NoteDurableSignals(durable_signals);
    if (const auto blocked = EvaluateMustSkipTextGate(text_stats, has_tool_evidence)) {
        // A short explicit preference or a tool-backed conclusion still matters.
        if (*blocked != ExtractionSkipReason::ShortText || durable_signals.empty()) {
            if (memory_turns != nullptr) memory_turns->NoteExtractionSkipped(*blocked);
            return TurnMemoryDispatch::Skipped;
        }
    }
    if (durable_signals.empty()) {
        if (memory_turns != nullptr) memory_turns->NoteExtractionSkipped(ExtractionSkipReason::NoDurableSignal);
        return TurnMemoryDispatch::Skipped;
    }

    const std::string turn_transcript = BuildTurnTranscript(slice, 8 * 1024);
    if (turn_transcript.empty()) {
        if (memory_turns != nullptr) {
            memory_turns->NoteExtractionSkipped(lubancode::app::ExtractionSkipReason::EmptyTranscript);
        }
        return TurnMemoryDispatch::Skipped;
    }

    const std::string task_type = ClassifyTaskType(user_text, tool_names);
    const std::string system_prompt = BuildExtractionSystemPrompt(*ctx.prompts_dir, task_type);
    if (system_prompt.empty()) {
        if (memory_turns != nullptr) {
            memory_turns->NoteExtractionSkipped(lubancode::app::ExtractionSkipReason::PromptMissing);
        }
        return TurnMemoryDispatch::Skipped;
    }
    // Local durable-signal gating above runs before transcript/prompt construction.
    if (memory_turns != nullptr) memory_turns->NoteExtractionCalled();

    // ---- 回合总结异步化单:门过,起飞(前台不再有网络等待) ----
    // 前台显示:收口不打 [memory] 起跑行——learn 开着的每一场,收口即刻
    // 还输入框;完成/失败的行挪到收账点(SettleTurnMemory)。
    if (ctx.extractor == nullptr || ctx.extractor->Busy()) {
        // 单飞让位:同场在途至多一枚(上一枚在跑或结果待收)。让位的回合
        // 如实记一笔——不冒充网络失败,也不冒充 aborted(那是"收口没走
        // 到"):这是主动让位,下一轮照常抽。执行器没接是装配缺口,另立
        // 稳定码,不和让位混账。
        if (memory_turns != nullptr) {
            lubancode::app::MemoryTurnLedger::ExtractOutcome outcome;
            outcome.ok = false;
            outcome.error_code = ctx.extractor == nullptr ? "no_extractor" : "in_flight_dropped";
            memory_turns->NoteExtractionOutcome(outcome);
        }
        return ctx.extractor == nullptr ? TurnMemoryDispatch::DroppedRoute : TurnMemoryDispatch::DroppedBusy;
    }
    // 独占裸 backend(RouteDetached):不与主会话共用 client,也不借同步
    // 路由的缓存 client 并发——抽取线程自己持有、自己释放。provider 找
    // 不到条目时 backend 为空,不偷偷换回当前端。
    auto detached = model_router.RouteDetached(lubancode::agent::TaskKind::MemoryExtract);
    if (detached.backend == nullptr) {
        // 路由落空:旧口径补零账(calls=1,零 token,"未报告"),不吞。
        model_router.ledger().Record(lubancode::agent::ModelRole::Cheap, detached.route.model,
                                      lubancode::api::Usage{}, /*duration_ms=*/0, /*reported=*/false);
        ExtractionError route_miss;
        route_miss.code = lubancode::app::ExtractionErrorCode::RouteMiss;
        route_miss.message = "cheap 路由找不到 provider \"" + detached.route.provider + "\"";
        if (memory_turns != nullptr) {
            lubancode::app::MemoryTurnLedger::ExtractOutcome outcome;
            outcome.ok = false;
            outcome.error_code = StableExtractErrorCode(route_miss);
            memory_turns->NoteExtractionOutcome(outcome);
        }
        // 本地判定(无网络往返),前台直接亮报一行即止。
        TermOut() << theme.stats << trf("memory.extract.failed", route_miss.message) << theme.reset << "\n";
        return TurnMemoryDispatch::DroppedRoute;
    }
    lubancode::app::TurnMemoryExtractor::Inputs inputs;
    inputs.backend = std::move(detached.backend);
    inputs.model = std::move(detached.route.model);
    inputs.effort = std::move(detached.route.effort);
    inputs.system_prompt = system_prompt;   // 前台拼好的冻结快照
    inputs.transcript = turn_transcript;    // 同上:迟到不串新轮的材料边界
    inputs.task_type = task_type;
    inputs.session_generation = ctx.session_generation;
    inputs.turn_id = ctx.turn_id;
    inputs.trajectory = ctx.trajectory;
    inputs.trajectory_wire = ctx.trajectory_wire;
    inputs.provider = detached.route.provider;
    if (!ctx.extractor->Start(std::move(inputs))) {
        // 竞态兜底:门后单飞被抢(与门前的 Busy 检查同一口径)。
        if (memory_turns != nullptr) {
            lubancode::app::MemoryTurnLedger::ExtractOutcome outcome;
            outcome.ok = false;
            outcome.error_code = "in_flight_dropped";
            memory_turns->NoteExtractionOutcome(outcome);
        }
        return TurnMemoryDispatch::DroppedBusy;
    }
    return TurnMemoryDispatch::Dispatched;
}

// 迟到收账(回合总结异步化单;主线程空闲拍调,见控制器的
// DrainFinishedTurnMemory):完工的抽取结果入队候选、记台账落袋、打
// 完成/失败行。usage 的分角色记账在调用方(世代门之前——弃账也照记,
// token 是真花了的);世代门(换代弃迟到)也在调用方,这里只管对档。
void SettleTurnMemory(const SessionTailContext& ctx, const TurnMemoryExtractor::Outcome& outcome,
                      std::int64_t tail_wall_ms) {
    lubancode::memory::ProjectMemory* project_memory = ctx.project_memory;
    const lubancode::cli::Theme& theme = *ctx.theme;
    lubancode::app::MemoryTurnLedger* memory_turns = ctx.memory_turns;

    lubancode::app::MemoryTurnLedger::ExtractOutcome ledger_outcome;
    ledger_outcome.usage_reported = outcome.accounting.usage_reported;
    ledger_outcome.input_tokens = outcome.accounting.usage.input_tokens;
    ledger_outcome.output_tokens = outcome.accounting.usage.output_tokens;
    ledger_outcome.cached_tokens = outcome.accounting.usage.cache_read_tokens +
                                    outcome.accounting.usage.cache_creation_tokens;
    ledger_outcome.extract_wall_ms = outcome.extract_wall_ms;

    if (!outcome.ok) {
        // 终端只出短错误与定位号(P0-A):诊断细节在错误对象里,查原文走
        // 受控轨迹。本地超时预算到点单独一行,不指控按键。
        if (outcome.error.code == lubancode::app::ExtractionErrorCode::DeadlineTimeout) {
            TermOut() << theme.stats << trf("memory.extract.deadline", kMemoryExtractTimeoutSecs)
                      << theme.reset << "\n";
        } else {
            TermOut() << theme.stats << trf("memory.extract.failed", outcome.error.message)
                      << theme.reset << "\n";
        }
        ledger_outcome.ok = false;
        ledger_outcome.error_code = StableExtractErrorCode(outcome.error);
        if (memory_turns != nullptr) {
            memory_turns->SettleSuspendedTurn(outcome.turn_id, tail_wall_ms, &ledger_outcome);
        }
        return;
    }

    // 检索扩展词:合并进 ProjectMemory,下一轮 BM25/词法查询用;learns off
    // 或失败时不清旧值,自然退回纯词法。
    std::vector<std::string> hints = outcome.extraction.retrieval_terms;
    hints.reserve(hints.size() + outcome.extraction.candidates.size());
    for (const auto& candidate : outcome.extraction.candidates) {
        for (const auto& keyword : candidate.keywords) hints.push_back(keyword);
    }
    if (!hints.empty()) project_memory->SetRetrievalHints(std::move(hints));

    std::size_t queued = 0;
    std::size_t auto_queued = 0;
    std::size_t start_failed = 0;  // 排队成功但 worker 没起来(修复单 §五 B:两笔账分开)
    for (const auto& proposed : outcome.extraction.candidates) {
        lubancode::memory::MemoryCandidate candidate;
        auto kind = lubancode::memory::ParseMemoryKind(proposed.kind);
        if (!kind.has_value()) continue;
        candidate.kind = *kind;
        candidate.title = proposed.title;
        candidate.summary = proposed.summary;
        candidate.content = proposed.content;
        candidate.keywords = proposed.keywords;
        candidate.paths = proposed.paths;
        candidate.confidence = proposed.confidence;
        candidate.occurred_at = proposed.occurred_at;
        candidate.task_type = outcome.task_type;

        // auto 档直写闸:inferred 只进候选区;fact 须 verified 且带证据,
        // feedback 须用户明说,否则也落待审区让人把关(规格"inferred 只准
        // 进候选区"、"模型推断不得直写 feedback")。
        const bool auto_writable = project_memory->learn_mode() == lubancode::memory::LearnMode::Auto &&
                                   candidate.confidence != "inferred" &&
                                   !(candidate.kind == lubancode::memory::MemoryKind::Fact &&
                                     (candidate.confidence != "verified" || candidate.paths.empty())) &&
                                   !(candidate.kind == lubancode::memory::MemoryKind::Feedback &&
                                     candidate.confidence != "user-stated");
        if (auto_writable) {
            lubancode::memory::SaveRequest request;
            request.kind = candidate.kind;
            request.title = candidate.title;
            request.summary = candidate.summary;
            request.content = candidate.content;
            request.keywords = candidate.keywords;
            request.paths = candidate.paths;
            request.occurred_at = candidate.occurred_at;
            const auto enqueued = project_memory->EnqueueSave(
                request, /*user_initiated=*/false,
                lubancode::memory::MemoryWriteSource::AutoExtraction);
            if (enqueued.has_value()) {
                ++auto_queued;
                if (enqueued->worker_state ==
                    lubancode::memory::MemoryWorkerLaunchState::StartFailed) {
                    ++start_failed;
                }
                continue;
            }
        }
        if (project_memory->AddCandidate(std::move(candidate)).has_value()) {
            ++queued;
        }
    }
    if (queued + auto_queued > 0) {
        TermOut() << theme.stats << trf("memory.extract.done", queued, auto_queued) << theme.reset << "\n";
        if (start_failed > 0) {
            TermOut() << theme.stats << trf("cmd.memory.worker_failed_hint_jobs", start_failed)
                      << theme.reset << "\n";
        }
    }
    // 记忆写入调度单 P0(§10.2/§10.3):收口账——候选/直写计数与 Token。
    // provider 没报 usage 时 token 三项整组缺席,不拿 0 顶上。
    ledger_outcome.ok = true;
    ledger_outcome.review_candidates = queued;
    ledger_outcome.auto_queued = auto_queued;
    if (memory_turns != nullptr) {
        memory_turns->SettleSuspendedTurn(outcome.turn_id, tail_wall_ms, &ledger_outcome);
    }
}

// (T17/V3-ADD-03:SummarizeArtifactOnDemand——context_read(summarize=true)
// 的按需摘要——已随旧 artifact 仓退役:context_tools 与 microcompact 整链
// 删除,cheap 路由不再有 Microcompact 任务档。)

// 命令分派注册制(会话终章):/memory 的分派位——命令与排版全在本文件,
// 分派位只递会话状态(工具补注册走回调)。
CommandFlow HandleSlashMemory(SlashDispatchContext& dispatch, const lubancode::cli::ParsedSlashCommand& parsed) {
    lubancode::app::MemoryCommandContext memory_ctx;
    memory_ctx.project_memory = dispatch.project_memory;
    memory_ctx.theme = dispatch.theme;
    memory_ctx.ensure_tool = dispatch.ensure_memory_tool;
    HandleMemoryCommand(memory_ctx, parsed.args);
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
