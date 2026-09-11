// /goal 子系统接线器的实现(会话终章):函数体原样自 interactive_session
// 大类搬来(RestoreGoalFromArchive/AttachGoalSnapshotToCompact/
// PumpGoalContinuation/CloseGoalIteration/MakeGoalWiring),材料换经 Host
// 递入,行为一字未改——注释一并随行。
//
// 骨架拆解反弹·问题 3:Ensure 里"事件类型分族 + ledger sink 搭建"抽去
// runtime::goal::MakeSessionLedgerSink(纯函数,单测钉);终端打印改产
// notify 回调(装配层决定怎么画),本文件不再有 TermOut。
#include "app/wirings/goal_session_wiring.hpp"

#include <chrono>
#include <filesystem>
#include <utility>

#include "platform/paths.hpp"
#include "runtime/goal_compact.hpp"
#include "runtime/goal_context.hpp"
#include "runtime/goal_evaluation_flow.hpp"
#include "runtime/goal_evidence.hpp"
#include "runtime/goal_evaluator.hpp"
#include "runtime/tool_trace_hub.hpp"
#include "runtime/trajectory_session.hpp"
#include "tools/agent_tool.hpp"
#include "tools/registry.hpp"

namespace lubancode::app {

namespace goal = lubancode::runtime::goal;

GoalSessionWiring::GoalSessionWiring(Host host) : host_(std::move(host)) {}

// 渲染事件出口(问题 3 第 2 条):is_error 定色,text 是纯文案——怎么画
// 由装配层(interactive_session_assembly 填的 notify)决定。
void GoalSessionWiring::Notify(bool is_error, const std::string& text) {
    if (host_.notify) {
        host_.notify(is_error, text);
    }
}

lubancode::runtime::goal::GoalCoordinator* GoalSessionWiring::coordinator() {
    return coordinator_.has_value() ? &*coordinator_ : nullptr;
}

bool GoalSessionWiring::ToolExposed() const {
    // 会话级条件(features.goals 正门 + env 总闸),与 GoalOptionsFromConfig
    // 给 coordinator 的 goals_enabled 同一条判式——暴露位与"真跑不跑"同源,
    // 不会出现"看得见工具却永远等不到轮次"之外的第三种状态。config 在
    // 会话启动定死,此值会话内恒定(动态工具 P2 的 ToolExposurePolicy)。
    return host_.config != nullptr && host_.config->features_goals && !lubancode::app::GoalsDisabledByEnv();
}

std::string GoalSessionWiring::ActiveGoalId() const {
    // checkpoint 工具账里的 goal_id 就是"当前在跑哪只 goal"的真值:Pump
    // 开轮前灌(goal iteration 的 id),收口后账面留着、下一次 Pump 重灌。
    // 能力段只在 HasActiveIteration 为真时才带这条注,收口后的旧值不会被
    // 念出来——判"在不在 goal 轮"永远以 HasActiveIteration 为准。
    return checkpoint_state_ != nullptr ? checkpoint_state_->goal_id : std::string();
}

void GoalSessionWiring::RegisterTools(lubancode::tools::ToolRegistry& registry) {
    if (!checkpoint_state_) {
        checkpoint_state_ = std::make_shared<lubancode::tools::GoalCheckpointState>();
    }
    registry.Register(std::make_unique<lubancode::tools::GoalCheckpointTool>(checkpoint_state_));
}

void GoalSessionWiring::Ensure(const lubancode::config::Config& config) {
    if (!coordinator_.has_value()) {
        auto options = lubancode::app::GoalOptionsFromConfig(config.features_goals, config.goals);
        coordinator_.emplace(std::move(options));
        // P0-6:旧存档的 LedgerSink 已删;v3 起 goal 持久账走 GoalService 的
        // state.goal.applied + 不可变快照(§4.67 G0 合同已定型,runtime/
        // goal_service.hpp;G1 已接命令面与本泵)。coordinator(v1 运行面)
        // 不接 sink = 事件只进内存,只在 v2 场干活。
        // loop 单分流合流:coordinator 的 ready continuation 经 GoalWorkSource
        // 进泵(泵问 ProbeWork;选中后装配层 TakeReadyIteration 发 synthetic
        // turn)。trigger 各归各(evaluator 判终点 vs 时钟到点),泵共用。
        // G1:v3 场的待续工作项也走同一探针(认领判据 EvaluateGoalWork),
        // workItemId 即去重身份——恢复补队列与泵取件同一枚 id。
        work_source_.SetProbe([this]() -> std::optional<lubancode::runtime::SessionWork> {
            if (ToolExposed() && goal_service_.has_value() && !goal_service_->broken()) {
                const goal::GoalStateSnapshot* snapshot = goal_service_->current();
                if (snapshot != nullptr) {
                    const goal::GoalWorkView view =
                        goal::EvaluateGoalWork(*snapshot, V3WriterEpoch());
                    if (view.claimable) {
                        lubancode::runtime::SessionWork work;
                        work.kind = lubancode::runtime::WorkKind::GoalContinuation;
                        work.id = view.intent.work_item_id;
                        work.payload["goal_id"] = snapshot->goal_id;
                        work.payload["work_item_id"] = view.intent.work_item_id;
                        return work;
                    }
                    return std::nullopt;  // v3 有 goal 在账:v1 路不再掺和(不双跑)
                }
            }
            if (!coordinator_.has_value() || !coordinator_->HasReadyContinuation()) {
                return std::nullopt;
            }
            lubancode::runtime::SessionWork work;
            work.kind = lubancode::runtime::WorkKind::GoalContinuation;
            work.id = coordinator_->ready_dedupe_key();
            work.payload["goal_id"] = coordinator_->task() != nullptr
                                          ? coordinator_->task()->id
                                          : std::string();
            return work;
        });
    }
    // 换场感知(G1):clear/resume 换卷后这里把 v3 服务重绑到新写者
    //(幂等;同卷不动)。
    BindGoalService();
}

// ---- v3 goal 服务接线(轨迹 v3 §4.67 G1) ------------------------------------

std::string GoalSessionWiring::V3WriterEpoch() const {
    if (host_.trajectory == nullptr) return std::string();
    lubancode::runtime::TrajectorySessionLedger* ledger = host_.trajectory;
    lubancode::trajectory::v3::V3Writer* writer = ledger->v3_main_writer();
    return writer != nullptr ? writer->run_id() : std::string();
}

void GoalSessionWiring::BindGoalService() {
    if (host_.trajectory == nullptr) return;  // 没接账:goal 走 v1 旧路
    lubancode::runtime::TrajectorySessionLedger* ledger = host_.trajectory;
    lubancode::trajectory::v3::V3Writer* writer = ledger->v3_main_writer();
    if (writer == nullptr) {
        // v2 卷:在场服务撤掉(保干净;v2 场命令照旧走 coordinator)。
        goal_service_.reset();
        goal_service_bound_session_.clear();
        goal_v3_restored_ = false;
        v3_evidence_memory_.clear();
        return;
    }
    if (goal_service_.has_value() && goal_service_bound_session_ == writer->session_id()) {
        return;  // 已在本卷安家:内存 current_ 是写面真值,不重建
    }
    goal::GoalService::Options options;
    options.session_dir = ledger->session_dir();
    goal_service_.emplace(writer, std::move(options));
    goal_service_bound_session_ = writer->session_id();
    goal_v3_restored_ = false;
}

lubancode::runtime::goal::GoalService* GoalSessionWiring::goal_service() {
    return goal_service_.has_value() ? &*goal_service_ : nullptr;
}

std::optional<lubancode::runtime::goal::GoalLineageProjection>
GoalSessionWiring::ProjectGoalForCommands() {
    if (host_.trajectory == nullptr) return std::nullopt;
    return goal::ProjectGoalLineage(host_.trajectory->session_dir());
}

void GoalSessionWiring::HandleSessionCleared() {
    // /clear 开的新场不带旧 goal(§4.67.2:clear 才撤 goal;链走只穿 resume
    // 边)。内存接管态清空,下一次 Ensure/Restore 在新卷上重判。
    goal_service_.reset();
    goal_service_bound_session_.clear();
    goal_v3_restored_ = false;
    v3_evidence_memory_.clear();
}

void GoalSessionWiring::RestoreFromArchive() {
    // P0-6:旧存档 goal 事件账的回放路已删(store 恒不 active,P0-2 起此
    // 路恒早退);v2 场保持幂等空位。v3 场(§4.67 G1):沿 resume 来源链
    // 投影 goal head → AdoptFromProjection 接管 → pendingIntent 按认领面
    // 决定补不补队列。换场(resume)后 Ensure 已重建服务,这里做接管。
    Ensure(*host_.config);
    if (goal_service_.has_value() && goal_v3_restored_) return;
    if (host_.trajectory == nullptr) return;
    if (!goal_service_.has_value() || goal_service_->broken()) return;
    const auto lineage = goal::ProjectGoalLineage(host_.trajectory->session_dir());
    if (!lineage.found) {
        goal_v3_restored_ = true;
        return;  // 链上没有 goal 账:安静(与 v2 空位同款)
    }
    if (lineage.projection.gap != goal::GoalProjectionGap::None) {
        // 缺口如实报(§4.55 状态损坏):不接管、不猜;自动续排自然停
        //(服务无 goal,泵不认领)。
        goal_v3_restored_ = true;
        Notify(/*is_error=*/true,
               "goal 状态缺口[" + goal::ToString(lineage.projection.gap) + "]: " +
                   lineage.projection.gap_detail + " — 不接管,查档或 /goal clear 重立。");
        return;
    }
    const auto adopted = goal_service_->AdoptFromProjection(lineage.projection);
    goal_v3_restored_ = true;
    if (!adopted.ok) {
        Notify(/*is_error=*/true,
               "goal 恢复接管失败(" + adopted.error_code + "): " + adopted.error_message);
        return;
    }
    const goal::GoalStateSnapshot* snapshot = goal_service_->current();
    if (snapshot == nullptr) return;
    // pendingIntent 恰好消费一次(§4.67.4/§4.67.8):恢复只按认领面把工作项
    // 摆回探针——claim 落账才是真消费;已认领(本 run 未开轮)沿用原项,
    // 他 run 已认领不重放。重复 resume 看到的是同一份持久状态,不重复提交。
    const goal::GoalWorkView view = goal::EvaluateGoalWork(*snapshot, V3WriterEpoch());
    std::string line = "goal 已恢复: " + snapshot->goal_id + "(r" +
                       std::to_string(snapshot->state_revision) + " ," +
                       goal::ToString(snapshot->lifecycle) + ")";
    if (view.has_intent) {
        if (view.claimable) {
            line += ";待续工作项 " + view.intent.work_item_id + " 按原 id 回泵";
        } else if (view.claimed_by_other) {
            line += ";工作项 " + view.intent.work_item_id + " 已被写者 " +
                    view.intent.writer_epoch + " 认领,恢复核验待下一批,不重放";
        } else {
            line += ";" + view.reason;
        }
    }
    Notify(/*is_error=*/false, line);
}

lubancode::app::GoalWiring GoalSessionWiring::MakeCommandWiring(
    lubancode::tools::AgentTool* agent_tool, lubancode::runtime::loop::LoopScheduler* loop_scheduler) {
    lubancode::app::GoalWiring wiring;
    wiring.theme = host_.theme;
    wiring.coordinator = coordinator_.has_value() ? &*coordinator_ : nullptr;
    wiring.agent_tool = agent_tool;
    wiring.checkpoint_state = checkpoint_state_.get();
    wiring.loop_scheduler = loop_scheduler;
    // v3(G1):服务 + 预算配置 + feature 正门 + 单一读面投影口。v2 场
    // goal_service 空,命令照旧走 coordinator。
    wiring.goal_service = goal_service();
    wiring.goals_config = host_.config != nullptr ? &host_.config->goals : nullptr;
    wiring.goals_enabled = ToolExposed();
    wiring.project_goal = [this]() {
        return goal::ProjectGoalLineage(host_.trajectory != nullptr
                                            ? host_.trajectory->session_dir()
                                            : std::filesystem::path{});
    };
    return wiring;
}

// (P0-6:AttachSnapshotToCompact——compact_v2 事件的 goal 快照附带——
// 已删;compact 的持久账是 trajectory 的 compact.applied,goal 守恒快照
// 的接续属 goal 单后续波次。)

void GoalSessionWiring::NoteSubagentCompletion() {
    lubancode::app::NoteSubagentCompletionForGoal(
        MakeCommandWiring(host_.agent_tool ? host_.agent_tool() : nullptr,
                          host_.loop_scheduler ? host_.loop_scheduler() : nullptr));
}

// v3 泵路(轨迹 v3 §4.67 G1/G2):认领 → 开轮 → synthetic turn → 收口
// (采证/验收/判词采用/续排意图)。单飞与 v1 同泵(一场 session 同时一枚
// 主 turn);每拍只认领一枚工作项,收口后按判词走——没有可采判词
// (evaluator_failed)不续排下一轮(§4.67.5)。
bool GoalSessionWiring::PumpV3Continuation(std::int64_t now_ms) {
    if (!ToolExposed()) {
        return false;  // features.goals 正门关着:不排轮(v1 同源判式)
    }
    if (!goal_service_.has_value() || goal_service_->broken()) {
        return false;
    }
    const goal::GoalStateSnapshot* snapshot = goal_service_->current();
    if (snapshot == nullptr) {
        return false;
    }
    const std::string epoch = V3WriterEpoch();
    const goal::GoalWorkView view = goal::EvaluateGoalWork(*snapshot, epoch);
    if (!view.claimable) {
        return false;
    }
    const nlohmann::json cause = nlohmann::json{{"source", "host"},
                                                {"workItemId", view.intent.work_item_id}};
    // 1) 认领(§4.67.4:取走工作项先提交 claimed + writerEpoch 再调模型)。
    auto claim = goal_service_->ClaimPendingIntent(epoch, snapshot->state_revision, cause);
    if (!claim.ok) {
        // 已被他写者认领/冲突:这一拍消费掉,别让泵空转打转;下一圈探针
        // 自会按持久状态停手。
        Notify(/*is_error=*/true,
               "goal 工作项认领失败(" + claim.error_code + "): " + claim.error_message);
        return true;
    }
    // 2) 开轮(iterationId 落快照,§4.67.4 主工作轮绑定 iteration)。
    auto began = goal_service_->BeginIteration(claim.payload.value("stateRevision", 0), cause);
    if (!began.ok) {
        Notify(/*is_error=*/true,
               "goal 开轮失败(" + began.error_code + "): " + began.error_message);
        return true;
    }
    const std::string goal_id = began.payload.value("goalId", std::string());
    const std::string iteration_id = began.payload.value("iterationId", std::string());
    const int iteration_index = began.payload.value("iterationIndex", 0);
    const std::string revision_note = "c" + std::to_string(claim.payload.value("contractRevision", 1));
    active_iteration_ = iteration_id;
    fairness_.NoteGoalRan();
    // goal_checkpoint 工具的会话级状态:本轮 scope 灌好(空 goal_id = 工具
    // 明拒),上一轮的 entries 清零(候选只算本轮的)。
    if (checkpoint_state_ != nullptr) {
        checkpoint_state_->goal_id = goal_id;
        checkpoint_state_->iteration_id = iteration_id;
        checkpoint_state_->entries.clear();
    }
    Notify(/*is_error=*/false,
           "[goal " + goal_id + " " + revision_note + " iteration " +
               std::to_string(iteration_index) + "]");
    lubancode::app::EmitGoalHook(
        MakeCommandWiring(host_.agent_tool ? host_.agent_tool() : nullptr,
                          host_.loop_scheduler ? host_.loop_scheduler() : nullptr),
        lubancode::hooks::HookEvent::GoalIterationStart,
        nlohmann::json{{"goal_id", goal_id},
                       {"iteration_id", iteration_id},
                       {"iteration_index", iteration_index},
                       {"work_item_id", view.intent.work_item_id}},
        /*match_value=*/std::string());
    // 3) synthetic turn(文字供模型看;正文自带目标,收口前 goal_checkpoint
    // 照 v1 口径可用)。
    bool turn_failed = false;
    if (host_.start_turn) {
        const goal::GoalStateSnapshot* running = goal_service_->current();
        host_.start_turn("[goal " + goal_id + " " + revision_note + " iteration " +
                             std::to_string(iteration_index) + "]\n目标:" +
                             (running != nullptr ? running->objective : snapshot->objective) +
                             "\n请推进当前目标;收口前调用 goal_checkpoint 写检查点。"
                             "\n完成后写清剩余工作,宿主将独立验收并决定续跑。",
                         &turn_failed);
    }
    // 4) 收口(G2:验收走 v3 内部请求服务):采证 -> checkpoint ->
    // 排评估 -> 采判词 -> 程序门槛 -> 采用与续排意图同一快照提交。
    // 逻辑全在 runtime 的 CloseGoalIterationWithEvaluation(单测钉),
    // 这里只折材料与转发通知。
    const goal::GoalStateSnapshot* closing = goal_service_->current();
    if (closing != nullptr && !turn_failed && host_.evaluation_backend != nullptr &&
        host_.trajectory != nullptr &&
        host_.trajectory->v3_main_writer() != nullptr) {
        goal::GoalCloseoutMaterial material;
        material.parent_turn_id = host_.last_turn_id ? host_.last_turn_id() : std::string();
        material.now_ms = now_ms;
        material.workspace_summary = "cwd: " + lubancode::platform::CurrentDirUtf8();
        // checkpoint:工具调过取之,否则宿主合成(标 synthesized,不能因此
        // 漏验,也不能把自报完成当证据)。
        bool has_tool_checkpoint = false;
        if (checkpoint_state_ != nullptr && checkpoint_state_->HasCheckpoint() &&
            checkpoint_state_->goal_id == goal_id) {
            const auto candidate = checkpoint_state_->Candidate();
            if (candidate.has_value()) {
                has_tool_checkpoint = true;
                material.checkpoint.summary = candidate->summary;
                material.checkpoint.completed = candidate->completed;
                material.checkpoint.remaining = candidate->remaining;
                material.checkpoint.next_action = candidate->next_action;
                material.checkpoint.evidence_ids = candidate->evidence_ids;
                material.checkpoint.blocker_key = candidate->blocker_key;
                material.checkpoint.question = candidate->question;
            }
        }
        if (!has_tool_checkpoint) {
            material.checkpoint.synthesized = true;
            material.checkpoint.summary = "执行轮收口时未调用 goal_checkpoint;宿主合成 missing checkpoint。";
            material.checkpoint.next_action = "重读目标与合同 criteria,补一枚明确 checkpoint 再收口。";
        }
        // 采证:本轮 finished 工具事件翻证据(v1 形状,内存留判材料;引用
        // 入快照归 flow)。写盘级工具落成后,旧验证证据翻 stale(§4.67.5)。
        if (host_.trace_hub != nullptr && !material.parent_turn_id.empty()) {
            goal::GoalEvidenceContext ctx;
            ctx.goal_id = goal_id;
            ctx.iteration_id = iteration_id;
            ctx.turn_id = material.parent_turn_id;
            // 发号续快照在账数(同 goal 内单调;resume 后内存空、快照计数
            // 衔接,不与旧 id 撞)。
            int evidence_seq = static_cast<int>(closing->evidence_refs.size());
            bool write_landed = false;
            for (const auto& event :
                 host_.trace_hub->FinishedEventsOfTurn(material.parent_turn_id)) {
                const auto evidence = goal::EvidenceFromToolTrace(
                    event, ctx, "ev-" + std::to_string(++evidence_seq));
                if (!evidence.has_value()) continue;
                if (goal::EvidenceStalesOnWrite(evidence->kind)) write_landed = true;
                material.fresh_evidence.push_back(*evidence);
            }
            if (write_landed) {
                for (auto& [id, ev] : v3_evidence_memory_) {
                    if (goal::EvidenceStalesOnWrite(ev.kind) && ev.fresh) {
                        ev.fresh = false;
                        material.evidence_stale_ids.push_back(id);
                    }
                }
            }
            for (const auto& ev : material.fresh_evidence) {
                v3_evidence_memory_[ev.id] = ev;
            }
            material.material_evidence.reserve(v3_evidence_memory_.size());
            for (const auto& [id, ev] : v3_evidence_memory_) {
                (void)id;
                material.material_evidence.push_back(ev);
            }
        } else {
            // 拿不到工作轮 turnId(旧装配没填 last_turn_id):只剩内存旧证
            // 据可判;材料缺口让验收更保守,不放缺口。
            material.material_evidence.reserve(v3_evidence_memory_.size());
            for (const auto& [id, ev] : v3_evidence_memory_) {
                (void)id;
                material.material_evidence.push_back(ev);
            }
        }
        // 评估模型路由(§4.67.5:沿模型路由选独立小模型,记录实际
        // provider/model/wire)。
        goal::GoalEvaluationFlowOptions flow_options;
        if (host_.current_model != nullptr) flow_options.model = *host_.current_model;
        if (host_.model_router != nullptr) {
            const auto routed_info =
                host_.model_router->RouteInfo(lubancode::agent::TaskKind::GoalEvaluate);
            if (!routed_info.model.empty()) {
                flow_options.model = routed_info.model;
                flow_options.reasoning_effort = routed_info.effort;
            }
        }
        flow_options.provider = host_.evaluation_provider;
        flow_options.wire = host_.evaluation_wire;
        const auto closed_eval = goal::CloseGoalIterationWithEvaluation(
            *goal_service_, *host_.trajectory->v3_main_writer(), *host_.evaluation_backend,
            flow_options, material);
        if (closed_eval.decision == "evaluator_failed") {
            Notify(/*is_error=*/true,
                   "goal evaluator 失败: " + closed_eval.summary + ";目标转暂停(/goal resume 续)。");
        } else if (!closed_eval.ok) {
            Notify(/*is_error=*/true, "goal 验收收口失败(" + closed_eval.error_code + "): " +
                                          closed_eval.error_message);
        } else {
            Notify(/*is_error=*/false,
                   "[goal 判词: " + closed_eval.decision + "] " + closed_eval.summary);
        }
        if (closed_eval.overridden_achieved) {
            Notify(/*is_error=*/false,
                   "  (evaluator 判 achieved 被程序门槛改判 continue: " +
                       closed_eval.override_reason + ")");
        }
        lubancode::app::EmitGoalHook(
            MakeCommandWiring(host_.agent_tool ? host_.agent_tool() : nullptr,
                              host_.loop_scheduler ? host_.loop_scheduler() : nullptr),
            lubancode::hooks::HookEvent::GoalEvaluated,
            nlohmann::json{{"goal_id", goal_id},
                           {"iteration_id", iteration_id},
                           {"summary", closed_eval.summary.substr(0, 600)},
                           {"decision", closed_eval.decision}},
            /*match_value=*/closed_eval.decision);
        const goal::GoalStateSnapshot* after = goal_service_->current();
        if (after != nullptr && goal::IsLifecycleTerminal(after->lifecycle)) {
            // terminal 事件已随快照提交落;GoalCompleted 在其后跑(通知失败
            // 不撤回已提交终态)。
            lubancode::app::EmitGoalHook(
                MakeCommandWiring(host_.agent_tool ? host_.agent_tool() : nullptr,
                                  host_.loop_scheduler ? host_.loop_scheduler() : nullptr),
                lubancode::hooks::HookEvent::GoalCompleted,
                nlohmann::json{{"goal_id", after->goal_id},
                               {"decision", closed_eval.decision},
                               {"iterations", after->counters.iterations_started}},
                /*match_value=*/goal::ToString(after->lifecycle));
        }
    } else if (closing != nullptr) {
        // 请求都没成或评估口没接:evaluator 没材料可判——照旧收口销账,
        // 不烧评估这一趟(连败记 provider 账,下一圈泵再问)。
        auto ended = goal_service_->EndIteration(closing->state_revision, cause);
        if (!ended.ok) {
            Notify(/*is_error=*/true,
                   "goal 收工落账失败(" + ended.error_code + "): " + ended.error_message);
        }
    }
    if (checkpoint_state_ != nullptr) {
        checkpoint_state_->entries.clear();
    }
    active_iteration_.clear();
    fairness_.NoteOtherWorkRan();
    lubancode::app::EmitGoalHook(
        MakeCommandWiring(host_.agent_tool ? host_.agent_tool() : nullptr,
                          host_.loop_scheduler ? host_.loop_scheduler() : nullptr),
        lubancode::hooks::HookEvent::GoalIterationEnd,
        nlohmann::json{{"goal_id", goal_id},
                       {"iteration_id", iteration_id},
                       {"iteration_index", iteration_index},
                       {"turn_failed", turn_failed}},
        /*match_value=*/std::string());
    (void)now_ms;
    return true;
}

void GoalSessionWiring::PumpContinuation(std::int64_t now_ms) {
    // G1:v3 场先走认领/开轮路;吃掉这一拍后 v1 路不再动(不双跑)。
    if (PumpV3Continuation(now_ms)) {
        return;
    }
    // goal 的 ready continuation 开一轮 synthetic turn(单飞:与 loop 同泵,
    // 一场 session 同时只跑一枚主 turn)。TakeReadyIteration 落 started
    // 事件(带 turn_id/dedupe_key);失败(goal 单测过)静默返回,下一圈
    // 再问。
    if (!coordinator_.has_value() ||
        !coordinator_->HasReadyContinuation()) {
        return;
    }
    const auto started = coordinator_->TakeReadyIteration("goal-turn", /*before_fingerprint=*/"", now_ms);
    if (!started.ok) {
        return;
    }
    active_iteration_ = started.dedupe_key;
    fairness_.NoteGoalRan();
    // goal_checkpoint 工具的会话级状态:本轮 scope 灌好(空 goal_id = 工具
    // 明拒),上一轮的 entries 清零(候选只算本轮的)。
    if (checkpoint_state_ != nullptr) {
        checkpoint_state_->goal_id = started.iteration.goal_id;
        checkpoint_state_->iteration_id = started.iteration.id;
        checkpoint_state_->entries.clear();
    }
    Notify(/*is_error=*/false, "[goal " + started.iteration.goal_id + " iteration " +
                                   std::to_string(started.iteration.index) + "]");
    lubancode::app::EmitGoalHook(
        MakeCommandWiring(host_.agent_tool ? host_.agent_tool() : nullptr,
                          host_.loop_scheduler ? host_.loop_scheduler() : nullptr),
        lubancode::hooks::HookEvent::GoalIterationStart,
        nlohmann::json{{"goal_id", started.iteration.goal_id},
                       {"iteration_id", started.iteration.id},
                       {"iteration_index", started.iteration.index},
                       {"dedupe_key", started.dedupe_key}},
        /*match_value=*/std::string());
    bool turn_failed = false;
    host_.start_turn(started.synthetic_text, &turn_failed);
    // 收口:completion-driven 泵的真接线——采证/checkpoint/evaluator/
    // ApplyEvaluation/ScheduleNextIteration 都在主线程安全边界跑。
    CloseIteration("goal-turn", turn_failed);
    active_iteration_.clear();
    fairness_.NoteOtherWorkRan();
    lubancode::app::EmitGoalHook(
        MakeCommandWiring(host_.agent_tool ? host_.agent_tool() : nullptr,
                          host_.loop_scheduler ? host_.loop_scheduler() : nullptr),
        lubancode::hooks::HookEvent::GoalIterationEnd,
        nlohmann::json{{"goal_id", started.iteration.goal_id},
                       {"iteration_id", started.iteration.id},
                       {"iteration_index", started.iteration.index},
                       {"turn_failed", turn_failed}},
        /*match_value=*/std::string());
}

void GoalSessionWiring::CloseIteration(const std::string& turn_id, bool turn_failed) {
    if (!coordinator_.has_value() || active_iteration_.empty()) {
        return;  // 不在 goal 收口位(用户普通轮/迟到)
    }
    const auto now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }();
    const auto* task = coordinator_->task();
    if (task == nullptr || lubancode::runtime::goal::IsGoalTerminal(task->state)) {
        return;  // 已收账(收口前用户 clear 了):只留审计
    }

