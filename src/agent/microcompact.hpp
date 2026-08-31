// L2 microcompact(渐进式上下文装载第三期):artifact 的按需局部摘要,
// 默认走 cheap 路由。模型只有显式调用 context_read(summarize=true)才发;
// 摘要随新的 tool_result 追加在历史尾部,绝不追改旧 artifact 预览。
//
// 四级压缩阶梯(项目内钉死的名词,规格"四级压缩阶梯"):
//   L0 原样与按需装载;L1 snip = 确定性裁面(结构压缩的 artifact 预览,
//   不调用模型——已在第二期落地);L2 microcompact = 本文件,局部语义摘要;
//   L3 global compact = 替换整份旧史(agent/compact);L4 hard trim 安全网。
//
// L2 的硬规矩:
//   - 不在回合收尾自动跑,不按冷区字节猜用户是否值得花 token。
//   - 一次只处理调用方点名的一枚 artifact。
//   - 产物是 versioned summary,带 source artifact 引用;原文永不删——
//     blob 与 session JSONL 照旧,模型觉得摘要不够可 context_read 回读。
//   - 输入永远从 blob 原文来(不拿摘要再摘要);确无原文可读时跳过,不做
//     derived_from_summary 的套娃(规格"不要反复拿摘要再摘要")。
//   - cheap 失败、超时、输出坏 JSON:工具回错,旧 L1 预览与原文都不动。
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agent/artifact_store.hpp"
#include "agent/loop.hpp"  // LoopBoundaryRecorder(Token 账本单 A1:旁路落账)
#include "agent/model_router.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"

namespace lubancode::agent {

// 单次按需摘要的预算旋钮。
struct MicrocompactOptions {
    // 单枚喂给 cheap 的原文上限(头尾各半;超了从行边界截,不劈码点)。
    std::size_t input_cap_bytes = 24 * 1024;
    // cheap 调用超时(秒)。
    int timeout_secs = 45;
    // Token 账本单 A1(旁路落账):一次 L2 请求一只轨迹桥;空 = 没接
    // 轨迹的会话/单测,行为与从前一致。purpose 固定 compact_map(点名
    // 一枚 artifact 的分块摘要,map 型小请求)。
    std::function<std::unique_ptr<LoopBoundaryRecorder>()> bypass_recorder;
};

// 一枚 L2 产物:versioned summary,source artifact 钉牢(规格 L2 节)。
struct MicrocompactSummary {
    std::string summary;        // 给模型看的摘要正文(Markdown 几行)
    std::vector<std::string> key_facts;   // 决定、错误码、路径、符号、命令、退出码
    std::string source_artifact_id;       // 原文的 artifact id(context_read 可回)
    std::string model;                    // 产出模型(看得见)
    // 输入是否是旧摘要(套娃标记):v1 输入永远来自 blob 原文,恒 false;
    // 将来确需从摘要重做时必须置真并在视图里写明(防一轮轮走样)。
    bool derived_from_summary = false;
};

// 解析 cheap 的严格 JSON 输出(容错:剥代码围栏、取首 { 到末个 })。
// 坏 JSON 给 nullopt,不删原文。
std::optional<MicrocompactSummary> ParseMicrocompactSummary(const std::string& text,
                                                            const std::string& artifact_id);

// 发一次 L2 请求(同步,带看门狗取消):input 永远取 blob 原文(头尾各半
// 截到 input_cap_bytes,行边界不劈码点)。失败只返回错误,旧消息不动。
// accounting 非空时记 usage/时长(分角色台账,cheap 档)。
std::expected<MicrocompactSummary, std::string> RunMicrocompact(
    api::Backend& backend, const std::string& model, const std::string& reasoning_effort,
    const ContextArtifactStore& store, const ArtifactRef& ref, const MicrocompactOptions& options,
    BackgroundCallAccounting* accounting = nullptr,
    const std::atomic<bool>* external_cancel = nullptr);

}  // namespace lubancode::agent
