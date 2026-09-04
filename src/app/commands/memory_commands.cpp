// /memory 命令 presenter 实现(合同见 memory_commands.hpp)。函数体自
// interactive_session 的 HandleMemoryCommand 原文搬家(改道:project_memory
// 与 theme 走 ctx、EnsureMemoryTool 走 ensure_tool 回调、输出走 TerminalPort),
// 行为一字不差——注释一并随行。

#include "app/commands/memory_commands.hpp"
#include "app/commands/command_registry.hpp"  // SlashDispatchContext(分派注册制)

#include <algorithm>
#include <cctype>
#include <memory>
#include <sstream>
#include <utility>

#include "accounting/purpose.hpp"  // RequestPurpose(Token 账本单 A1)
#include "agent/microcompact.hpp"  // RunMicrocompact(按需摘要)
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
                  << trf("cmd.memory.counts", status.entry_count, status.pending_jobs) << "\n";
        if (status.user_enabled) {
            TermOut() << trf("cmd.memory.user_status", status.user_entry_count,
                             lubancode::tools::PathToUtf8(status.user_memory_dir))
                      << "\n";
        }
        TermOut() << trf("cmd.memory.candidates", status.pending_candidates) << "\n";
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
            TermOut() << (queued.has_value() ? trf("cmd.memory.queued", *queued)
                                             : trf("cmd.memory.queue_failed", queued.error()))
                      << "\n";
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
        const auto queued = project_memory->EnqueueSave(request, /*user_initiated=*/true);
        TermOut() << (queued.has_value() ? trf("cmd.memory.queued", *queued)
                                         : trf("cmd.memory.queue_failed", queued.error()))
                  << "\n";
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
        TermOut() << (queued.has_value() ? trf("cmd.memory.queued", *queued)
                                         : trf("cmd.memory.queue_failed", queued.error()))
                  << "\n";
        return;
    }
    if (action == "rebuild") {
        const auto queued = project_memory->EnqueueRebuild();
        TermOut() << (queued.has_value() ? trf("cmd.memory.queued", *queued)
                                         : trf("cmd.memory.queue_failed", queued.error()))
                  << "\n";
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
        TermOut() << (queued.has_value() ? trf("cmd.memory.queued", *queued)
                                         : trf("cmd.memory.queue_failed", queued.error()))
                  << "\n";
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

void ExtractTurnMemory(const SessionTailContext& ctx, const std::string& user_text, std::size_t history_before) {
    lubancode::memory::ProjectMemory* project_memory = ctx.project_memory;
    const lubancode::cli::Theme& theme = *ctx.theme;
    lubancode::app::ModelRouterService& model_router = *ctx.model_router;
    if (project_memory == nullptr || !project_memory->generate_enabled()) return;

    const auto& history = ctx.agent->History();
    if (history_before >= history.size()) return;
    std::vector<api::Message> slice(history.begin() + static_cast<std::ptrdiff_t>(history_before),
                                    history.end());

    // 工具名清单喂给分型器;转写压缩后整段不超 24 KiB。
    std::vector<std::string> tool_names;
    for (const auto& message : slice) {
        for (const auto& block : message.content) {
            if (const auto* use = std::get_if<api::ToolUseBlock>(&block)) {
                tool_names.push_back(use->name);
            }
        }
    }
    const std::string turn_transcript = BuildTurnTranscript(slice, 24 * 1024);
    if (turn_transcript.empty()) return;

    const std::string task_type = ClassifyTaskType(user_text, tool_names);
    const std::string system_prompt = BuildExtractionSystemPrompt(*ctx.prompts_dir, task_type);
    if (system_prompt.empty()) return;

    // 抽取走 cheap 角色(模型分工第一期):低风险后台小活,配了 cheap_model
    // 用便宜的,没配回落 normal(与 main 同模型,行为与从前一致)。状态栏
    // 短闪一行:任务种类 + 角色:模型(规格"运行提示")。采样走
    // ModelRouterService::Sample 一站(批一·病四):路由/采样/记账一扇门。
    const auto extract_route = model_router.RouteInfo(lubancode::agent::TaskKind::MemoryExtract);
    TermOut() << theme.stats
              << trf("router.task_flash", trf("memory.extract.running", task_type),
                     "cheap:" + extract_route.model)
              << theme.reset << "\n";
    lubancode::app::ModelRouterService::SampleCall sample_call;
    sample_call.system = system_prompt;
    {
        api::Message message;
        message.role = api::Role::User;
        message.content.push_back(api::TextBlock{turn_transcript});
        sample_call.messages.push_back(std::move(message));
        sample_call.max_tokens = 1500;
    }
    lubancode::agent::SampleOptions sample_options;
    sample_options.timeout_secs = 45;
    // Token 账本单 A1(旁路落账):抽取请求铸一只旁路桥,prepared/sent/
    // usage/output 连同 purpose=memory_extract 落 Journal。flag 关的会话
    //(trajectory 空)一笔不落,行为与从前一致。
    std::unique_ptr<lubancode::agent::LoopBoundaryRecorder> extract_recorder;
    if (ctx.trajectory != nullptr) {
        lubancode::runtime::TrajectoryTurnBridge::Identity identity{extract_route.provider, ctx.trajectory_wire,
                                                                    "host"};
        extract_recorder = ctx.trajectory->NewBypassBridge(std::move(identity));
        sample_options.boundary_recorder = extract_recorder.get();
        sample_options.purpose = lubancode::accounting::RequestPurpose::MemoryExtract;
    }
    const auto sampled =
        model_router.Sample(lubancode::agent::TaskKind::MemoryExtract, sample_call, sample_options);
    std::expected<MemoryExtraction, std::string> extraction;
    if (sampled.backend == nullptr) {
        // 路由落空:旧口径也记一笔零账(calls=1,零 token,"未报告"),不吞。
        model_router.ledger().Record(lubancode::agent::ModelRole::Cheap, sampled.route.model,
                                      lubancode::api::Usage{}, /*duration_ms=*/0, /*reported=*/false);
        extraction = std::unexpected("cheap 路由找不到 provider \"" + sampled.route.provider + "\"");
    } else {
        extraction = FinishMemoryExtraction(sampled.result);
    }
    if (!extraction.has_value()) {
        TermOut() << theme.stats << trf("memory.extract.failed", extraction.error()) << theme.reset << "\n";
        return;
    }

    // 检索扩展词:合并进 ProjectMemory,下一轮 BM25/词法查询用;learns off
    // 或失败时不清旧值,自然退回纯词法。
    std::vector<std::string> hints = extraction->retrieval_terms;
    hints.reserve(hints.size() + extraction->candidates.size());
    for (const auto& candidate : extraction->candidates) {
        for (const auto& keyword : candidate.keywords) hints.push_back(keyword);
    }
    if (!hints.empty()) project_memory->SetRetrievalHints(std::move(hints));

    std::size_t queued = 0;
    std::size_t written = 0;
    for (const auto& proposed : extraction->candidates) {
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
        candidate.task_type = task_type;

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
            if (project_memory->EnqueueSave(request).has_value()) {
                ++written;
                continue;
            }
        }
        if (project_memory->AddCandidate(std::move(candidate)).has_value()) {
            ++queued;
        }
    }
    if (queued + written > 0) {
        TermOut() << theme.stats << trf("memory.extract.done", queued, written) << theme.reset << "\n";
    }
}