    // ---- 1) 采证:本轮 finished 的工具事件翻 GoalEvidence 喂账 ----
    if (host_.trace_hub != nullptr) {
        namespace goalns = lubancode::runtime::goal;
        goalns::GoalEvidenceContext ctx;
        ctx.goal_id = task->id;
        ctx.iteration_id = task->id + "/iter-" + std::to_string(task->counters.iterations_started);
        ctx.turn_id = turn_id;
        int evidence_seq = static_cast<int>(coordinator_->evidence_count());
        std::vector<std::string> fresh_ids;
        for (const auto& event : host_.trace_hub->FinishedEventsOfTurn(turn_id)) {
            const auto evidence =
                goalns::EvidenceFromToolTrace(event, ctx, "ev-" + std::to_string(++evidence_seq));
            if (!evidence.has_value()) {
                continue;
            }
            // P0-6:旧存档的 goal_evidence_v1 行已删;证据账进 coordinator
            //(进程内)。持久证据账走 v3 goal 快照的 evidenceRefs
            //(§4.67 G0 GoalService;采证接线归 G2)。
            coordinator_->RecordEvidence(*evidence);
            fresh_ids.push_back(evidence->id);
            // 写盘级工具落完:旧验证证据按分档翻 stale(单子"证据涉及改动
            // 后,旧 validation 要按影响范围翻 stale")。
            if (goalns::EvidenceStalesOnWrite(evidence->kind)) {
                for (const auto& id : coordinator_->EvidenceIds()) {
                    const auto* existing = coordinator_->FindEvidence(id);
                    if (existing != nullptr && goalns::EvidenceStalesOnWrite(existing->kind)) {
                        coordinator_->MarkEvidenceStale(id);
                    }
                }
            }
        }
        // 证据白名单喂给 checkpoint 工具状态:本轮采到的 id 是下一轮
        // goal_checkpoint 引用校验的白名单底(旧证据 id 引了报 unknown,
        // 单子:只能引用本 goal、本 iteration 已产生的 evidence id)。
        if (checkpoint_state_ != nullptr) {
            checkpoint_state_->valid_evidence_ids = std::move(fresh_ids);
        }
    }

