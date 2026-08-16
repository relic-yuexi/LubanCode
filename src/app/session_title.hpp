// 会话标题与 resume 摘要的 cheap 生成(模型分工第一期):起标题/续聊点名
// 这类后台小活走 cheap_model(路由由调用方经 ModelRouterService 决定,这里
// 只收 backend + model + effort 三件)。失败降级:调用方沿用现状的首句摘要
// 兜底,不许因起名失败拦住会话。
#pragma once

#include <string>
#include <vector>

#include "agent/model_router.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"

namespace lubancode::app {

// 拿会话开头几条消息起一枚短标题(走 cheap 路由)。失败只返回错误,调用方
// 降级——标题不是关键路径。timeout_secs 到点拉取消旗。
std::expected<std::string, std::string> GenerateSessionTitle(lubancode::api::Backend& backend,
                                                             const std::string& model,
                                                             const std::string& reasoning_effort,
                                                             const std::vector<lubancode::api::Message>& head,
                                                             int timeout_secs = 30,
                                                             lubancode::agent::BackgroundCallAccounting* accounting = nullptr);

// 标题清洗(纯函数,单测钉):剥代码围栏与首尾引号、压连续空白成单空格、
// 限 max_chars 个 UTF-8 码点(绝不从码点中腰劈开)、剥两端空白。全空给
// 空串(调用方当"没起出来"处理)。
std::string SanitizeTitle(const std::string& raw, std::size_t max_chars = 24);

}  // namespace lubancode::app
