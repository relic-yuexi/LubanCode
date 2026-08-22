// Workflow YAML 解析器(自然语言编排单第 1 批):workflow.yaml -> AST。
//
// 只认 yaml-cpp 一条路,不手搓缩进。所有错误带位置(行号)与人话,校验
// 错误须落到人看得懂的位置——parser 管得住的(YAML 形状、字段类型、
// 枚举名)在这里报;引用、可达性、能力那类要全图的归 validator。
//
// 路径规矩(单子"文件与安装范围"):prompt/template/task 一类包内引用,
// 一律相对 workflow 目录根解析,拒绝 ".." 越界与绝对路径。这里做纯字符串
// 检查(IsSafePackageRelative),磁盘上有没有这个文件由 catalog/validator
// 查——parser 不碰盘。

#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "workflow/definition.hpp"

namespace lubancode::workflow {

// 解析错误:location 是 "workflow.yaml:42" 一类(行号 1 起);message 人话。
struct ParseIssue {
    std::string location;
    std::string message;
};

// 包内相对引用的守门:只认普通相对路径段(字母数字、-、_、. 与 /),
// 拒绝绝对路径、 ".." 越界、反斜杠、空段。这是纯字符串检查,不碰盘。
bool IsSafePackageRelative(const std::string& ref);

// "10m" / "90s" / "1h" / "600" -> 秒。认不得返回 nullopt。
std::optional<long long> ParseDurationSecs(const std::string& text);

// YAML 文本 -> 定义。失败给 ParseIssue 列表(至少一条)。同样文本在
// 任何平台解析出同一份 AST(字段顺序按 YAML 出现序)。
std::expected<WorkflowDefinition, std::vector<ParseIssue>> ParseWorkflowYaml(const std::string& yaml_text);

// 读 workflow.yaml(须已确认存在)并解析;读失败折成 ParseIssue。
std::expected<WorkflowDefinition, std::vector<ParseIssue>> LoadWorkflowDefinition(
    const std::filesystem::path& workflow_yaml);

// 定义 -> workflow.yaml 文本(创建向导落盘用)。带 schema_version 与
// 归一化字段序;round-trip 后 ContentHash 不变由单测钉。
std::string EmitWorkflowYaml(const WorkflowDefinition& def);

}  // namespace lubancode::workflow