    // ---- 2) checkpoint:工具调了取最后一枚,没调合成 missing ----
    lubancode::runtime::goal::GoalCheckpoint checkpoint;
    bool has_tool_checkpoint = false;
    if (checkpoint_state_ != nullptr && checkpoint_state_->HasCheckpoint() &&
        checkpoint_state_->goal_id == task->id) {
        const auto candidate = checkpoint_state_->Candidate();
        if (candidate.has_value()) {
            has_tool_checkpoint = true;
            checkpoint.version = 1;
            checkpoint.summary = candidate->summary;
            checkpoint.completed = candidate->completed;
            checkpoint.remaining = candidate->remaining;
            checkpoint.next_action = candidate->next_action;
            checkpoint.evidence_ids = candidate->evidence_ids;
            checkpoint.blocker_key = candidate->blocker_key;
            checkpoint.question = candidate->question;
            using GoalCheckpointStatus = lubancode::tools::GoalCheckpointStatus;
            checkpoint.synthesized = false;
            (void)GoalCheckpointStatus::Progress;  // 枚举仅对齐注释,不另存
        }
    }
    if (!has_tool_checkpoint) {
        checkpoint = coordinator_->MakeMissingCheckpoint();
    }
    const auto checkpoint_result = coordinator_->CheckpointReached(checkpoint, now_ms);
    if (!checkpoint_result.ok) {
        Notify(/*is_error=*/true, "goal checkpoint 落账失败: " + checkpoint_result.error_message);
        return;
    }
    // checkpoint 工具账清零:下一枚 iteration 从头攒(状态是会话级复用的)。
    if (checkpoint_state_ != nullptr) {
        checkpoint_state_->entries.clear();
    }

