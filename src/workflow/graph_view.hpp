// 图渲染(自然语言编排单第 1 批):ASCII 与 Mermaid 均由纯图数据生成。
//
// 两份输出都从 WorkflowDefinition 现算,不落盘、不缓存——展示层随便重画。
// ASCII 树用于终端 /workflow graph;Mermaid 给 Web/文档。节点用
// id(kind) 标,边按 outcome 分组;parallel 的 branches 画成展开的扇形。

#pragma once

#include <string>

#include "workflow/definition.hpp"

namespace lubancode::workflow {

// ASCII 树形图:entry 起头,深度优先,并行分支缩进并列。宽度按 max_width
// 软包(超长不硬截,保证可读)。
std::string RenderAsciiGraph(const WorkflowDefinition& def, std::size_t max_width = 80);

// Mermaid flowchart 文本(TAB 缩进,子图不嵌套,frontends 可直接喂)。
std::string RenderMermaidGraph(const WorkflowDefinition& def);

// /workflow show 的一行摘要:kind + 关键领域字段(tool 名/prompt 路径/
// 分支数),不重排版。
std::string NodeSummaryLine(const WorkflowNode& node);

}  // namespace lubancode::workflow
