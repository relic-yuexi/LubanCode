// JSON 序列化的窄边界兜底:nlohmann::json 的 dump() 遇到树里混着的非法
// UTF-8 字符串会抛 type_error.316,异常一旦穿透,整场会话就被顶层 catch
// 掐死(见 todos/工具输出非法UTF8导致会话退出.todo)。这里提供两个出口:
//
//   - FindInvalidUtf8Field:坏串长在树的哪个字段上,给错误信息和日志用,
//     只报路径,不报内容。
//   - DumpJsonSanitized:正常树直接 dump;有坏串的树先逐串过一遍
//     SanitizeExternalText 再 dump,出口保证是能重新解析的 JSON 文本。
//     会话 JSONL、录制件这些"落盘后还要被 /resume、起草器重新读"的地方
//     用它——宁可落盘内容被替换字符洗过,也不能落一行解不开的坏行。
//
// 走网络的请求体也用它兜底(三个 wire client 的窄 catch 里):回传错误
// 会把带病历史原样留在会话里,往后每回合必挂,会话等于砖死;改为响亮
// 记日志后清洗照发。编码关口(工具结果清洗、截短对齐码点边界)仍是
// 正道,这里只是最后兜底。
#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace lubancode::platform {

// 在 JSON 树里找第一处含非法 UTF-8 的字符串,返回它的字段路径(如
// "messages[2].content[0].content")。整棵树干净返回 nullopt。
std::optional<std::string> FindInvalidUtf8Field(const nlohmann::json& value);

// dump() 的窄边界兜底,见文件头。changed_out 非空时回报是否动过手脚,
// 调用方好记一笔日志。绝不抛异常。
std::string DumpJsonSanitized(const nlohmann::json& value, bool* changed_out = nullptr);

// 请求体 dump 抛异常时的人话错误消息:拼上库自己报的坏字节位置和
// FindInvalidUtf8Field 找到的字段路径。wire client 的窄 catch 拿它包成
// api::Error 返回——会话活着,错误看得见。
std::string DescribeDumpFailure(const nlohmann::json& body, const nlohmann::json::exception& error);

}  // namespace lubancode::platform
