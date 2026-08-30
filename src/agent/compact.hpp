// 上下文压缩(0.21.x 起,0.27.x 分层化第一期):history 长了(手动 /compact,
// ContextTracker 测出占用过 80%,或 AgentLoop 在模型请求前测出 projected
// overflow 触发 mid-turn 压缩)时,把历史送给模型换一份浓缩"存档"回来,
// 顶替掉老对话——跟 agent/context.hpp 的 TrimHistory(按字节硬切,不管
// 语义、纯粹丢内容)是两条路子:这里靠模型读懂对话再总结,保留任务目标/
// 关键决策/文件路径/代码要点/未完成事项,是主防线;TrimHistory 仍留着当
// 兜底的硬安全网,两者互不影响、互不依赖。
//
// 第一期堵的洞:
//   - 压缩模型自己的窗口单独算预算;窗口装不下时明确拒绝,不静默截史。
//   - 摘要末尾必须带可解析的 JSON manifest(目标/约束/待办/下一步);
//     manifest 缺失、解析不动、或"必须守恒的未完成事项"漏了一项,一律
//     拒收——旧 history 原样不动。
//   - 热区按 token 预算保留(不再固定"只留最后一轮")。

#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "agent/context_events.hpp"  // StructuralCompressionOptions(L1 工作视图口径)
#include "agent/model_router.hpp"  // BackgroundCallAccounting(usage 出账)

