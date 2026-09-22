// /memory 命令 presenter 实现(合同见 memory_commands.hpp)。函数体自
// interactive_session 的 HandleMemoryCommand 原文搬家(改道:project_memory
// 与 theme 走 ctx、EnsureMemoryTool 走 ensure_tool 回调、输出走 TerminalPort),
// 行为一字不差——注释一并随行。

#include "app/commands/memory_commands.hpp"

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
#include "cli/terminal_frame.hpp"  // frame::*(TUI 排版批 1:/memory 渲染段)
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "memory/project_memory.hpp"
#include "platform/console.hpp"  // GetScreenInfo:整条 /memory 的框宽同一把尺
#include "runtime/trajectory_session.hpp"  // TrajectorySessionLedger(旁路桥)
#include "tools/path_utils.hpp"

namespace lubancode::app {

using lubancode::cli::TermOut;
using lubancode::cli::tr;
using lubancode::cli::trf;

namespace {

namespace frame = lubancode::cli::frame;

std::string TrimAscii(std::string value);  // 定义见下(拆句两侧衬空用)

// ---- TUI 排版批 1(/memory 全套)的公共小件 --------------------------------
//
// 渲染段只调 cli::frame::* 三助手(批 0 基件,约定见
// docs/development/tui_style.md);文案一律既有 tr()/trf() 键,i18n 不新增
// (单子合同第 5 条)。表头与档位标识用数据字段名(job/state/.../weak)——
// 是 schema 名不是待译文案,对中英文用户一视同仁。

// 宽度统一:整条 /memory 各子命令的框吃同一把终端列宽(单子批 1 验收
// "所有输出宽度统一");探不到(管道/重定向/CI)给 0 = 按内容自适应。
int MemoryFrameWidth() {
    if (const auto info = lubancode::platform::GetScreenInfo()) {
        return info->width;
    }
    return 0;
}

// 三助手吐 vector<string>(行内无换行符),落盘由调用方逐行走 TermOut
// ——基件只产行,端口不换(文档总规矩)。
void EmitFrameLines(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        TermOut() << line << "\n";
    }
}

// 一句既有文案 -> 键值对的一枚 Field:文案自带 "key: value" 句式的按第
// 一个冒号拆两列(冒号两侧衬空剥掉——助手自带两格列距,双份空格难看);
// 不带冒号的整句进 value。拆的是既有文案,不添不改一个字;中英两套
// memory 文案的冒号都是半角,同一把尺通吃。
frame::Field SentenceField(const std::string& sentence,
                           frame::FieldAccent accent = frame::FieldAccent::None) {
    const std::size_t colon = sentence.find(':');
    if (colon == std::string::npos) {
        return frame::Field{"", sentence, accent};
    }
    std::string key = TrimAscii(sentence.substr(0, colon));
    std::string value = TrimAscii(sentence.substr(colon + 1));
    return frame::Field{std::move(key), std::move(value), accent};
}

// 简短提示进 frame(单子批 1:"成功/失败提示收进 frame,不再裸打印"):
// 一到几句既有文案 -> 一个键值对框。accent 递 Error/Stats 给失败/告警上
// 语义色(文档:错误与警告仍走 theme.error/theme.stats,不另立色)。
void PrintNotice(const lubancode::cli::Theme& theme, std::initializer_list<std::string> sentences,
                 frame::FieldAccent accent = frame::FieldAccent::None) {
    std::vector<frame::Field> fields;
    for (const std::string& sentence : sentences) {
        fields.push_back(SentenceField(sentence, accent));
    }
    EmitFrameLines(
        frame::RenderKeyValues({}, fields, theme, frame::Light(), MemoryFrameWidth()));
}

// 同上,但吃现成的 Field 列表(一句一档 accent 的混排用)。
void PrintNoticeFields(const lubancode::cli::Theme& theme, std::vector<frame::Field> fields) {
    EmitFrameLines(
        frame::RenderKeyValues({}, std::move(fields), theme, frame::Light(), MemoryFrameWidth()));
}

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
// 说明);启动失败另起一行短提示附诊断入口,不吞队列成功值。批 1 起两句
// 同进一个键值对框(成功句素净,启动失败句上 error 色)。
void PrintEnqueueResult(const lubancode::cli::Theme& theme,
                        const lubancode::memory::MemoryEnqueueResult& result) {
    std::vector<frame::Field> fields{SentenceField(trf("cmd.memory.queued", result.job_id))};
    if (result.worker_state == lubancode::memory::MemoryWorkerLaunchState::StartFailed) {
        fields.push_back(SentenceField(trf("cmd.memory.worker_failed",
                                          result.worker_error.empty()
                                              ? result.worker_error_code
                                              : result.worker_error),
                                       frame::FieldAccent::Error));
    }
    PrintNoticeFields(theme, std::move(fields));
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
        PrintNotice(theme, {tr("cmd.memory.unavailable")}, frame::FieldAccent::Error);
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
        // 键值对助手(单子批 1):各句既有文案按 "key: value" 句式拆两列,
        // key 列全表对齐;counts/status 一键塞多对的,首对进 key、余下整段
        // 进 value(不拆分号——中英文分号全半角不一,拆了脆)。
        std::vector<frame::Field> fields;
        fields.push_back(SentenceField(trf("cmd.memory.global", toggle_word(status.global_allowed))));
        fields.push_back(SentenceField(trf("cmd.memory.status", toggle_word(status.enabled),
                                           toggle_word(status.use), toggle_word(status.generate))));
        fields.push_back(SentenceField(trf("cmd.memory.learn_status", status.learn)));
        fields.push_back(SentenceField(trf("cmd.memory.project", status.workspace_key)));
        fields.push_back(
            SentenceField(trf("cmd.memory.directory", lubancode::tools::PathToUtf8(status.memory_dir))));
        fields.push_back(SentenceField(trf("cmd.memory.counts", status.entry_count, status.pending_jobs,
                                           status.failed_jobs)));
        if (status.user_enabled) {
            fields.push_back(SentenceField(trf("cmd.memory.user_status", status.user_entry_count,
                                               lubancode::tools::PathToUtf8(status.user_memory_dir))));
        }
        fields.push_back(SentenceField(trf("cmd.memory.candidates", status.pending_candidates)));
        EmitFrameLines(
            frame::RenderKeyValues({}, fields, theme, frame::Light(), MemoryFrameWidth()));
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
                PrintNotice(theme, {retried.has_value()
                                        ? trf("cmd.memory.jobs.retry_queued", *retried)
                                        : trf("cmd.memory.jobs.retry_rejected", retried.error())},
                            retried.has_value() ? frame::FieldAccent::None : frame::FieldAccent::Error);
                return;
            }
            const auto woken = project_memory->WakePendingWorker();
            switch (woken.state) {
                case lubancode::memory::MemoryWorkerLaunchState::Started:
                    PrintNotice(theme, {tr("cmd.memory.jobs.retry_started")});
                    break;
                case lubancode::memory::MemoryWorkerLaunchState::StartFailed:
                    PrintNotice(theme, {trf("cmd.memory.worker_failed", woken.error)},
                                frame::FieldAccent::Error);
                    break;
                case lubancode::memory::MemoryWorkerLaunchState::Unavailable:
                    PrintNotice(theme, {tr("cmd.memory.jobs.retry_unavailable")},
                                frame::FieldAccent::Stats);
                    break;
                case lubancode::memory::MemoryWorkerLaunchState::Idle:
                    PrintNotice(theme, {tr("cmd.memory.jobs.retry_idle")});
                    break;
                case lubancode::memory::MemoryWorkerLaunchState::AlreadyRunning:
                    PrintNotice(theme, {tr("cmd.memory.jobs.retry_running")});
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
            PrintNotice(theme, {tr("cmd.memory.jobs.empty")});
            return;
        }
        // 主账表格(单子批 1):六段数据各成一列,表头用字段名(schema 名,
        // 语言无关);state 上语义色——failed 走 error、pending 走 skip 档。
        std::vector<frame::TableColumn> columns;
        columns.push_back({"job"});
        columns.push_back({"state"});
        columns.push_back({"op"});
        columns.push_back({"layer"});
        columns.push_back({"wait"});
        columns.push_back({"worker"});
        std::vector<frame::TableRow> rows;
        for (const auto& job : jobs) {
            const frame::CellTone state_tone =
                job.state == "failed" ? frame::CellTone::Fail
                                      : (job.state == "pending" ? frame::CellTone::Skip
                                                                : frame::CellTone::Normal);
            rows.push_back(frame::TableRow{
                {job.job_id, job.state, job.operation, job.layer, job.wait_hint, job.worker_state},
                {frame::CellTone::Normal, state_tone}});
        }
        EmitFrameLines(frame::RenderTable(tr("cmd.memory.jobs.header"), columns, rows, theme,
                                          frame::Light(), MemoryFrameWidth()));
        // 附注框:标题/失败/worker 日志与 retry 用法是人话短注,另起一个
        // 键值对框收口——塞主表会被列宽截断丢信息(表帽优先削最宽列)。
        std::vector<frame::Field> notes;
        for (const auto& job : jobs) {
            if (!job.title.empty()) {
                notes.push_back(SentenceField(trf("cmd.memory.jobs.title_line", job.title)));
            }
            if (!job.error.empty()) {
                notes.push_back(
                    SentenceField(trf("cmd.memory.jobs.error_line", job.error), frame::FieldAccent::Error));
            }
        }
        if (!jobs.front().worker_log.empty()) {
            notes.push_back(SentenceField(trf("cmd.memory.jobs.log_line", jobs.front().worker_log)));
        }
        notes.push_back(SentenceField(tr("cmd.memory.jobs.hint")));
        PrintNoticeFields(theme, std::move(notes));
        return;
    }
    if (action == "on" || action == "off") {
        // 授权闸:全局未授权时 /memory on 只会得到"去哪改全局配置"的指引,
        // 不能凭本场命令翻开能力(规格"授权与本场状态分开")。
        const auto toggled = project_memory->set_enabled(action == "on");
        if (!toggled.has_value()) {
            PrintNotice(theme, {tr("cmd.memory.denied")}, frame::FieldAccent::Error);
            return;
        }
        if (action == "on") ensure_tool();
        PrintNotice(theme, {trf("cmd.memory.master",
                                action == "on" ? tr("cmd.memory.on") : tr("cmd.memory.off"))});
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
            PrintNotice(theme, {tr("cmd.memory.denied")}, frame::FieldAccent::Error);
            return;
        }
        project_memory->set_use(enabled);
        PrintNotice(theme, {trf("cmd.memory.toggle", tr("cmd.memory.retrieval"),
                                enabled ? tr("cmd.memory.on") : tr("cmd.memory.off"))});
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
                PrintNotice(theme, {tr("cmd.memory.denied")}, frame::FieldAccent::Error);
            } else {
                PrintNotice(theme, {trf("cmd.memory.learn_denied", switched.error())},
                            frame::FieldAccent::Error);
            }
            return;
        }
        ensure_tool();
        PrintNotice(theme,
                    {trf("cmd.memory.learn_set", lubancode::memory::LearnModeName(*mode))});
        return;
    }
    if (action == "review") {
        const auto candidates = project_memory->ListCandidates();
        if (candidates.empty()) {
            PrintNotice(theme, {tr("cmd.memory.review.empty")});
            return;
        }
        // 列表助手(与 /memory list 同构):id 对齐列 + kind/置信档/标题,
        // summary 作行尾短注(key_hint 色)。候选未定层,项目符走 Project 档。
        std::vector<frame::ListRow> rows;
        for (const auto& candidate : candidates) {
            frame::ListRow row;
            row.label = candidate.id;
            row.value = "[" + lubancode::memory::MemoryKindName(candidate.kind) + "/" +
                        candidate.confidence + "] " + candidate.title;
            if (!candidate.summary.empty() && candidate.summary != candidate.title) {
                row.hint = candidate.summary;
            }
            row.bullet = frame::Bullet::Project;
            rows.push_back(std::move(row));
        }
        EmitFrameLines(
            frame::RenderList(tr("cmd.memory.review.header"), rows, theme, frame::Light(),
                              MemoryFrameWidth()));
        PrintNotice(theme, {tr("cmd.memory.review.hint")});
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
                PrintEnqueueResult(theme, *queued);
            } else {
                PrintNotice(theme, {trf("cmd.memory.queue_failed", queued.error())},
                            frame::FieldAccent::Error);
            }
        } else {
            const auto rejected = project_memory->RejectCandidate(id, std::move(reason));
            if (rejected.has_value()) {
                PrintNotice(theme, {tr("cmd.memory.reject.done")});
            } else {
                PrintNotice(theme, {trf("cmd.memory.queue_failed", rejected.error())},
                            frame::FieldAccent::Error);
            }
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
        if (edited.has_value()) {
            PrintNotice(theme, {tr("cmd.memory.edit.done")});
        } else {
            PrintNotice(theme, {trf("cmd.memory.queue_failed", edited.error())},
                        frame::FieldAccent::Error);
        }
        return;
    }
    if (action == "why") {
        std::string id;
        words >> id;
        const auto trace = project_memory->LastTrace();
        if (!trace.valid) {
            PrintNotice(theme, {tr("cmd.memory.why.none")});
            return;
        }
        // 头部框:标题用 why.header 既有文案,来源进键值对;skipped 的早退
        // 次序照旧(header/origin 之后即收,检索词与条目表都不出)。
        std::vector<frame::Field> head{SentenceField(trf("cmd.memory.why.origin", trace.query_origin))};
        // 检索词带词路与权重:word=整词/词典实体,gram=中文二元,虚词碎片
        // 拿低权重——用户要看得出为何命中,不只见一把碎字。
        std::ostringstream joined_terms;
        for (std::size_t i = 0; i < trace.terms.size(); ++i) {
            if (i != 0) joined_terms << " ";
            joined_terms << trace.terms[i].text << "[" << trace.terms[i].kind << "/"
                         << trace.terms[i].source << " ×" << trace.terms[i].weight << "]";
        }
        if (!trace.skipped) {
            head.push_back(SentenceField(trf("cmd.memory.why.terms", joined_terms.str())));
        }
        EmitFrameLines(frame::RenderKeyValues(trf("cmd.memory.why.header", trace.at), head, theme,
                                              frame::Light(), MemoryFrameWidth()));
        if (trace.skipped) {
            PrintNotice(theme, {tr("cmd.memory.why.skipped_turn")});
            return;
        }
        // 条目表(单子批 1):id/四项数值列(右对齐)/result。注入行 Pass 色
        // (弱档 result 标 weak——数据字段名),落选行 Skip 色带既有原因
        // 短句;bytes 即原 hit/weak_hit 句尾的注入字节数,cooccur 是弱档
        // 判据的单行共现词组数,非弱档为空。
        std::vector<frame::TableColumn> columns;
        columns.push_back({"id"});
        columns.push_back({"score", 0, /*align_right=*/true});
        columns.push_back({"hard", 0, /*align_right=*/true});
        columns.push_back({"terms", 0, /*align_right=*/true});
        columns.push_back({"bytes", 0, /*align_right=*/true});
        columns.push_back({"cooccur", 0, /*align_right=*/true});
        columns.push_back({"result"});
        std::vector<frame::TableRow> rows;
        bool matched_id = id.empty();
        for (const auto& entry : trace.entries) {
            if (!id.empty() && entry.id != id) continue;
            matched_id = true;
            // 命中来自哪一层:用户层带标注,项目层不打扰(规格"/memory why
            // 须写清命中来自 user 还是某个 project key")。
            const std::string shown_id =
                entry.layer == "user" ? entry.id + tr("cmd.memory.why.layer_user") : entry.id;
            std::string result;
            frame::CellTone tone = frame::CellTone::Normal;
            if (entry.injected) {
                tone = frame::CellTone::Pass;
                if (entry.weak) result = "weak";
            } else {
                tone = frame::CellTone::Skip;
                if (entry.expired) result = tr("cmd.memory.why.expired");
                else if (entry.scope_blocked) result = tr("cmd.memory.why.scope");
                else if (entry.stale_blocked) result = tr("cmd.memory.why.stale");
                else if (entry.snapshot_failed) result = tr("cmd.memory.why.snapshot_failed");
                else if (entry.layer_superseded) result = tr("cmd.memory.why.superseded");
                else if (entry.duplicate_dropped) result = tr("cmd.memory.why.duplicate");
                else if (entry.weak_dropped) result = tr("cmd.memory.why.weak_dropped");
                else if (entry.below_threshold) result = tr("cmd.memory.why.below_threshold");
                else if (entry.budget_dropped) result = tr("cmd.memory.why.budget");
                else result = tr("cmd.memory.why.skipped");
            }
            rows.push_back(frame::TableRow{
                {shown_id, std::to_string(entry.score), std::to_string(entry.hard_hits),
                 std::to_string(entry.term_hits), std::to_string(entry.bytes),
                 entry.injected && entry.weak ? std::to_string(entry.cooccur) : std::string(),
                 std::move(result)},
                {frame::CellTone::Normal, frame::CellTone::Normal, frame::CellTone::Normal,
                 frame::CellTone::Normal, frame::CellTone::Normal, tone, tone}});
        }
        EmitFrameLines(
            frame::RenderTable({}, columns, rows, theme, frame::Light(), MemoryFrameWidth()));
        if (!matched_id) {
            PrintNotice(theme, {trf("cmd.memory.why.missing", id)}, frame::FieldAccent::Error);
        }
        PrintNotice(theme, {trf("cmd.memory.why.total", trace.injected_count, trace.injected_bytes)});
        return;
    }
    if (action == "list") {
        const std::string layer = ParseLayerWord(words);
        std::string error;
        // 两层合并列:项目层在前,全局层带标注。显式 global 只列全局层
        //(管理读口,不看召回授权)。
        const auto entries = layer == "user" ? std::vector<lubancode::memory::MemoryEntry>{}
                                             : project_memory->ListEntries(&error);
        if (!error.empty()) {
            PrintNotice(theme, {trf("cmd.memory.catalog_warning", error)}, frame::FieldAccent::Stats);
        }
        const auto user_entries =
            layer == "user" ? project_memory->ListGlobalEntriesForManagement(&error)
                            : project_memory->ListUserEntries(&error);
        if (!error.empty()) {
            PrintNotice(theme, {trf("cmd.memory.catalog_warning", error)}, frame::FieldAccent::Stats);
        }
        // 条目行:id 对齐列,label=id、value="[Kind] 标题 - 摘要"、hint=层
        // 标注;项目符两档(list_bullet_user/project,批 0 主题字段的设计
        // 用例)与行尾 "(全局记忆)"(key_hint 色)就是 layer 标色的两处落笔。
        const auto entry_row = [](const lubancode::memory::MemoryEntry& entry, frame::Bullet bullet,
                                  std::string layer_note) {
            frame::ListRow row;
            row.label = entry.id;
            row.value = "[" + lubancode::memory::MemoryKindName(entry.kind) + "] " + entry.title;
            if (!entry.summary.empty() && entry.summary != entry.title) {
                row.value += " - " + entry.summary;
            }
            row.hint = std::move(layer_note);
            row.bullet = bullet;
            return row;
        };
        if (entries.empty() && user_entries.empty()) {
            // 空态也带 frame(单子批 1):空库提示 + 待写/worker 两句同框。
            std::vector<frame::ListRow> rows;
            rows.push_back(frame::ListRow{tr("cmd.memory.empty"), {}, {}, frame::Bullet::None});
            const auto status = project_memory->Status();
            if (status.pending_jobs > 0 || status.failed_jobs > 0) {
                rows.push_back(frame::ListRow{trf("cmd.memory.pending_hint", status.pending_jobs), {},
                                              {}, frame::Bullet::None});
                if (const auto woken = project_memory->EnsureWorkerRunning();
                    woken.state == lubancode::memory::MemoryWorkerLaunchState::StartFailed) {
                    rows.push_back(frame::ListRow{trf("cmd.memory.worker_failed", woken.error), {},
                                                  {}, frame::Bullet::None});
                }
            }
            EmitFrameLines(
                frame::RenderList({}, rows, theme, frame::Light(), MemoryFrameWidth()));
            return;
        }
        std::vector<frame::ListRow> rows;
        rows.reserve(entries.size() + user_entries.size());
        for (const auto& entry : entries) {
            rows.push_back(entry_row(entry, frame::Bullet::Project, {}));
        }
        for (const auto& entry : user_entries) {
            rows.push_back(entry_row(entry, frame::Bullet::User, tr("cmd.memory.global_layer")));
        }
        EmitFrameLines(frame::RenderList({}, rows, theme, frame::Light(), MemoryFrameWidth()));
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
            PrintNotice(theme, {tr("cmd.memory.global.no_fact")}, frame::FieldAccent::Error);
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
                PrintNotice(theme, {tr("cmd.memory.remember.legacy_hint")}, frame::FieldAccent::Stats);
            }
        }
        // 全局层逐次确认(§6.1:写入永远须用户主动授权与主动命令)。
        if (to_global &&
            !ConfirmGlobalAction(theme, trf("cmd.memory.global.confirm", request.title))) {
            PrintNotice(theme, {tr("cmd.memory.global.cancelled")});
            return;
        }
        const auto queued = project_memory->EnqueueSave(request, /*user_initiated=*/true,
                                                        lubancode::memory::MemoryWriteSource::ExplicitCommandSave);
        if (queued.has_value()) {
            PrintEnqueueResult(theme, *queued);
        } else {
            PrintNotice(theme, {trf("cmd.memory.queue_failed", queued.error())},
                        frame::FieldAccent::Error);
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
            PrintNotice(theme, {tr("cmd.memory.global.cancelled")});
            return;
        }
        const auto queued = project_memory->EnqueueForget(id, layer);
        if (queued.has_value()) {
            PrintEnqueueResult(theme, *queued);
        } else {
            PrintNotice(theme, {trf("cmd.memory.queue_failed", queued.error())},
                        frame::FieldAccent::Error);
        }
        return;
    }
    if (action == "rebuild") {
        const auto queued = project_memory->EnqueueRebuild();
        if (queued.has_value()) {
            PrintEnqueueResult(theme, *queued);
        } else {
            PrintNotice(theme, {trf("cmd.memory.queue_failed", queued.error())},
                        frame::FieldAccent::Error);
        }
        return;
    }
    if (action == "stale") {
        const auto stale = project_memory->ListStaleEntries();
        if (stale.empty()) {
            PrintNotice(theme, {tr("cmd.memory.stale.empty")});
            return;
        }
        // 列表助手:id 对齐列,reason(数据值)与标题进 value,行尾短注给
        // "文件已变/已过期"的人话标注(key_hint 色)。
        std::vector<frame::ListRow> rows;
        for (const auto& item : stale) {
            frame::ListRow row;
            row.label = item.entry.id;
            row.value = "[" + item.reason + "] " + item.entry.title;
            row.hint = item.reason == "fingerprint"
                           ? tr("cmd.memory.stale.fingerprint")
                           : tr("cmd.memory.stale.expired") + ": " + item.entry.expires_at;
            row.bullet = frame::Bullet::None;
            rows.push_back(std::move(row));
        }
        EmitFrameLines(frame::RenderList(tr("cmd.memory.stale.header"), rows, theme, frame::Light(),
                                         MemoryFrameWidth()));
        PrintNotice(theme, {tr("cmd.memory.stale.hint")});
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
            PrintEnqueueResult(theme, *queued);
        } else {
            PrintNotice(theme, {trf("cmd.memory.queue_failed", queued.error())},
                        frame::FieldAccent::Error);
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
            PrintNotice(theme, {trf("cmd.memory.queue_failed", topic.error())},
                        frame::FieldAccent::Error);
            return;
        }
        const auto& [text, dir] = *topic;
        if (layer == "user" && dir != project_memory->user_memory_dir()) {
            PrintNotice(theme, {trf("cmd.memory.queue_failed", tr("cmd.memory.global.layer_mismatch"))},
                        frame::FieldAccent::Error);
            return;
        }
        // 头部一行键值对框:id(加粗列)+ 所在目录(淡色);正文是 markdown
        // 文档,框外原样跟出——塞框会被列帽截断劈行,保终端自然折行。
        EmitFrameLines(frame::RenderKeyValues(
            {}, {frame::Field{id, lubancode::tools::PathToUtf8(dir), frame::FieldAccent::Muted}},
            theme, frame::Light(), MemoryFrameWidth()));
        TermOut() << text;
        if (!text.empty() && text.back() != '\n') TermOut() << "\n";
        return;
    }
    if (action == "open") {
        std::string id;
        words >> id;
        const auto edited = id.empty() ? project_memory->OpenIndexInEditor()
                                       : project_memory->EditTopicInEditor(id);
        if (edited.has_value()) {
            PrintNotice(theme, {tr("cmd.memory.open.done")});
        } else {
            PrintNotice(theme, {trf("cmd.memory.queue_failed", edited.error())},
                        frame::FieldAccent::Error);
        }
        return;
    }
    if (action == "migrate") {
        // 先列将改/跳过/警告几份,经确认才动盘;原件备进
        // .state/migration-backup/<时间>/,全部写妥、重建成功才报完成。
        const auto plan = project_memory->PlanMigration();
        if (plan.to_migrate == 0) {
            PrintNotice(theme, {trf("cmd.memory.migrate.none", plan.to_skip, plan.warnings)});
            return;
        }
        PrintNotice(theme,
                    {trf("cmd.memory.migrate.plan", plan.to_migrate, plan.to_skip, plan.warnings)});
        // 迁移账单走列表:id 对齐列,迁项带 (文件; 原因),警告项
        // "[warn]" 是数据档标识(action 字段的值域)。
        std::vector<frame::ListRow> rows;
        for (const auto& item : plan.items) {
            if (item.action == "migrate") {
                rows.push_back(frame::ListRow{item.id, "(" + item.file + "; " + item.reason + ")",
                                              {}, frame::Bullet::None});
            } else if (item.action == "warn") {
                rows.push_back(frame::ListRow{"[warn]", item.reason, {}, frame::Bullet::None});
            }
        }
        if (!rows.empty()) {
            EmitFrameLines(
                frame::RenderList({}, rows, theme, frame::Light(), MemoryFrameWidth()));
        }
        const auto answer = lubancode::cli::ReadLine(theme.confirm + tr("cmd.memory.migrate.confirm") + theme.reset,
                                                     theme, /*esc_rejects=*/true);
        if (!answer.has_value() || (*answer != "y" && *answer != "Y")) {
            PrintNotice(theme, {tr("cmd.memory.migrate.cancelled")});
            return;
        }
        const auto result = project_memory->RunMigration();
        if (result.has_value()) {
            PrintNotice(
                theme, {trf("cmd.memory.migrate.done", result->migrated, result->backup_dir)});
        } else {
            PrintNotice(theme, {trf("cmd.memory.queue_failed", result.error())},
                        frame::FieldAccent::Error);
        }
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
// 分派位只递会话状态(工具补注册走回调)。HC-06(材料收窄,第二小批)起
// 只吃窄材料,材料在组合根折好。
CommandFlow HandleSlashMemory(const MemoryCommandContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    HandleMemoryCommand(ctx, parsed.args);
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
