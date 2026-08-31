// 会话标题的两层生成(实测问题 7 修复):
//   第一层 本地临时标题:首问一到、建档即成——清洗首行、截到合宜长度,
//   零模型 token,当场可见(LocalSessionTitle)。
//   第二层 cheap 精炼:只在配置了独立 cheap 路由时由 SessionTitleRefiner
//   异步并行发(session_title_refiner.hpp),结果原子替换临时标题;
//   失败保留本地标题,不重试,不回落 normal。
// 本文件只放纯函数:本地标题派生、精炼请求的拼装与采样(采样本体走
// agent::SampleModel 原语)。失败降级:调用方沿用本地标题,不许因起名
// 失败拦住会话。
#pragma once

#include <atomic>
#include <cstddef>
#include <expected>
#include <string>

#include "agent/loop.hpp"  // LoopBoundaryRecorder(Token 账本单 A1:旁路落账口)
#include "agent/model_router.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"

namespace lubancode::app {

// 精炼的硬超时(秒):看门狗到点拉取消旗。单子预算 3-5 秒,取上限再留
// 一分慢网余量——超了就保留本地标题,不为十几个 token 等下去。
inline constexpr int kTitleRefineTimeoutSecs = 5;

// 精炼请求的输出上限(单子预算:标题十几个字,不预留 100 tokens)。
inline constexpr int kTitleRefineMaxTokens = 24;

// 第一层:拿首问派生本地临时标题。只取首行,压空白、剥围栏与首尾引号、
// 按 UTF-8 码点截到 max_chars(绝不从码点中腰劈开)。没有可用的字给空串
// (调用方当"没起出来"处理,/sessions 回退首句摘要)。
std::string LocalSessionTitle(const std::string& first_query, std::size_t max_chars = 24);

// 第二层:拿首问(截段 ≤600 UTF-8 字节,只喂首问,不喂回复与工具历史)
// 发一枚无工具的短采样,把标题精炼成十几个字。max_tokens=kTitleRefineMax
// Tokens;reasoning_effort 为空时自带最低档"low"(单子:reasoning 关或
// 最低)。timeout_secs 到点或 cancel 被拉起就收手——失败只回错误,调用方
// 保留本地标题。accounting 半截也出账(旧口径:先记账再判错)。
std::expected<std::string, std::string> RefineSessionTitle(lubancode::api::Backend& backend,
                                                           const std::string& model,
                                                           const std::string& reasoning_effort,
                                                           const std::string& first_query, int timeout_secs = 0,
                                                           const std::atomic<bool>* cancel = nullptr,
                                                           lubancode::agent::BackgroundCallAccounting* accounting = nullptr,
                                                           lubancode::agent::LoopBoundaryRecorder* boundary_recorder = nullptr);

// 标题清洗(纯函数,单测钉):剥代码围栏与首尾引号、压连续空白成单空格、
// 限 max_chars 个 UTF-8 码点(绝不从码点中腰劈开)、剥两端空白。全空给
// 空串(调用方当"没起出来"处理)。
std::string SanitizeTitle(const std::string& raw, std::size_t max_chars = 24);

}  // namespace lubancode::app
