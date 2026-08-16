// schema 3 主题的 YAML front matter。读写都交给 yaml-cpp,不手搓解析器;
// 字段顺序、缩进与引号策略固定,连续两次 parse/write 字节稳定。parser 只
// 认文件开头第一对 ---,正文里的水平线不算 front matter 结束符。

#pragma once

#include <expected>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "memory/project_memory.hpp"

namespace lubancode::memory::frontmatter {

// 旧格式(schema 1/2)的元数据标记。真本在 Markdown 头部的 HTML 注释里,
// 新写一律走 front matter,这对标记只读不写。
constexpr std::string_view kLegacyMetaOpen = "<!-- lubancode-memory\n";
constexpr std::string_view kLegacyMetaClose = "\n-->";

// Parse 的产物:公开条目 + 指纹表 + 分隔线之后的正文原样(未 trim)。
// 标题取正文首个一级标题,没有则退回 name/id。entry.schema 恒为 3。
struct ParsedTopic {
    MemoryEntry entry;
    nlohmann::json fingerprints = nlohmann::json::object();
    std::string body;
};

// 解析一份 schema 3 主题全文。text 须以单独一行 --- 开头。YAML 坏、
// metadata.schema 不是 3、必填字段缺,返回错误。时间一律按字符串读
// (node 原文),不受 YAML 隐式类型与本机时区牵扯。
std::expected<ParsedTopic, std::string> Parse(const std::string& text);

// 组整份主题文本:front matter(含首尾 ---)+ 空行 + "# 标题" + 空行 +
// 正文。entry.paths 与 entry.evidence 合并后进 evidence(schema 3 没有
// 独立的 paths 字段,老条目改写时路径全部升格为证据)。
std::string BuildTopicText(const MemoryEntry& entry, const nlohmann::json& fingerprints,
                           const std::string& body);

// 剥掉元数据头(schema 1/2 的 HTML 注释或 schema 3 的 front matter),
// 只留正文;不认得的格式原样返回。
std::string StripTopicMetadata(const std::string& text);

// 再把正文开头的一级标题行剥掉(写回时 writer 会按 entry.title 重写标
// 题,留着会在核验/迁移路径里翻倍)。
std::string StripTitleHeading(const std::string& body);

}  // namespace lubancode::memory::frontmatter
