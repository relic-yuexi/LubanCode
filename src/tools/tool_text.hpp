// 工具文案查表(模型可见文字按语言分档):工具描述与 schema 参数说明
// 不再焊死在 C++ 里,源头是 src/prompts/tools/<语言码>/<工具名>.md,构建期
// 由 cmake/embed_tool_text.cmake 嵌进 embedded_tool_text.hpp 的 kAllKeys,
// 这里首次用时装载一次成全局查表。
//
// 查表规矩三句话说清:
//   ToolText(tool, key, fallback) 按当前会话语言查(cli::CurrentLanguage,
//   即 LUBANCODE_LANG / config.language → 内置 zh-CN/en → 外部语言包那条
//   选择链);当前语言没有 → zh-CN → 兜底字串(调用方带着的 C++ 原文案,
//   迁移期保底——工具还没搬进数据文件时行为不变)。
//   description()/input_schema() 每次调用现查,语言切换天然即时生效。
//
// 键名:工具名(name() 那个,如 "read_file") + 键("description" 或
// "param.<参数路径>")。cli/i18n 是零依赖叶子,tools 引它不算反向依赖
// (它明言"发给模型的不走这里",这里只借它的语言选择链,不借它的文案表)。
//
// 线程注意:装载有锁,查表只读,读线程安全。

#pragma once

#include <string>
#include <string_view>

namespace lubancode::tools {

// 查一条工具文案:先当前语言,再 zh-CN,都没有用 fallback(调用方的
// C++ 原文案兜底)。fallback 为空串且表里也没有时,返回空串——调用方
// 自己看着办(不该发生,防御而已)。
std::string ToolText(std::string_view tool, std::string_view key, std::string_view fallback = {});

}  // namespace lubancode::tools
