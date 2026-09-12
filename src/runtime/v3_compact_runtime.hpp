// v3 compact 运行时接线(compact 全链唯一驱动者):把已落库的
// v3::CompactSession 状态机接进会话运行时——触发(manual/auto)、范围
// 冻结、发送前容量门禁与整轮回退(§4.64)、压缩请求组装(压缩专用
// system + 材料清单 + 指令,§4.6)、模型采样、候选留档、必需内容校验
// (§4.8)、compact.applied 原子提交。单子
// todos/session轨迹v3_消息主轴树链与四角色壳收敛设计.todo §4.6-4.10/
// §4.37-4.41/§4.64。
//
// 分层:本件只依赖 trajectory/v3 库(writer/compact/reader)与 nlohmann,
// 不认 api::Backend——压缩模型经 V3CompactModelClient 抽象口递进
// (生产侧适配在 app 层用 agent::SampleModel 铸,单测喂桩)。token 估算
// 首版 bytes/4(§1.14:ceil(utf8Bytes/4));§4.36 的"估算=内置 hook"切换
// 等 LuaHook P0-B,本棒不接中间件。
//
// 触发语义(§1.15/§4.37):顶层 trigger 只分 manual/auto;auto 的 reason
// 分 threshold(水位触发)/pre_send_overflow(发送前容量门禁不通过)/
// context_overflow(provider 报输入超窗)。
//
// 撞窗回退(§4.64):本地预检 Ic+Oc+Mc<=Cc 不过时,先把保留尾部(R)中
// 仅供参考的整轮移出摘要输入;仍不足从可压缩历史(H)最新一轮起整轮移出
// 组成连续保留尾部 K——目标一次回退至少让出 minRetreatTokens(首版
// 8192),整轮较大允许超出,不拆工具原子组;每步落 compact.range.
// retreated。回退不发明知超限的请求;H 退空仍不过则 rejected 收场。
//
// 崩溃边界(§4.6/§4.8):applied 落稳前一切只是候选,resume 仍用旧上下文
// (writer::Continue 重放 open_compact_ids);applied 落稳后 resume 从
// applied 重建新链。本运行时不另造恢复路径,只保证每步先落账再动手。
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/v3/writer.hpp"

namespace lubancode::runtime {

// ---------------------------------------------------------------------------
// 压缩模型客户端抽象(生产接 api::Backend,测试喂桩)
// ---------------------------------------------------------------------------

// 一次压缩请求的回复。ok=false 时其余字段只在有料时填。
struct V3CompactModelReply {
    bool ok = false;
    // 稳定失败码:provider_error / sse_error / empty_response /
    // input_context_overflow(服务端确认输入超窗)……
    std::string error_code;
    std::string error_detail;  // 人话(原始错误摘要,不进事件 payload 正文)
    std::string text;          // assistant 正文(成功时;空正文按失败收)
    // finish_reason=length 一类:候选按 truncated 留档,不 applied(§4.39)。
    bool truncated = false;
    // provider 实报 usage;缺失给 nullopt——usage 唯一 owner,缺实报不补 0
    // (§4.12/§5.1"provider usage 缺失不补 0")。
    std::optional<nlohmann::json> usage;
};

class V3CompactModelClient {
public:
    virtual ~V3CompactModelClient() = default;
    // system:压缩专用 system 正文;messages:材料消息(role/content 原样,
    // 链序)+ 末尾一条压缩指令。同步;永不抛。
    virtual V3CompactModelReply Send(const std::string& system,
                                     const std::vector<nlohmann::json>& messages) = 0;
};

// ---------------------------------------------------------------------------
// 容量与策略(§4.40/§4.64)
// ---------------------------------------------------------------------------

struct V3CompactProfile {
    // Cc:压缩模型上下文窗口(token)。0 = 未知——门禁不做,结果里明说
    // "窗口未知未校验",不假装核过(与 v2 CompactBudget 同口径)。
    std::uint64_t compact_window_tokens = 0;
    // Oc:压缩请求的有效输出预留(模型有效 max_tokens,§1.18)。
    std::uint64_t compact_output_reserve_tokens = 4096;
    // Mc:显式安全余量(协议头/估算误差边)。
    std::uint64_t compact_margin_tokens = 2048;
    // 主模型窗口/输出预留:applied 前的第二道门禁(S+Q新+K+O主<=C主,
    // §4.64"前后两道容量门禁")。窗口 0 = 不做,如实标注。
    std::uint64_t main_window_tokens = 0;
    std::uint64_t main_output_reserve_tokens = 0;
    // 一次回退至少让出的 token(§4.64 首版建议 8192)。
    std::uint64_t min_retreat_tokens = 8192;
    // 同一恢复操作的回退(计划修订)上限;超限或退空仍不过则 rejected。
    int max_retreat_steps = 3;
    // 压缩模型身份(candidate 落档与请求快照用)。
    std::string provider;
    std::string wire;
    std::string model;
    // 估算口径名(进 tokenMetric 与回退事件)。
    std::string estimator = "utf8_bytes_div4";
};

// ---------------------------------------------------------------------------
// 一次压缩的入参与结果
// ---------------------------------------------------------------------------

struct V3CompactRunInput {
    std::string trigger;  // manual | auto(§1.15 顶层只此两值)
    std::string reason;   // user_command | threshold | pre_send_overflow | context_overflow
    // turn 中途自动压缩挂主 turn;空闲手动为 nullopt(§4.6:不假称上一轮
    // 仍在运行)。
    std::optional<std::string> parent_turn_id;
    // 校验清单快照(§4.8 内容结构)。认得的键:
    //   schema(版本串)、requiredStringFields[](非空 string)、
    //   requiredArrayFields[](在场的数组)、requiredOpenItems[](逐字守恒)、
    //   minSummaryChars(正文最短码点数,缺省 40)
    // 空对象/缺键按缺省合同(与 v2 manifest 同形:goal/next_action 非空串,
    // constraints/open_items 数组,末尾 ```json 围栏)。
    nlohmann::json requirements_snapshot = nlohmann::json::object();
    // 调用方钉死的整轮保留(未完成主 turn 等);运行时另把工具动作未收口
    // 的 turn 并进保护集(§4.8"工具配对"行:未完成 turn 保留完整消息结构)。
    std::vector<std::string> protected_turn_ids;
    // Capacity recovery may summarize closed old steps of parent_turn_id.
    // Explicitly protected other turns remain indivisible.
    bool allow_closed_step_compaction = false;
    // 压缩专用 system 正文(§4.3:另存实际内容,不替换会话 system)。
    // 空串 = 用本运行时的缺省模板。
    std::string special_system;
    // /compact <重点>:重点保留一段(进指令,不进校验清单)。
    std::string focus;