namespace lubancode::agent {

// compact_partition_count 的默认值与首版取值域(§八):默认 4,允许 2..8,
// 越界由配置层报错、不静默夹值。config 层(config.hpp)有同值镜像,依赖只
// 许单向(config 不牵扯 agent),改时两处一起改。
constexpr std::size_t kDefaultCompactPartitionCount = 4;
constexpr std::size_t kMinCompactPartitionCount = 2;
constexpr std::size_t kMaxCompactPartitionCount = 8;

// 压缩预算:按压缩模型自己的窗口算,不拿主模型 context_window 冒充。
struct CompactBudget {
    // 压缩模型的上下文窗口(token)。nullopt = 窗口未知——不做窗口拦截,
    // 但调用方应向用户说明"未按窗口校验",不许假装核过。
    std::optional<std::size_t> window_tokens;
    // 摘要输出预留(token),与请求里的 max_tokens 对齐。
    std::size_t output_reserve_tokens = 4096;
    // 协议与安全余量(token):system 指令、wire 包装、流式协议开销。
    std::size_t protocol_headroom_tokens = 2048;
};

// 压缩请求的可用输入预算:window - 输出预留 - 协议余量。窗口未知给
// nullopt(没有预算可用,只能不拦)。
std::optional<std::size_t> CompactInputBudget(const CompactBudget& budget);

// 摘要必须附上的机器 manifest。Markdown 栏目给模型读,manifest 给程序验,
// 两者由模型按同一份结构产出(指令里钉死键名),不许各写一份互相打架。
struct CompactManifest {
    std::string goal;                    // 当前任务目标(一句话)
    std::vector<std::string> constraints;  // 用户明示约束/禁止/验收句
    std::vector<std::string> open_items;   // 未完成事项(逐字收编活动待办)
    std::string next_action;              // 下一步要执行的准确动作
};

// manifest 守恒校验结果。ok=false 时 failures 逐条列拒收原因(人话,
// 直接给用户看)。
struct CompactValidation {
    bool ok = false;
    std::vector<std::string> failures;
};

// 守恒校验(纯函数):
//   - goal 非空;
//   - required_open_items(活动 plan/todo 的未完成条目)每一条都必须在
//     open_items 里逐字在场(空白归一后比对)——摘要漏一项 pending 就拒收,
//     旧 history 不动。
CompactValidation ValidateCompactManifest(const CompactManifest& manifest,
                                          const std::vector<std::string>& required_open_items);

// 从摘要正文里解析 manifest:取末尾最后一个 ```json 围栏块,按
// goal/constraints/open_items/next_action 键取值。缺围栏块、坏 JSON、
// 缺必填键,给 nullopt。
std::optional<CompactManifest> ParseCompactManifest(const std::string& summary_text);

// 压缩参数。
struct CompactOptions {
    std::string focus;  // /compact <重点>:额外重点保留一段
    CompactBudget budget{};
    // 必须守恒的未完成事项(活动 todo 的 pending/in_progress 条目原文)。
    // 空表 = 没有可钉的待办,manifest 只做结构校验。
    std::vector<std::string> required_open_items;
    // compact_partition_count(§八):compact 触发后把原始 turns 切成几份,
    // 末份是热区,前 partition_count-1 份各 map 一次。默认 4;取值域
    // 2..8 由配置层校验,这里只按收到的数算。
    std::size_t partition_count = kDefaultCompactPartitionCount;
};

// 一次压缩的产物:archive 消息(顶进新历史用) + 解析出的 manifest。
struct CompactSummary {
    api::Message archive;
    CompactManifest manifest;
};

// 热区 token 预算:压缩后从最后一轮用户输入往前按整轮收,收满预算为止;
// 末轮装得下时整轮保留,装不下时轮头(最新用户消息)必保、其后按
// assistant+tool_result 消息组从尾收——不再"整轮无论多大都保留"。那一
// 条在 mid-turn 长工具循环上会让压缩反涨(整轮吞掉全部历史,存档只添
// 不删,真机 70.8k 压成 73.7k)。TrimHistory 是热区后面的安全网。
constexpr std::size_t kDefaultHotZoneTokens = 12000;

// 压缩滞回带(P1-1 连环压缩):上次压缩收口后,新增内容不足这个量时,
// 再压一次榨不出新空间——热区+存档本身就有十几 k 的底。同一 turn 无
// 进展不得连压两次,靠这条带隔开。
constexpr std::size_t kCompactHysteresisFloorTokens = 4096;

// 滞回判定(纯函数):last_post_tokens 是上次压缩收口(成功换账或明确
// 拒收)时压力口径的估算,before_tokens 是这次触发时的同一口径估算。
// 返回 true = 跳过这次压缩(无进展,再压也是拿同一副牌重洗)。
inline bool ShouldSkipCompactForHysteresis(std::size_t last_post_tokens, std::size_t before_tokens,
                                           std::size_t floor_tokens = kCompactHysteresisFloorTokens) {
    const std::size_t grew = before_tokens > last_post_tokens ? before_tokens - last_post_tokens : 0;
    return grew < floor_tokens;
}

// 拼出压缩后的完整新历史:archive 消息并入热区第一条 user 消息开头(不
// 单独成一条,免得相邻两条 user 违反 Anthropic 角色交替),其后是按
// hot_zone_tokens 预算保留的最近若干完整轮。history 里找不到真正的用户
// 文本输入时,新历史里只有 archive 这一条。
// kept_indices 出参(可空):保留集在原 history 里的消息下标(升序)。连续
// 保留时就是 [keep_from, end);末轮超预算走组收法时会跳过没收进的中段
// 组——compact 事件得带上这份跳跃集,/resume 才不会把被替换的旧历史又
// 装回来。
std::vector<api::Message> BuildCompactedHistory(const std::vector<api::Message>& history,
                                                const api::Message& archive,
                                                std::size_t hot_zone_tokens = kDefaultHotZoneTokens,
                                                std::vector<std::size_t>* kept_indices = nullptr);

// 向模型请求一次历史压缩。指令要求:固定六栏 Markdown 存档 + 末尾一枚
// ```json manifest;required_open_items 非空时,指令明说这些条目必须逐字
// 进 open_items。
// 发送前先按预算核窗口:估算输入(指令 + 全份 history,统一 token 口径)
// 超过 CompactInputBudget 时直接拒绝(不发请求),错误信息写明窗口数与
// 估算体积——分块压缩是下一期的活,这期绝不静默截史。
// 成功条件(全过才收,任一不过返回错误、调用方旧 history 不动):
//   1. 流式请求成功、无 StreamError;
//   2. Markdown 正文剥空白后不少于 40 个 UTF-8 码点(防 prefill 残次品);
//   3. 末尾有可解析的 JSON manifest;
//   4. ValidateCompactManifest 通过(目标非空、待办守恒)。
std::expected<CompactSummary, api::Error> Compact(api::Backend& backend, const std::string& model,
                                                  const std::vector<api::Message>& history,
                                                  const CompactOptions& options,
                                                  const std::string& reasoning_effort = std::string(),
                                                  BackgroundCallAccounting* accounting = nullptr);

// ---------------------------------------------------------------------------
// 第三期:分阶段、分层摘要(map/reduce)
// ---------------------------------------------------------------------------

// 一段探索阶段的局部摘要(map 产物)。evidence_refs 是程序按事件账钉上的
// 来源事件号区间("e12-e45"),不由模型自己编——模型只写正文与 manifest,
// 来历由账说话。
struct EpisodeSummary {
    std::string markdown;        // 六栏小结正文
    CompactManifest manifest;    // 该段的 goal/open_items 等
    std::string evidence_refs;   // "e12-e45"(来源事件区间,程序钉)
    std::size_t from_message = 0;
    std::size_t to_message = 0;  // [from, to) 消息区间
};

// 分层压缩的指标:写进 compact_v2 事件,观测/回放都能看。
struct HierarchicalMetrics {
    int chunks = 1;             // map 块数;1 = 单次装下,没分层
    int reduce_passes = 0;      // 归并轮次(局部摘要仍超预算时的再归并)
    bool hierarchical = false;  // chunks > 1
    // 观测账(第四期钩子,现在就记):
    //   implementation——local-single(单次)/ local-hierarchical(分层),
    //     对照观测账里的 implementation 口径;远端 compact 将来另加 "remote"。
    //   source_digest——本次压缩输入(整份历史)的内容指纹:将来做"episode
    //     关闭后后台预计算局部摘要、正式触发时按 digest 复用"(第四期)就靠
    //     它判失效——digest 未变可复用,变了重算。现在只记不用。
    std::string implementation = "local-single";
    std::string source_digest;
};

// 一次压缩的完整产物:archive + manifest + 指标。
struct LayeredCompactResult {
    api::Message archive;
    CompactManifest manifest;
    HierarchicalMetrics metrics;
};

// episode 边界(纯函数,显式信号优先):每条外层用户文本输入(新要求/纠正)
// 与每个 todo_write 调用(plan 变化)都开新段;切块绝不劈开 tool use/result
// (段界只落在轮边界上)。返回每段的 [from, to) 消息区间。
std::vector<std::pair<std::size_t, std::size_t>> SplitEpisodes(const std::vector<api::Message>& history);

// 分层压缩:历史装不进压缩模型单次输入预算时,先按 episode 边界切块,
// 各块独立产出带 evidence_refs 的局部摘要(map),再归并成最终存档与
// manifest(reduce);归并输入仍超预算就两两归并,直到装得下。装得下时
// 退化为单次压缩,与 Compact() 同路同校验。上一轮存档只作为 reduce 的
// 一份"既有工作状态"参考输入,绝不拿摘要复印摘要——局部摘要永远从
// 原始消息块来(阻断递归失真)。失败任一环(请求失败/摘要残次/守恒不过)
// 返回错误,调用方旧 history 不动。
std::expected<LayeredCompactResult, api::Error> CompactHierarchical(api::Backend& backend,
                                                                     const std::string& model,
                                                                     const std::vector<api::Message>& history,
                                                                     const CompactOptions& options,
                                                                     const std::string& reasoning_effort = std::string(),
                                                                     BackgroundCallAccounting* accounting = nullptr);

// ---------------------------------------------------------------------------
// 四分区(Compact 四分区单·阶段 1):TurnPartitionPlan 纯计算,不起模型。
//
// §三《一次 compact 的完整算法》的切法,先按 turn 收齐、再照 L1 工作视图
// 的 token 重量切成连续 partition_count 份:边界只落 turn 之间(工具原子组
// 天然不被劈开——组永远住在一枚 turn 里),前 n-1 份是冷区 map 输入,末份
// 是热区。plan 只算账:哪些 turn 进哪份、各占多少 token、哪些 ToolResult
// 已外置成 artifact 视图、分区/单 turn 是否超压缩模型预算。真跑 map/reduce
// 是阶段 2/3 的事,这里一次模型都不调。
// ---------------------------------------------------------------------------

// map 输入预算诊断时给压缩指令留的公开估算档(与 /context 预算总账、
// BuildCompactOptions 里的 compact_prompt_overhead_tokens 同档)。
constexpr std::size_t kTurnPlanPromptOverheadTokens = 512;

// 一枚原始 turn 的画像(分区用 L1 工作视图那把尺)。
struct TurnInfo {
    std::size_t number = 0;  // 1 起的 turn 序号(原始 turns 里第几枚)
    std::string id;          // "t1","t2"... 宿主生成,给 map/reduce 钉来源
    std::size_t from_message = 0;  // [from,to) 在原 history 里的消息区间
    std::size_t to_message = 0;
    std::size_t working_tokens = 0;  // L1 工作视图 token(长 ToolResult 按外置后算)
    std::size_t raw_tokens = 0;      // 全量口径 token(对照,看外置省了多少)
    std::size_t externalized_results = 0;  // 已外置成 artifact 视图的 ToolResult 数
    std::size_t tool_groups = 0;           // 本 turn 覆盖的工具原子组数
};

// 一个分区:P1..Pn 连续 turn 段,末份是热区。
struct TurnPartitionInfo {
    std::string label;           // "P1".."Pn"
    std::size_t first_turn = 0;  // [first_turn,last_turn) 的 turn 下标(0 起)
    std::size_t last_turn = 0;
    std::size_t working_tokens = 0;
    std::size_t externalized_results = 0;
    bool is_hot = false;  // 末分区:保原始消息形状,只进 final reduce
    bool over_map_budget = false;  // 超压缩模型单次输入预算(map 时须沿 turn 再切)
};

// 工具原子组(§6.1):一条 assistant 消息(可含并行多枚 tool_use)加上按
// tool_use_id 收齐的全部配对 result。plan 里只作画像与"不拆"的证据——分
// 区边界只落 turn 之间,组整组随所属 turn 进退。
struct ToolExchangeGroupInfo {
    std::size_t assistant_message = 0;  // 发起调用的 assistant 消息下标
    std::size_t from_message = 0;       // [from,to) 覆盖 assistant..最后一条配对 result
    std::size_t to_message = 0;
    std::vector<std::string> tool_use_ids;  // 本组发出的全部调用(并行调用全在列)
    std::size_t turn = 0;                   // 所属 turn 下标(0 起)
    bool complete = true;  // 全部 use 都有配对 result;false = orphan/incomplete
};

// BuildTurnPartitionPlan 的预算侧输入。
struct TurnPartitionBudgets {
    StructuralCompressionOptions structural{};  // L1 工作视图口径(外置/判重)
    CompactBudget compact_model{};              // 压缩模型自己的窗口(预算诊断)
};

struct TurnPartitionPlan {
    // 旧 archive(§3.2):首条 user 文本以上一轮存档开头时剥出,不算 turn、
    // 不占分区账,只作 final reduce 的既有基线。
    bool has_prior_archive = false;
    std::string prior_archive_text;
    std::size_t prior_archive_tokens = 0;