    // provider 账:turn 失败记连败(撞闸 coordinator 自己收 Paused)。
    coordinator_->NoteProviderOutcome(!turn_failed);

    // ---- 3) evaluator:独立无工具请求,判词不混 main history ----
    if (turn_failed) {
        // 请求都没成:evaluator 没材料可判,不烧这一趟。goal 留在原态,
        // 连败账已在上面记;下一圈泵再问(pause_requested/终态会拦)。
        return;
    }
    const auto* task_now = coordinator_->task();
    if (task_now == nullptr || task_now->state != lubancode::runtime::goal::GoalState::Evaluating) {
        return;  // 状态没走到 Evaluating(收口前 pause 了):留账等 resume
    }
    lubancode::runtime::goal::GoalEvaluationInput input;
    input.task = *task_now;
    input.checkpoint = checkpoint;
    for (const auto& id : checkpoint.evidence_ids) {
        const auto* evidence = coordinator_->FindEvidence(id);
        if (evidence != nullptr) {
            input.evidence.push_back(*evidence);
        }
    }
    if (input.evidence.empty()) {
        // checkpoint 引用的证据一枚都没有:evaluator 没有可判的材料,
        // 记 provider 连败同路的"无材料"分支——判 continue 只会空转。
        Notify(/*is_error=*/false, "goal 轮收口:checkpoint 没有可核证据,不烧 evaluator(下轮先产证据)。");
        const auto schedule = coordinator_->ScheduleNextIteration(now_ms);
        if (!schedule.ok) {
            Notify(/*is_error=*/false, "goal 停排下一轮: " + schedule.error_message);
        }
        return;
    }
    if (task_now->last_evaluation.has_value()) {
        input.previous = *task_now->last_evaluation;
    }
    input.workspace_summary = "cwd: " + lubancode::platform::CurrentDirUtf8();
    input.now_ms = now_ms;