    // P1-C(compact 旁路请求切槽,§4.36/§7.2):压缩请求的输入估算经宿主
    // 的 PreRequest/estimate 槽位——用户同名替换的估算器对 compact 请求同
    // 样生效,旁路不再自带第二份公式。入参 = 模型输入快照({system,
    // messages, instruction});出参 = EST1 形状(须含 estimatedInputTokens)。
    // 空 = 旧路(消息级 bytes/4 合成;未接槽的调用方行为一字不变)。
    // 槽失败 → 本次压缩按 estimate_failed 收口,不回落内置公式假装核过
    //(§4.36 fail closed)。生产装配见 session_commands 的
    // runtime::EstimateBypassRequestTokens。
    std::function<std::expected<nlohmann::json, std::string>(const nlohmann::json&)> estimate;
};

struct V3CompactRunResult {
    bool began = false;      // false = 没开成场(busy/账读不了)
    bool applied = false;    // compact.applied 落稳
    std::string compact_id;  // 贯穿身份(began 时非空)
    std::string turn_id;     // 内部回合(compact-turn-*)
    // 终态:applied / failed / cancelled / rejected / busy / not_begun
    std::string terminal_kind;
    // 终态 reason(拒绝/失败的稳定码:validation_failed / no_eligible_
    // history / input_capacity_exceeded / retreat_budget_exhausted /
    // source_conflict / provider_error / empty_compact_response / …)
    std::string reason;
    // 同一口径(bytes/4)的前后上下文数字;applied 时即 applied 事件里的
    // contextTokensBefore/After(显示侧压缩分界线吃这两枚,§4.11)。
    std::uint64_t tokens_before = 0;
    std::uint64_t tokens_after = 0;
    int model_calls = 0;       // no_eligible_history/容量拒绝时为 0(不空调模型)
    int retreat_steps = 0;     // 实际发生的计划修订次数
    bool gate_checked = false;  // Cc>0 时做过发送前门禁
    bool window_unknown = false;  // Cc=0:门禁没做,如实标注
    std::vector<std::string> notes;  // 人话进度(终端/日志用,不进事件账)
};

// ---------------------------------------------------------------------------
// 主入口
// ---------------------------------------------------------------------------

// 跑一次 v3 compact 全链(单发、同步):Begin -> 范围计划 -> 发送前门禁与
// 整轮回退 -> Freeze -> 压缩专用 system + 指令 -> prepared -> 采样 ->
// 候选(截断标 truncated) -> 校验 -> applied / 失败三态。
// writer 是会话 v3 主账的唯一写者;同一主上下文一次只运行一个 compact
// (已有进行中的 compact 时返回 busy,不另开场)。
V3CompactRunResult RunV3Compact(trajectory::v3::V3Writer& writer,
                                V3CompactModelClient& client, const V3CompactProfile& profile,
                                V3CompactRunInput input);

// 估算口径(§1.14 首版):ceil(utf8 字节数 / 4)。公开给测试与显示侧对表。
std::uint64_t EstimateV3TokensUtf8Div4(std::string_view utf8);

}  // namespace lubancode::runtime
