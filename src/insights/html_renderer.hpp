// /insights 的 HTML renderer(Token 账本单 §9.5/§9.6/A5)。
//
// 自包含单文件:CSS/JS 全内联,不开网络、不拉 CDN;CSP 钉
// default-src 'none',内联样式与脚本带本地 sha256(base64)哈希;所有
// 动态文本过 HTML escape;prompt 正文/工具入参/结果全文/绝对路径一律
// 不进 DOM(§9.6)。七节齐:概览/Token/摩擦之前的 prompt 构成/摩擦/
// 交互形状/建议/覆盖与限制,外加可筛的场次明细表。
//
// 同一份 typed report 进终端与 HTML(§11.1),不各算一套。
#pragma once

#include <string>

#include "insights/report_model.hpp"
#include "insights/workspace_aggregator.hpp"

namespace lubancode::insights {

// 渲染(纯函数,零 IO)。同输入同输出——generated_at 由调用方注入,
// 测试注固定值即得字节稳定的 golden。
std::string RenderInsightsHtml(const InsightsReport& report,
                               const WorkspaceAggregate& aggregate,
                               const InsightsRenderExtras& extras);

// renderer 自检(/doctor insights 用):拿一份带恶意样本文本的报告渲染,
// 验 CSP 头、七节锚点与转义都过。返回空串 = 过;否则是没过的条目说明。
std::string InsightsHtmlSelfCheck();

}  // namespace lubancode::insights
