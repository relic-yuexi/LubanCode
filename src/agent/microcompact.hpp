// L2 microcompact(渐进式上下文装载第三期):冷区超长工具结果的局部语义
// 压缩,默认走 cheap 路由。
//
// 四级压缩阶梯(项目内钉死的名词,规格"四级压缩阶梯"):
//   L0 原样与按需装载;L1 snip = 确定性裁面(结构压缩的 artifact 预览,
//   不调用模型——已在第二期落地);L2 microcompact = 本文件,局部语义摘要;
//   L3 global compact = 替换整份旧史(agent/compact);L4 hard trim 安全网。
//
// L2 的硬规矩:
//   - 一次只处理冷区里一枚 tool result(热区 = 最后一条用户文本输入之后,
//     绝不碰);每趟最多收拾 max_per_pass 枚,按字节从大到小。
//   - 产物是 versioned summary,带 source artifact 引用;原文永不删——
//     blob 与 session JSONL 照旧,模型觉得摘要不够可 context_read 回读。
//   - 输入永远从 blob 原文来(不拿摘要再摘要);确无原文可读时跳过,不做
//     derived_from_summary 的套娃(规格"不要反复拿摘要再摘要")。
//   - cheap 失败、超时、输出坏 JSON:这一枚退回 L1 预览,不删原文、不拦
//     主流程(规格"cheap 失败、超时、输出坏格式时,退回 L1")。
//   - 触发线带迟滞:压完一趟后,冷区字节要比上趟再长 cooldown_growth_
//     percent 才准再压——免得刚压完又立刻重压。
#pragma once

#include <atomic>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "agent/artifact_store.hpp"
#include "agent/context_events.hpp"
#include "agent/model_router.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"

namespace lubancode::agent {

// 触发与预算的旋钮(阈值可由调用方覆写;默认值公开在这里,不藏魔数)。
struct MicrocompactOptions {
    // 冷区 artifact 视图累计字节超过这条线才触发(默认 32 KiB:比单条
    // snip 线高一档——冷区攒了几条长结果才值得请一次 cheap)。
    std::size_t cold_trigger_bytes = 32 * 1024;
    // 每趟最多收拾几枚(渐次收拾,不一趟全压;默认 3)。
    int max_per_pass = 3;
    // 单枚喂给 cheap 的原文上限(头尾各半;超了从行边界截,不劈码点)。
    std::size_t input_cap_bytes = 24 * 1024;
    // 迟滞:压完一趟后冷区字节须比上趟再涨这么多才触发下一趟(百分比)。
    int cooldown_growth_percent = 50;
    // cheap 调用超时(秒)。
    int timeout_secs = 45;
};

// 迟滞的活账:调用方(会话层)持一份跨轮次更新。
struct MicrocompactHysteresis {
    // 上一趟压完时的冷区字节数;0 = 还没压过(首趟不受迟滞限制)。
    std::size_t last_pass_cold_bytes = 0;
    // 上一趟是否真压成了至少一枚(全失败也要冷却,不许立刻重试烧钱)。
    bool pass_attempted = false;
};

// 候选:冷区里已经落盘成 artifact、还没被 L2 收拾过的 tool result。
struct MicrocompactCandidate {
    std::string tool_use_id;
    std::string artifact_id;
    std::string event_id;   // 事件账里的 eN(source ref 钉桩用)
    std::string tool_name;
    std::size_t content_bytes = 0;
};

// 挑候选(纯函数,单测钉):冷区 = 热区起点之前的消息;候选 = memo 里
// kind=Artifact 且带 artifact_id 的决策(已落盘可回读原文)。按字节从大
// 到小取前 max_per_pass 枚。返回空 = 不触发。
std::vector<MicrocompactCandidate> PickMicrocompactCandidates(const std::vector<api::Message>& history,
                                                              const ResultViewMemo& memo,
                                                              const MicrocompactOptions& options,
                                                              const MicrocompactHysteresis& hysteresis);

// 冷区 artifact 视图累计字节(触发线的度量;纯函数)。
std::size_t ColdArtifactBytes(const std::vector<api::Message>& history, const ResultViewMemo& memo);

// 一枚 L2 产物:versioned summary,source refs 钉牢(规格 L2 节)。
struct MicrocompactSummary {
    std::string summary;        // 给模型看的摘要正文(Markdown 几行)
    std::vector<std::string> key_facts;   // 决定、错误码、路径、符号、命令、退出码
    std::string source_artifact_id;       // 原文的 artifact id(context_read 可回)
    std::string source_event_id;          // 事件账里的 eN
    std::string model;                    // 产出模型(看得见)
    // 输入是否是旧摘要(套娃标记):v1 输入永远来自 blob 原文,恒 false;
    // 将来确需从摘要重做时必须置真并在视图里写明(防一轮轮走样)。
    bool derived_from_summary = false;
};

// 解析 cheap 的严格 JSON 输出(容错:剥代码围栏、取首 { 到末个 })。
// 坏 JSON 给 nullopt——调用方把这枚退回 L1,不删原文。
std::optional<MicrocompactSummary> ParseMicrocompactSummary(const std::string& text,
                                                            const std::string& artifact_id,
                                                            const std::string& event_id);

// 发一次 L2 请求(同步,带看门狗取消):input 永远取 blob 原文(头尾各半
// 截到 input_cap_bytes,行边界不劈码点)。失败只返回错误,调用方退 L1。
// accounting 非空时记 usage/时长(分角色台账,cheap 档)。
std::expected<MicrocompactSummary, std::string> RunMicrocompact(
    api::Backend& backend, const std::string& model, const std::string& reasoning_effort,
    const ContextArtifactStore& store, const ArtifactRef& ref, const std::string& event_id,
    const MicrocompactOptions& options, BackgroundCallAccounting* accounting = nullptr,
    const std::atomic<bool>* external_cancel = nullptr);

// 应用一趟产物(视图换摘要,原文不动):更新 memo 里对应决策为 L2 视图。
// 返回换掉的枚数。已应用过的(无 artifact_id 匹配)自然跳过。
int ApplyMicrocompactSummaries(ResultViewMemo& memo, const std::map<std::string, MicrocompactSummary>& summaries);

}  // namespace lubancode::agent