    lubancode::runtime::goal::GoalEvaluatorOptions evaluator_options;
    evaluator_options.model = *host_.current_model;
    if (host_.model_router != nullptr) {
        const auto routed_info = host_.model_router->RouteInfo(lubancode::agent::TaskKind::GoalEvaluate);
        if (!routed_info.model.empty()) {
            evaluator_options.model = routed_info.model;
            evaluator_options.reasoning_effort = routed_info.effort;
        }
    }
    const auto evaluation =
        lubancode::runtime::goal::RunGoalEvaluation(*host_.evaluation_backend, evaluator_options, input, nullptr);
    if (!evaluation.has_value()) {
        // evaluator 两坏/请求失败:goal 进 Paused(evaluator_failed),
        // 不默认 achieved 也不盲开下一轮(单子"evaluator 失败")。
        Notify(/*is_error=*/true, "goal evaluator 失败: " + evaluation.error() + ";目标转暂停(/goal resume 续)。");
        (void)coordinator_->NoteEvaluatorFailed(evaluation.error(), now_ms);
        lubancode::app::EmitGoalHook(
            MakeCommandWiring(host_.agent_tool ? host_.agent_tool() : nullptr,
                              host_.loop_scheduler ? host_.loop_scheduler() : nullptr),
            lubancode::hooks::HookEvent::GoalPaused,
            nlohmann::json{{"goal_id", task_now->id}, {"error", evaluation.error()}},
            /*match_value=*/"evaluator_failed");
        return;
    }
    coordinator_->AddUsage(evaluation->usage);