std::expected<std::string, std::string> SummarizeArtifactOnDemand(const SessionTailContext& ctx,
                                                                  const lubancode::agent::ArtifactRef& ref) {
    if (ctx.model_router == nullptr || ctx.artifact_store == nullptr || !ctx.artifact_store->active()) {
        return std::unexpected("按需摘要暂不可用:artifact 仓或模型路由未就绪");
    }
    auto routed = ctx.model_router->RouteDetached(lubancode::agent::TaskKind::Microcompact);
    if (routed.route.model.empty()) {
        return std::unexpected("按需摘要暂不可用:cheap 模型未配置");
    }
    if (routed.backend == nullptr) {
        return std::unexpected("按需摘要暂不可用:cheap provider 找不到");
    }
    lubancode::agent::BackgroundCallAccounting accounting;
    // Token 账本单 A1:按需摘要(L2)也走旁路桥(purpose=compact_map)。
    lubancode::agent::MicrocompactOptions micro_options;
    if (ctx.trajectory != nullptr) {
        lubancode::runtime::TrajectorySessionLedger* ledger = ctx.trajectory;
        const std::string wire = ctx.trajectory_wire;
        const std::string provider = routed.route.provider;
        micro_options.bypass_recorder = [ledger, wire, provider]()
                                            -> std::unique_ptr<lubancode::agent::LoopBoundaryRecorder> {
            lubancode::runtime::TrajectoryTurnBridge::Identity identity{provider, wire, "host"};
            return ledger->NewBypassBridge(std::move(identity));
        };
    }
    auto summary = lubancode::agent::RunMicrocompact(
        *routed.backend, routed.route.model, routed.route.effort, *ctx.artifact_store, ref,
        std::move(micro_options), &accounting);
    ctx.model_router->ledger().Record(lubancode::agent::ModelRole::Cheap, routed.route.model,
                                      accounting.usage, accounting.duration_ms,
                                      accounting.usage_reported);
    if (!summary.has_value()) {
        return std::unexpected(summary.error());
    }
    std::string out = "artifact " + ref.artifact_id + "(" + ref.tool_name + ")按需摘要 · cheap:" +
                      routed.route.model + " · 原文未改:\n" + summary->summary;
    if (!summary->key_facts.empty()) {
        out += "\n关键事实:";
        for (const auto& fact : summary->key_facts) {
            out += "\n- " + fact;
        }
    }
    out += "\n摘要不作最终证据;有疑点请用 context_search/context_read 回看 artifact " +
           ref.artifact_id + " 原文。";
    return out;
}

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
