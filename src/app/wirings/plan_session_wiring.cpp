// Plan 模式接线器的实现(会话终章):函数体原样自 interactive_session
// 大类搬来(RestorePlanStateFrom/SwitchCollaborationMode/HandlePlanCommand/
// EvaluatePlanGate/MaybeCollectPlanProposal/RunPlanReviewPrompt/
// LaunchApprovedPlanExecution),材料换经 Host 递入,行为一字未改——注释
// 一并随行。
#include "app/wirings/plan_session_wiring.hpp"

#include <map>
#include <utility>
#include <vector>

#include "agent/agent.hpp"
#include "cli/choice_menu.hpp"
#include "cli/console_input.hpp"
#include "cli/i18n.hpp"
#include "cli/queue_model.hpp"
#include "cli/terminal_port.hpp"
#include "hooks/hash.hpp"  // Sha256Hex:PlanDocument 内容锚
#include "platform/console.hpp"
#include "tools/agent_tool.hpp"

namespace lubancode::app {

using lubancode::cli::tr;
using lubancode::cli::trf;
using lubancode::cli::TermOut;

PlanSessionWiring::PlanSessionWiring(Host host) : host_(std::move(host)) {}

void PlanSessionWiring::RestoreFromArchive(
    const std::optional<lubancode::sessions::ModeEvent>& mode_event,
    const std::vector<lubancode::sessions::PlanEvent>& plans,
    const std::optional<lubancode::sessions::PlanReviewEvent>& review) {
    using lubancode::runtime::CollaborationMode;
    using lubancode::runtime::PlanDocument;
    using lubancode::runtime::PlanReviewState;
    // 计划账:按 plan_id 取最高 revision(逐稿都在,取最新);审批只认与
    // 最新稿匹配的 approved(单子:批准须同时匹配 id/revision/hash)。
    std::map<std::string, const lubancode::sessions::PlanEvent*> latest_by_id;
    for (const auto& event : plans) {
        const auto it = latest_by_id.find(event.plan_id);
        if (it == latest_by_id.end() || event.revision >= it->second->revision) {
            latest_by_id[event.plan_id] = &event;
        }
    }
    std::optional<PlanDocument> restored_plan;
    for (const auto& [id, event] : latest_by_id) {
        (void)id;
        PlanDocument plan;
        plan.plan_id = event->plan_id;
        plan.revision = event->revision;
        if (!lubancode::runtime::ParsePlanReviewState(event->state, plan.state)) {
            plan.state = PlanReviewState::Presented;
        }
        plan.content_sha256 = event->sha256;
        plan.markdown = event->markdown;
        plan.artifact_ref = event->artifact_ref;
        plan.source_turn_id = event->turn_id;
        // 审批回放:匹配最新稿的 approved/rejected 盖掉 presented。
        if (review.has_value() && review->plan_id == plan.plan_id && review->revision == plan.revision) {
            if (review->decision == "approved") {
                plan.state = PlanReviewState::Approved;
            } else if (review->decision == "rejected") {
                plan.state = PlanReviewState::Rejected;
            }
        }
        restored_plan = plan;  // map 按序遍历,留下的是最后一个(多稿计划取最新 plan_id)
    }
    // mode 回放:最后一条 mode 事件决定档位;老档没行按 Default。恢复只
    // 回内存真值与提示段,不落 mode 事件(档位是回放出来的,再落一行会把
    // resume 当一次切换记账)。
    CollaborationMode restored_mode = CollaborationMode::Default;
    std::uint64_t restored_revision = 0;
    if (mode_event.has_value()) {
        lubancode::runtime::ParseCollaborationMode(mode_event->mode, restored_mode);
        restored_revision = mode_event->revision;
        restored_from_archive_ = true;  // 旧账有真值,起手档不再插手
    }
    // 崩溃恢复的事务规则(单子):Approved 已落、Default mode 未落——按
    // 事务恢复规则完成 mode 切换,但不自动重跑 implementation turn。
    if (restored_plan.has_value() && restored_plan->state == PlanReviewState::Approved &&
        restored_mode == CollaborationMode::Plan) {
        restored_mode = CollaborationMode::Default;
        TermOut() << host_.theme->stats << tr("plan.resume.approved_pending") << host_.theme->reset << "\n";
    }
    host_.session_runtime->RestoreCollaborationMode(restored_mode, restored_revision);
    host_.prompt_options->plan_mode = restored_mode == CollaborationMode::Plan;
    if (restored_mode == CollaborationMode::Plan) {
        TermOut() << host_.theme->stats << tr("plan.status.in_plan") << host_.theme->reset << "\n";
    }
    if (restored_plan.has_value()) {
        if (restored_plan->state == PlanReviewState::Presented) {
            review_pending_ = *restored_plan;  // 半路退出:审阅框可重开
        }
        host_.session_runtime->RestorePlanDocument(*restored_plan);
    }
    // 提示段跟着档位走:重拼(保历史)。
    host_.rebuild_preserving();
}

void PlanSessionWiring::SwitchMode(lubancode::runtime::CollaborationMode mode, const std::string& reason) {
    using lubancode::runtime::CollaborationMode;
    // 进 Plan 前记当前确认档("confirm"/"auto"/"yolo")——离开 Plan 不重置
    // 用户原有档(单子:批准框选的新档只改本 session,那由审阅框那边落)。
    std::string permission_now;
    switch (lubancode::cli::CurrentConfirmMode()) {
        case lubancode::cli::ConfirmMode::Auto: permission_now = "auto"; break;
        case lubancode::cli::ConfirmMode::Yolo: permission_now = "yolo"; break;
        case lubancode::cli::ConfirmMode::Confirm: permission_now = "confirm"; break;
    }
    host_.session_runtime->SetCollaborationMode(mode, reason, permission_now);
    // 模式段在系统提示末尾,换档即重拼(Default 模板明说旧 Plan 已结束)。
    host_.prompt_options->plan_mode = mode == CollaborationMode::Plan;
    host_.rebuild_preserving();
}

lubancode::app::CommandFlow PlanSessionWiring::HandleCommand(const std::string& args) {
    using lubancode::cli::PlanCommandAction;
    using lubancode::runtime::CollaborationMode;
    const lubancode::cli::ParsedPlanCommand parsed = lubancode::cli::ParsePlanCommand(args);
    const bool in_plan = host_.session_runtime->collaboration_mode() == CollaborationMode::Plan;

    // 命令只在空闲 composer 生效。任务跑着(队列里有待发消息或子代理在跑)
    // 时不半腰切——提示先 Esc 或排下一轮(单子"切换规矩")。
    const bool busy = lubancode::cli::SessionSteeringQueue().HasDeliverable(lubancode::cli::MessageTarget::Main()) ||
                      (host_.agent_tool != nullptr && host_.agent_tool() != nullptr &&
                       host_.agent_tool()->HasRunningTasks());
    if (busy) {
        TermOut() << host_.theme->error << tr("plan.busy") << host_.theme->reset << "\n";
        return CommandFlow::Continue;
    }

    switch (parsed.action) {
        case PlanCommandAction::Invalid:
            TermOut() << host_.theme->error << trf("plan.bad_sub", args) << host_.theme->reset << "\n";
            return CommandFlow::Continue;
        case PlanCommandAction::Status: {
            TermOut() << host_.theme->stats
                      << tr(in_plan ? "plan.status.in_plan" : "plan.status.in_default") << host_.theme->reset
                      << "\n";
            if (const auto* plan = host_.session_runtime->latest_plan()) {
                TermOut() << host_.theme->stats
                          << trf("plan.status.plan_line", plan->plan_id,
                                 static_cast<int>(plan->revision),
                                 lubancode::runtime::ToString(plan->state))
                          << host_.theme->reset << "\n";
            } else {
                TermOut() << host_.theme->stats << tr("plan.status.no_plan") << host_.theme->reset << "\n";
            }
            return CommandFlow::Continue;
        }
        case PlanCommandAction::Off:
            if (!in_plan) {
                TermOut() << host_.theme->stats << tr("plan.not_in") << host_.theme->reset << "\n";
                return CommandFlow::Continue;
            }
            SwitchMode(CollaborationMode::Default, "off");
            review_pending_.reset();
            TermOut() << host_.theme->stats << tr("plan.exited") << host_.theme->reset << "\n";
            return CommandFlow::Continue;
        case PlanCommandAction::Review:
            RunReviewPrompt();
            return CommandFlow::Continue;
        case PlanCommandAction::Enter:
            if (in_plan) {
                TermOut() << host_.theme->stats << tr("plan.already_in") << host_.theme->reset << "\n";
                return CommandFlow::Continue;
            }
            SwitchMode(CollaborationMode::Plan, "slash");
            TermOut() << host_.theme->stats << tr("plan.entered") << host_.theme->reset << "\n";
            return CommandFlow::Continue;
        case PlanCommandAction::EnterWithTask: {
            if (!in_plan) {
                SwitchMode(CollaborationMode::Plan, "slash");
                TermOut() << host_.theme->stats << tr("plan.entered") << host_.theme->reset << "\n";
            }
            // 正文立刻作为规划请求发一轮(带 [Plan 模式规划请求] 前缀,与
            // 普通消息分得开——resume 重放时看得出这轮是规划请求)。
            const std::string task = tr("plan.turn.task_prefix") + parsed.description;
            host_.start_turn(task, nullptr);
            return CommandFlow::Continue;
        }
    }
    return CommandFlow::Continue;
}

std::string PlanSessionWiring::EvaluateGate(const std::string& tool_name, const nlohmann::json& input) {
    using lubancode::runtime::CollaborationMode;
    using lubancode::runtime::PlanToolCapability;
    using lubancode::runtime::PlanToolOrigin;
    if (host_.session_runtime->collaboration_mode() != CollaborationMode::Plan) {
        return std::string();  // Default 一概放行(闸只在 Plan 收紧)
    }
    // 来源/副作用从注册表元数据拿(逐枚追踪单立的账,不靠 RTTI 猜);没账
    // 的注册按 unknown 来源拒(保守为纲)。
    const lubancode::tools::ToolRegistration* registration =
        host_.registry() != nullptr ? host_.registry()->RegistrationOf(tool_name) : nullptr;
    PlanToolCapability capability;
    capability.name = tool_name;
    if (registration != nullptr) {
        switch (registration->source_kind) {
            case lubancode::tools::ToolSourceKind::Builtin: capability.origin = PlanToolOrigin::Builtin; break;
            case lubancode::tools::ToolSourceKind::Mcp: capability.origin = PlanToolOrigin::Mcp; break;
            case lubancode::tools::ToolSourceKind::Lsp: capability.origin = PlanToolOrigin::Lsp; break;
            case lubancode::tools::ToolSourceKind::PluginLua: capability.origin = PlanToolOrigin::PluginLua; break;
            case lubancode::tools::ToolSourceKind::PluginNative: capability.origin = PlanToolOrigin::PluginNative; break;
            case lubancode::tools::ToolSourceKind::Agent: capability.origin = PlanToolOrigin::Agent; break;
            case lubancode::tools::ToolSourceKind::Ptc: capability.origin = PlanToolOrigin::Ptc; break;
            case lubancode::tools::ToolSourceKind::Deferred: capability.origin = PlanToolOrigin::Unknown; break;
        }
        // 写盘级副作用档(effect_class 是逐枚追踪单的账,这里只判"非只读")。
        capability.mutating = registration->effect_class == lubancode::tools::EffectClass::LocalReversible ||
                              registration->effect_class == lubancode::tools::EffectClass::RemoteIrreversible ||
                              registration->effect_class == lubancode::tools::EffectClass::RemoteCompensatable ||
                              registration->effect_class == lubancode::tools::EffectClass::InProcessUnknown;
    } else {
        capability.origin = PlanToolOrigin::Unknown;
        capability.mutating = true;  // 没账的注册按最危险档
    }
    // 宿主声明的 Plan 白名单(read/search/web/ask_user/...):注册表查得到、
    // 名字在表白里的 builtin 才算声明过。MCP readOnlyHint 不算(单子:
    // annotation 只是 hint,不是信任根)。
    capability.plan_safe_by_default =
        capability.origin == PlanToolOrigin::Builtin &&
        lubancode::runtime::IsPlanAllowedBuiltinTool(tool_name) && tool_name != "run_command" &&
        tool_name != "agent";
    // PTC 的 stub 调用走同一 gate(单子:PTC 生成的调用也走 RunOneTool 与
    // ModePolicy);programmatic_tool_calling 本身不在白名单,按 unknown 拒。

    lubancode::runtime::PlanToolInput plan_input;
    if (tool_name == "run_command") {
        const std::string command = input.value("command", std::string());
        const std::string shell = input.value("shell", std::string("powershell"));
        const lubancode::runtime::PlanShellClassification classification =
            lubancode::runtime::ClassifyPlanShellDetailed(command, shell);
        plan_input.shell_safe = classification.verdict == lubancode::runtime::PlanShellVerdict::ReadOnly;
        plan_input.shell_rule = classification.rule;  // 拒绝回执把命中的规则打出来(P2-3)
    }
    if (tool_name == "agent") {
        // P2-3:agent 是派发容器,自身不落盘——注册档的 InProcessUnknown
        // 只说明"内部行为未知",不代表这一派就是写盘;Plan 闸按派出去的
        // 工具面判(Explore / tools.allow 全只读的自定义 Agent 放行),
        // 交给 EvaluateModePolicy 的 agent 分支。真正写盘的子代理在那儿拒。
        capability.mutating = false;
        plan_input.agent_role = input.value("agent_type", std::string("general-purpose"));
        lubancode::tools::AgentTool* agent_tool =
            host_.agent_tool != nullptr ? host_.agent_tool() : nullptr;
        lubancode::tools::ToolRegistry* registry =
            host_.registry != nullptr ? host_.registry() : nullptr;
        if (agent_tool != nullptr && registry != nullptr) {
            plan_input.agent_tools_readonly =
                lubancode::tools::AgentFaceIsReadOnly(agent_tool->custom_agent_resolver(), *registry,
                                                      plan_input.agent_role);
        }
    }
    const lubancode::runtime::ModeVerdict verdict =
        lubancode::runtime::EvaluateModePolicy(CollaborationMode::Plan, capability, plan_input);
    if (verdict.allowed) {
        return std::string();
    }
    // RunOneTool 约定:"code|reason" 两截。
    return verdict.code + "|" + verdict.reason;
}

void PlanSessionWiring::CollectProposal(std::size_t history_before, const std::string& turn_id) {
    using lubancode::runtime::CollaborationMode;
    if (host_.session_runtime->collaboration_mode() != CollaborationMode::Plan) {
        return;  // 只有 Plan 模式认 <proposed_plan>(单子:stream parser 只在 Plan mode 识别)
    }
    // 本轮新增的 assistant 正文:倒着找最后一条 assistant 消息,拼全部文本块。
    const auto& history = host_.main_agent()->History();
    std::string text;
    for (std::size_t i = history.size(); i > history_before;) {
        --i;
        if (history[i].role != lubancode::api::Role::Assistant) {
            continue;
        }
        for (const auto& block : history[i].content) {
            if (const auto* tb = std::get_if<lubancode::api::TextBlock>(&block)) {
                if (!text.empty()) {
                    text += "\n";
                }
                text += tb->text;
            }
        }
        break;  // 只看本轮最后一条 assistant 消息(一轮至多一份计划)
    }
    if (text.empty()) {
        return;
    }
    const lubancode::runtime::ProposedPlanScan scan = lubancode::runtime::ScanProposedPlan(text);
    if (scan.ambiguous) {
        TermOut() << host_.theme->error << tr("plan.ambiguous") << host_.theme->reset << "\n";
        return;
    }
    if (scan.truncated) {
        TermOut() << host_.theme->error << tr("plan.truncated") << host_.theme->reset << "\n";
        return;
    }
    if (!scan.found) {
        return;  // 没交计划,正常答疑,不打扰
    }
    // 建 PlanDocument:同 plan_id 的新稿 revision+1(替换稿),换了任务另起
    // plan_id。这里按"最近稿是否出自同一 turn 群"简单分:上一稿还在
    // Presented/Rejected 就当修订(revision+1),否则新 plan。
    lubancode::runtime::PlanDocument plan;
    const lubancode::runtime::PlanDocument* previous = host_.session_runtime->latest_plan();
    if (previous != nullptr &&
        (previous->state == lubancode::runtime::PlanReviewState::Presented ||
         previous->state == lubancode::runtime::PlanReviewState::Rejected)) {
        plan.plan_id = previous->plan_id;
        plan.revision = previous->revision + 1;
    } else {
        plan.plan_id = "plan-" + std::to_string(++plan_counter_);
        plan.revision = 1;
    }
    plan.markdown = scan.markdown;
    plan.source_turn_id = turn_id;
    plan.state = lubancode::runtime::PlanReviewState::Presented;
    plan.content_sha256 = lubancode::hooks::Sha256Hex(plan.markdown);
    // 超限:正文落 artifact(item 留引用)。仓走 Offload(幂等,tool 名记
    // "plan");仓没开给 nullopt,序列化层退内联分支。
    if (plan.markdown.size() > lubancode::runtime::kPlanMarkdownInlineCap && host_.artifact_store != nullptr &&
        host_.artifact_store->active()) {
        if (auto ref = host_.artifact_store->Offload(plan.plan_id + "-r" + std::to_string(plan.revision), "plan",
                                                     plan.markdown, /*source_message_index=*/0);
            ref.has_value()) {
            plan.artifact_ref = ref->artifact_id;
        }
    }
    host_.session_runtime->RecordPlanDocument(plan);
    review_pending_ = plan;
    TermOut() << host_.theme->stats
              << trf("plan.recorded", plan.plan_id, static_cast<int>(plan.revision), plan.markdown.size())
              << host_.theme->reset << "\n";
    RunReviewPrompt();
}

void PlanSessionWiring::RunReviewPrompt() {
    using lubancode::runtime::CollaborationMode;
    if (host_.session_runtime->collaboration_mode() != CollaborationMode::Plan) {
        TermOut() << host_.theme->error << tr("plan.review.no_plan") << host_.theme->reset << "\n";
        return;
    }
    if (!review_pending_.has_value()) {
        TermOut() << host_.theme->error << tr("plan.review.no_plan") << host_.theme->reset << "\n";
        return;
    }
    lubancode::runtime::PlanDocument plan = *review_pending_;
    // 审批对象带 id+revision+hash(单子:"用户审的是哪一稿,账上写清");
    // 若最新稿已被顶替,这枚 pending 就 stale,不弹。
    const lubancode::runtime::PlanDocument* latest = host_.session_runtime->latest_plan();
    if (latest == nullptr || latest->plan_id != plan.plan_id || latest->revision != plan.revision ||
        latest->content_sha256 != plan.content_sha256) {
        TermOut() << host_.theme->error << tr("plan.review.stale") << host_.theme->reset << "\n";
        review_pending_.reset();
        return;
    }
    // 非真终端(管道/单发)不弹无人能答的框(单子:one-shot 产出计划后退出)。
    if (!lubancode::platform::StdinIsInteractive() || !lubancode::platform::ProbeStdoutConsole().is_console) {
        return;
    }
    {
        const lubancode::cli::StreamFooterSuspendScope footer_suspend;
        TermOut() << "\n"
                  << host_.theme->banner
                  << trf("plan.review.title", plan.plan_id, static_cast<int>(plan.revision),
                         plan.content_sha256.substr(0, 12))
                  << host_.theme->reset << "\n";
        // 计划正文直接铺(终端审阅就是读正文;编辑器改稿是第 6 期)。
        TermOut() << plan.markdown << "\n\n";
        std::vector<lubancode::cli::ChoiceMenuItem> items = {
            {tr("plan.review.opt.approve_confirm"), {}},
            {tr("plan.review.opt.approve_auto"), {}},
            {tr("plan.review.opt.stay"), {}},
            {tr("plan.review.opt.exit"), {}},
        };
        lubancode::cli::ChoiceMenuOptions opts;
        opts.hint = tr("plan.review.hint");
        opts.initial_cursor = 2;  // 默认"留在 Plan"(安全,回车不误批准)
        const auto selected = lubancode::cli::ReadChoiceMenu(items, opts, *host_.theme);
        if (!selected.has_value() || selected->selected_indices.empty()) {
            TermOut() << host_.theme->stats << tr("plan.review.cancelled") << host_.theme->reset << "\n";
            return;  // ESC 只关框,仍留 Plan;/plan review 再开
        }
        switch (selected->selected_indices.front()) {
            case 0:
                LaunchApprovedExecution(plan, /*auto_mode=*/false);
                return;
            case 1:
                LaunchApprovedExecution(plan, /*auto_mode=*/true);
                return;
            case 2: {
                // 继续规划不换 mode;落一条 continued 审批账(不匹配 stale
                // 判定,只作人话留痕)。恢复侧见 ReviewPlan 的拒绝分支。
                TermOut() << host_.theme->stats << tr("plan.review.stayed") << host_.theme->reset << "\n";
                return;
            }
            default:
                SwitchMode(CollaborationMode::Default, "off");
                review_pending_.reset();
                TermOut() << host_.theme->stats << tr("plan.review.exited") << host_.theme->reset << "\n";
                return;
        }
    }
}

void PlanSessionWiring::LaunchApprovedExecution(lubancode::runtime::PlanDocument plan, bool auto_mode) {
    using lubancode::runtime::CollaborationMode;
    // 单子"执行交接"次序:
    //   1. append + flush plan_review approved(ReviewPlan 里做,含 id/
    //      revision/hash 三对——不匹配给 stale,不落账、不执行)。
    const auto outcome = host_.session_runtime->ReviewPlan(plan.plan_id, plan.revision, plan.content_sha256,
                                                           /*approve=*/true);
    if (outcome == lubancode::runtime::SessionRuntime::PlanReviewOutcome::Stale) {
        TermOut() << host_.theme->error << tr("plan.review.stale") << host_.theme->reset << "\n";
        return;
    }
    //   2. 切 CollaborationMode=Default(mode 事件先于 synthetic turn 落盘,
    //      崩在这之后 resume 认得出"已批准待执行")。
    SwitchMode(CollaborationMode::Default, "approved");
    //   3. 批准框选的执行档只改本 session(Confirm/Auto;Yolo 不出现在框里
    //      ——单子:Yolo 只在本场原本已显式启用且高风险提示时才可选,首版
    //      不做那条路)。
    lubancode::cli::SetConfirmMode(auto_mode ? lubancode::cli::ConfirmMode::Auto
                                             : lubancode::cli::ConfirmMode::Confirm);
    //   4-5. ImplementationBrief + synthetic user turn:同一轮把 brief 与
    //      计划正文都喂给执行模型(不只剩"按上面的计划做"——compact 后
    //      "上面"可能早没了,单子明令)。
    review_pending_.reset();
    TermOut() << host_.theme->stats << trf("plan.review.approved", static_cast<int>(plan.revision))
              << host_.theme->reset << "\n";
    const std::string brief = trf("plan.turn.handoff", plan.plan_id, static_cast<int>(plan.revision)) + plan.markdown;
    host_.start_turn(brief, nullptr);
    //   6. 执行模型先用 todo_write 拆施工清单——brief 文案里已带这句
    //      (plan.turn.handoff),不在这里另塞指令。
}

}  // namespace lubancode::app