    std::vector<TurnInfo> turns;             // 全部原始 turn,照原 history 顺序
    std::vector<TurnPartitionInfo> partitions;  // P1..Pn,连续无缝盖住全部 turn
    std::size_t requested_partition_count = kDefaultCompactPartitionCount;
    std::size_t map_calls = 0;  // 冷区 map 次数 = 分区数 - 1(0 = 没有冷区)

    std::vector<ToolExchangeGroupInfo> tool_groups;
    bool has_incomplete_tool_exchange = false;  // orphan tool_use / 悬空 result
    std::size_t dangling_results = 0;           // 配不上 use 的 result(异常形状)

    std::size_t total_working_tokens = 0;  // turns 合计(不含 prior archive)
    std::size_t total_raw_tokens = 0;      // 同口径的全量对照
    std::size_t externalized_results = 0;  // 全史外置 ToolResult 总数

    // 预算诊断:nullopt = 压缩模型窗口未知,没法校验(不假装核过)。
    std::optional<std::size_t> compact_input_budget;
    bool any_partition_over_map_budget = false;
    bool any_turn_over_map_budget = false;  // 单 turn 装不下:该次 compact 须明确拒绝

    // §9.3:没有冷区(分区数 <= 1)且没有旧存档时,压缩榨不出东西。
    bool WorthCompacting() const { return partitions.size() > 1 || has_prior_archive; }
};

// 纯函数:剥旧 archive → 按 §二 切 turn → 按 L1 工作视图 token 平衡切成
// min(turn 数, partition_count) 份连续分区(边界只落 turn 之间)→ 顺工具
// 原子组、外置账与预算诊断。不调模型、不改 history、不落盘。
TurnPartitionPlan BuildTurnPartitionPlan(const std::vector<api::Message>& history,
                                         std::size_t partition_count,
                                         const TurnPartitionBudgets& budgets);

}  // namespace lubancode::agent