    // ---- 4) 判词落地:continue 排下一轮,terminal 收账 ----
    const auto applied = coordinator_->ApplyEvaluation(evaluation->evaluation, now_ms);
    if (!applied.ok) {
        Notify(/*is_error=*/true, "goal 判词落账失败: " + applied.error_message);
        return;
    }
    const std::string decision = applied.payload.value("decision", std::string());
    Notify(/*is_error=*/false, "[goal 判词: " + decision + "] " + evaluation->evaluation.summary);
    if (evaluation->evaluation.overridden_achieved) {
        Notify(/*is_error=*/false, "  (evaluator 判 achieved 被程序门槛改判 continue: " +
                                       evaluation->evaluation.override_reason + ")");
    }
    lubancode::app::EmitGoalHook(
        MakeCommandWiring(host_.agent_tool ? host_.agent_tool() : nullptr,
                          host_.loop_scheduler ? host_.loop_scheduler() : nullptr),
        lubancode::hooks::HookEvent::GoalEvaluated,
        nlohmann::json{{"goal_id", task_now->id},
                       {"iteration_id", checkpoint_result.payload.value("iteration_id", std::string())},
                       {"summary", evaluation->evaluation.summary.substr(0, 600)},
                       {"confidence", evaluation->evaluation.confidence}},
        /*match_value=*/decision);
    const auto* after = coordinator_->task();
    if (after != nullptr && lubancode::runtime::goal::IsGoalTerminal(after->state)) {
        // terminal 事件已落,GoalCompleted 在其后跑(单子:它失败不把
        // Achieved 改回 Active——OutputCapabilities 的 can_block=false
        // 正是这条边界)。
        lubancode::app::EmitGoalHook(
            MakeCommandWiring(host_.agent_tool ? host_.agent_tool() : nullptr,
                              host_.loop_scheduler ? host_.loop_scheduler() : nullptr),
            lubancode::hooks::HookEvent::GoalCompleted,
            nlohmann::json{{"goal_id", after->id},
                           {"decision", decision},
                           {"iterations", after->counters.iterations_started}},
            /*match_value=*/lubancode::runtime::goal::ToString(after->state));
    }
    if (applied.payload.value("schedule_next", false)) {
        const auto schedule = coordinator_->ScheduleNextIteration(now_ms);
        if (!schedule.ok) {
            Notify(/*is_error=*/false, "goal 停排下一轮: " + schedule.error_message);
        }
    }
}

}  // namespace lubancode::app
