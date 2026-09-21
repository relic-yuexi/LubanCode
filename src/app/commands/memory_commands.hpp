// 终端接线收尾单:/memory 命令 presenter。原先整只 HandleMemoryCommand
// (387 行单函数)住在 interactive_session 大类里,按病灶二拆出:命令
// 产出数据行(project_memory 的调用与结果),presenter 负责怎么画(输出
// 全走 TerminalPort);大类只留分派(一行调用)与会话状态。
//
// ensure_tool:会话侧 EnsureMemoryTool 的接线——on/learn 档位打开后要把
// memory_save 工具补注册进 registry,这事归会话(工具栈在它那),命令层
// 只回调报一声。

#pragma once

#include "app/commands/command_flow.hpp"  // CommandFlow(分派注册制)
#include "cli/slash_commands.hpp"          // ParsedSlashCommand(分派注册制)

#include <expected>
#include <functional>
#include <string>

#include "agent/agent.hpp"        // Agent(批四自立门户)
#include "agent/model_router.hpp"  // TaskKind/ModelRole
#include "app/memory_extract.hpp"  // MemoryTurnLedger(P0 调度账)
#include "app/turn_memory_extractor.hpp"  // TurnMemoryExtractor(回合总结异步化)

namespace lubancode::memory {
class ProjectMemory;
}
namespace lubancode::sessions {
class SessionStore;
}
namespace lubancode::cli {
struct Theme;
}
namespace lubancode::runtime {
class TrajectorySessionLedger;
}

namespace lubancode::app {

class ModelRouterService;

struct MemoryCommandContext {
    lubancode::memory::ProjectMemory* project_memory = nullptr;  // 空 = 未装配(unavailable)
    const lubancode::cli::Theme* theme = nullptr;
    // on/learn 成功后回调(补注册 memory_save;可空 = 单测/无 registry 场景)。
    std::function<void()> ensure_tool;
};

// /memory 的全部动作:status/on/off/use/learn/review/accept/reject/edit/
// why/list/jobs/retry/remember/forget/rebuild/stale/verify/refresh/show/
// open/migrate。用法不对打用法;project_memory 未装配打 unavailable。
void HandleMemoryCommand(const MemoryCommandContext& ctx, const std::string& raw_args);

// ---- 会话尾款的 memory 接线(终端接线收尾单自大类搬出) --------------------
//
// 回合收尾的记忆抽取——"回合外面的后台小活",原先住在大类里,搬到这里;
// 材料经 SessionTailContext 递入。(会话起名原是另一样,实测问题 7 后搬
// 到会话控制器:首问建档当场起本地标题,精炼走 SessionTitleRefiner 异步,
// 不再在回合收尾同步等 cheap。T17:artifact 按需摘要
// SummarizeArtifactOnDemand 已随旧仓与 context_read 一并退役。)
//
// 记忆回合总结异步化单:前置门留前台(纯本地,微秒级),门过即把采样+
// 解析丢进 TurnMemoryExtractor 的后台线程——收口即刻还输入框,learn 开着
// 的每一场不再同步等 cheap 往返。收账(usage/候选入队/台账落袋)在主线程
// 空闲拍走 SettleTurnMemory。
struct SessionTailContext {
    lubancode::memory::ProjectMemory* project_memory = nullptr;
    lubancode::agent::Agent* agent = nullptr;          // 活 loop(history 与路由)
    lubancode::app::ModelRouterService* model_router = nullptr;
    const std::string* prompts_dir = nullptr;          // 抽取系统提示的运行时模块
    const lubancode::cli::Theme* theme = nullptr;
    // Token 账本单 A1(旁路落账):flag 开的会话递账本,回合收尾的抽取铸
    // 旁路桥落 Journal(purpose=memory_extract)。空 = 没接轨迹,行为与
    // 从前一致。wire 是桥 identity 的渠道名(与主 turn 桥同源)。
    lubancode::runtime::TrajectorySessionLedger* trajectory = nullptr;
    std::string trajectory_wire;
    // 记忆写入调度单 P0:回合级调度账(漏斗/Token/尾延迟 + 写路回执)。
    // 空 = 没开账(单测),抽取一切照旧。
    lubancode::app::MemoryTurnLedger* memory_turns = nullptr;
    // ---- 回合总结异步化单 ----
    // 门过起飞的后台执行器(会话控制器持一只,活一场会话)。空 = 没接
    // 执行器:门过也只能记一笔失败账让位(同步等待网络的旧路已退役,
    // 不留两套行为)。
    lubancode::app::TurnMemoryExtractor* extractor = nullptr;
    // 起飞时的会话世代(/clear、/resume 翻号)与本轮轮号:迟到收账对档,
    // 换代弃旧账。轮号也进 MemoryTurnLedger 的悬账(账落对档)。
    std::uint64_t session_generation = 0;
    std::string turn_id;
};

// ExtractTurnMemory 的收口档(调用方据此决定回合账怎么落):
enum class TurnMemoryDispatch {
    Skipped,      // 前置门拦下(learn off/短文本/同轮去重/…):不发,账已前台记完
    Dispatched,   // 门过:后台已起飞,回合账悬起(SuspendTurn),迟到收账走 SettleTurnMemory
    DroppedBusy,  // 门过但单飞在途(上一枚没收走):本轮让位不发,账已前台记完
    DroppedRoute, // 门过但路由落空/执行器没接:不发,零账与 route_miss 已前台记完
};

// 回合收尾抽取(前台半边):只看本轮增量;前置门全在本地(必跳层/同轮
// 去重/短文本/耐久信号/转写与提示拼装),门过把采样丢后台——不等网络。
// 返回收口档:门拦的回合调用方立即 FinishTurn,Dispatched 的回合
// SuspendTurn 悬账。前台不再打 [memory] 起跑行(收口即刻还输入框),
// 完成与失败的行都在收账点(SettleTurnMemory)打。
TurnMemoryDispatch ExtractTurnMemory(const SessionTailContext& ctx, const std::string& user_text,
                                     std::size_t history_before);

// 迟到收账(主线程空闲拍):完工的抽取结果记 usage 账(由调用方在世代门
// 之前记,弃账也照记)、检索扩展词、候选入队(auto 档直写闸照旧)、台账
// 补 outcome 落袋,打完成/失败行。tail_wall_ms = 回合收口到收账完成的墙钟
// (MemoryTurnLedger 的并档口径,见 FinishTurn 注释)。世代门(换代弃迟到)
// 在调用方,这里只管对档落袋。
void SettleTurnMemory(const SessionTailContext& ctx, const TurnMemoryExtractor::Outcome& outcome,
                      std::int64_t tail_wall_ms);

// 命令分派注册制(会话终章):/memory 的分派位。HC-06(材料收窄,第二
// 小批)起只吃窄材料——材料在组合根折好,Memory 域编译不再需要
// SlashDispatchContext。
CommandFlow HandleSlashMemory(const MemoryCommandContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
