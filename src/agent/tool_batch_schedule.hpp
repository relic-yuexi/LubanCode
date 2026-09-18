// 只读工具并行与写入串行单 P2:批次调度核心(策略两档 + 连续读段划批 +
// 有界执行器)。AgentLoop 的工具批次从这里拿"哪些调用能并行、并行到几只",
// 执行链本身(PrepareToolCall/MarkExecutionStarted/ExecuteApprovedTool/
// CompleteToolCall,loop.cpp)一套不拆——调度只接管"批次第二遍"的派发次序,
// 不开第二扇权限门。
//
// 调度合同(单子 §一):按模型声明序划连续读段与独占节点;读段内有界并行,
// 读段全收口(含结果落账与后处理)才跑独占节点,独占收口才启下段;不把
// 后面的 read 提到前面的 write 之前;结果按原槽位回填,完成先后不改排列。
//
// 策略两档(§三):
//   Exclusive    默认。全串行——与拆链前的行为一字不差,权限/Hook/审计
//                一样不少。
//   ParallelRead 连续只读段有界并行。只放行审定过的内置只读工具(首批
//                read_file/search),其余(写/undo/shell/Git/未知/插件/
//                Lua/MCP/宿主状态操作)一律独占;Hook 在场或混入
//                job_handle/native_deferred 时整批回退串行。
//
// EffectClass 不参与调度判定(§三:它服务恢复语义;job_cancel/loop_control
// 都挂着只读标签却在改宿主状态),放行只认"审定过的内置工具名 + 注册来源
// 是 Builtin + 不需确认"三件套。
//
// 所有权账(P2 立,单子 §四):有界执行器在段内全数 join 才返回,worker
// 手里的注册表/工具实例/wiring 引用在段收口前恒有效;热卸载/切场不得与
// 进行中的批次并发(宿主合同),执行器不留任何跨段全局状态。

#pragma once

#include <cstddef>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "api/types.hpp"                       // ToolUseBlock
#include "tools/deferred_tool_resolver.hpp"    // DeferredToolResolver/ProxyCallContext
#include "tools/tool.hpp"                      // Tool::Result
#include "tools/registry.hpp"                  // ToolRegistry

namespace lubancode::agent {

// 批次执行策略(两档,§三;默认 Exclusive——不声明便不并行)。
enum class ToolBatchStrategy { Exclusive, ParallelRead };

// 策略字符串(config 的 agent.tool_execution 与展示共用同一套词)。
// 认不得的值回 nullopt,调用方按默认档收口,不猜。
std::optional<ToolBatchStrategy> ParseToolBatchStrategy(const std::string& text);
std::string_view ToolBatchStrategyName(ToolBatchStrategy strategy);

// 读段并发上限:默认 4(§一"初始并发上限建议为 4,允许配置成 1"),硬帽 16。
// 1 = 调度器不接管,完整走串行语义(§四"并发配置为 1 或策略不满足时")。
inline constexpr int kDefaultParallelReadConcurrency = 4;
inline constexpr int kMaxParallelReadConcurrency = 16;
inline constexpr int kMinParallelReadConcurrency = 1;
int ClampParallelReadConcurrency(int configured);

// 首批审定放行的内置只读工具(§三:审过实例/共享 runner/路径解析/缓存/
// 取消/进程封装才开放;read_file 纯局部状态,search 的共享
// BundledRipgrepRunner 按合同支持并发 Run)。后续逐个审 web_fetch/
// web_search,审一个放一个。
// 判定三件套:名字在册 + 注册来源 Builtin(插件/MCP 不许借名影子内置件)
// + 工具不需确认(需要审批的调用走独占,确认流的旧语义不动)。
bool IsParallelReadAllowlistedBuiltin(const tools::ToolRegistry& registry, const std::string& tool_name);

// 一枚 wire 调用在 ParallelRead 策略下的入段资格(§三:deferred/tool_invoke
// 先解析真实目标再判策略,包装器透传;解析不了按独占收口——原错误路径在
// 串行那半边原样走,这里不做第二次裁决)。
struct ParallelReadEligibility {
    bool eligible = false;
    bool via_proxy = false;               // true = 经 tool_invoke 解引用来的
    api::ToolUseBlock resolved_call;      // eligible 时的执行目标(id 沿用 wire)
    tools::ProxyCallContext proxy;        // via_proxy 时的协议证据
};

ParallelReadEligibility ProbeParallelReadEligibility(const tools::ToolRegistry& registry,
                                                     const tools::DeferredToolResolver* resolver,
                                                     const api::ToolUseBlock& wire_call);

// 连续区段划批(纯函数,单测钉):eligible 的连续段合成一枚并行段,每个
// 不 eligible 的位置自成一枚独占节点。声明序保留,不重排。
struct ToolBatchSegment {
    std::size_t begin = 0;  // [begin, end) 下标域,对应批次声明的调用槽位
    std::size_t end = 0;
    bool parallel = false;  // true = 读段(有界并行);false = 独占节点(单枚串行)
};

std::vector<ToolBatchSegment> PlanToolBatchSegments(const std::vector<char>& eligible);

// 有界执行器:把一批"已获准执行"的任务(阶段三闭包)按上限并行跑完,
// 全数 join 后返回。合同:
//   - 结果与任务同序回填(完成先后不改排列,配对归调用方);
//   - 任务抛出的异常不在 worker 上传播(那会 std::terminate):逐枚存
//     exception_ptr,交回主线程折算,不吞、不冒充成功;
//   - 在跑峰值(peak_concurrency)不超 limit(原子计数,验收用);
//   - 线程按段起、按段收,不留常驻池——注册表/工具实例的生命由段收口
//     保证,不引入跨段全局状态。
class BoundedParallelExecutor {
public:
    struct RunOutcome {
        std::vector<tools::Tool::Result> results;        // 与任务同序
        std::vector<std::exception_ptr> failures;        // 同序;空 = 该任务未抛
        std::size_t peak_concurrency = 0;                 // 本次在跑峰值(<= limit)
    };

    // limit <= 0 按 1 处理(单任务也走同一只口,行为一致);任务数为 0
    // 直接返回空账。
    RunOutcome RunAll(int limit, std::vector<std::function<tools::Tool::Result()>> tasks);
};

}  // namespace lubancode::agent
