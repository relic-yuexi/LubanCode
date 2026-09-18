// Kanban 看板单:`lubancode kanban` 的自包含 HTML 渲染器。
//
// 纯函数:吃一枚已经备好的 JSON(项目 + 会话摘要),吐一份零外链的
// HTML(CSS/JS 全内联,数据以 <script> 内联 JSON 嵌入),由调用方落盘并
// 打开浏览器。照 insights/html_renderer 的"渲染纯函数 + 落盘"模式,但
// 页面交互(看板列、搜索、详情弹层)全在前端 JS 做,渲染器本身无状态。
//
// 注入安全:数据一律经 JSON 序列化后嵌入,并把 "</" 折成 "<\/" 防
// </script> 逃逸;前端渲染全程走 textContent,不把数据拼进 HTML。
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace lubancode::kanban {

// data 形状(键全 snake_case,前端按此读):
//   generated_at_ms: number
//   version: string
//   diagnostic: string(可空;非空时页顶示警)
//   projects: [{key,name,identity_kind,identity_root,created_at_ms,
//               last_opened_at_ms,checkouts:[{root,first_seen_at_ms,
//               last_seen_at_ms}]}]
//   sessions: [{id,workspace_key,status,archived,run_kind,
//               run_kind_unknown,title,first_user_text,cwd,model,
//               created_at_ms,updated_at_ms,message_count,event_count,
//               damaged}]
std::string RenderKanbanHtml(const nlohmann::json& data);

}  // namespace lubancode::kanban
