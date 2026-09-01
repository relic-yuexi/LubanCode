#include "memory/memory_tool.hpp"

#include <string>

#include "tools/tool_text.hpp"  // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/

namespace lubancode::memory {

std::string MemorySaveTool::name() const {
    return "memory_save";
}

std::string MemorySaveTool::description() const {
    // 文案在 src/prompts/tools/<语言>/memory_save.md,兜底是迁移前的原文。
    return tools::ToolText("memory_save", "description",
                    "把一条小而稳定的项目事实、用户明确偏好或用户明说的行事纠正排进后台记忆(正式入库,不经待审区)。"
                    "只在信息已经由源码、工具结果或用户明说证实时调用;fact 必须在 paths 或 evidence 里"
                    "给出可核验证据;feedback 只收用户当场明说的纠正(如版本节奏、验收习惯),confidence 须"
                    "user-stated,模型推断不得直写。不要保存当前任务进度、猜测、日志、网页/MCP 原文、密钥或个人数据。"
                    "已有同主题时沿用索引里的 id 做更新。自动候选走回合总结,不经过这个工具。");
}

nlohmann::json MemorySaveTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties",
         {
             {"kind", {{"type", "string"}, {"enum", {"fact", "preference", "feedback"}},
                       {"description", tools::ToolText("memory_save", "param.kind",
                                                "fact=可核验的项目事实；preference=用户明确说出的本项目偏好；"
                                                "feedback=用户明说的行事纠正(须 user-stated)")}}},
             {"id", {{"type", "string"},
                     {"description", tools::ToolText("memory_save", "param.id", "可选。更新已有记忆时用索引里的稳定 id")}}},
             {"title", {{"type", "string"},
                        {"description", tools::ToolText("memory_save", "param.title", "一个可独立更新的短主题")}}},
             {"summary", {{"type", "string"},
                          {"description", tools::ToolText("memory_save", "param.summary", "索引里的一行摘要")}}},
             {"content",
              {{"type", "string"},
               {"description", tools::ToolText("memory_save", "param.content",
                                        "精炼正文，写事实、证据与注意事项，不抄大段源码")}}},
             {"keywords", {{"type", "array"}, {"items", {{"type", "string"}}},
                           {"description", tools::ToolText("memory_save", "param.keywords",
                                                    "函数名、类名、命令等精确检索词，最多 16 项")}}},
             {"paths", {{"type", "array"}, {"items", {{"type", "string"}}},
                        {"description", tools::ToolText("memory_save", "param.paths",
                                                 "支撑事实的项目内相对路径，最多 24 项；fact 必填至少一项")}}},
             {"confidence", {{"type", "string"}, {"enum", {"user-stated", "verified", "inferred"}},
                             {"description", tools::ToolText("memory_save", "param.confidence",
                                                      "user-stated=用户明说的偏好；verified=已核验的事实；"
                                                      "inferred=推断(只该出现在待审候选，不该走本工具)")}}},
             {"scope", {{"type", "object"},
                        {"properties",
                         {
                             {"kind",
                              {{"type", "string"},
                               {"enum", {"project", "subtree", "path", "user"}},
                               {"description",
                                tools::ToolText("memory_save", "param.scope.kind",
                                         "记忆适用的范围；subtree/path 须配 value；"
                                         "user=跨项目用户记忆(仅 preference/feedback，"
                                         "不得带项目路径证据，须全局授权 memory.user_enabled)")}}},
                             {"value",
                              {{"type", "string"},
                               {"description", tools::ToolText("memory_save", "param.scope.value",
                                                        "项目内相对路径(subtree/path 时必填)")}}},
                         }},
                        {"description", tools::ToolText("memory_save", "param.scope",
                                                 "可选。当前工作目录不在范围内时不注入，防串味")}}},
             {"evidence", {{"type", "array"},
                           {"items",
                            {{"type", "object"},
                             {"properties",
                              {
                                  {"path", {{"type", "string"},
                                            {"description", tools::ToolText("memory_save", "param.evidence.path",
                                                                     "项目内相对路径")}}},
                                  {"symbol", {{"type", "string"},
                                              {"description", tools::ToolText("memory_save", "param.evidence.symbol",
                                                                       "可选:函数/类/配置键")}}},
                              }},
                             {"required", {"path"}}}},
                           {"description", tools::ToolText("memory_save", "param.evidence",
                                                    "可选。可核验证据，最多 24 项；fact 建议给出")}}},
             {"expires_at", {{"type", "string"},
                             {"description", tools::ToolText("memory_save", "param.expires_at",
                                                      "可选。临时规约的到期日(YYYY-MM-DD 或 ISO 时间);"
                                                      "到期后不再召回")}}},
         }},
        {"required", {"kind", "title", "summary", "content"}},
    };
}

tools::Tool::Result MemorySaveTool::execute(const nlohmann::json& input) {
    if (!memory_->generate_enabled()) return {"本场记忆写入未开启", true};
    if (!input.is_object()) return {"memory_save 参数必须是 object", true};
    for (const char* field : {"kind", "title", "summary", "content"}) {
        if (!input.contains(field) || !input[field].is_string()) {
            return {std::string("memory_save 缺字符串字段 ") + field, true};
        }
    }
    auto kind = ParseMemoryKind(input["kind"].get<std::string>());
    if (!kind.has_value()) return {kind.error(), true};
    SaveRequest request;
    request.kind = *kind;
    request.id = input.value("id", std::string());
    request.title = input["title"].get<std::string>();
    request.summary = input["summary"].get<std::string>();
    request.content = input["content"].get<std::string>();
    request.confidence = input.value("confidence", std::string());
    request.expires_at = input.value("expires_at", std::string());
    if (input.contains("scope") && input["scope"].is_object()) {
        request.scope.kind = input["scope"].value("kind", std::string("project"));
        request.scope.value = input["scope"].value("value", std::string());
        if (request.scope.kind == "user") request.scope.level = "user";
    }
    // 存储 v2 P0-4(§6.1):memory_save 的 scope 只能是 project——模型不得
    // 自行提交全局记忆。想升为 global 的,只该在结论里建议用户走
    // /memory remember global(须逐次确认)。
    if (request.scope.level == "user") {
        return {"memory.global_unauthorized: 全局记忆不接受模型工具直写。"
                "如确属跨项目偏好,请在回复里建议用户执行 /memory remember global <kind> 标题 :: 正文,"
                "由用户确认后入库。",
                true};
    }
    if (input.contains("evidence") && input["evidence"].is_array()) {
        for (const auto& item : input["evidence"]) {
            if (!item.is_object()) return {"evidence 每项必须是带 path 的 object", true};
            MemoryEvidence evidence;
            evidence.path = item.value("path", std::string());
            evidence.symbol = item.value("symbol", std::string());
            if (evidence.path.empty()) return {"evidence 每项必须有 path", true};
            request.evidence.push_back(std::move(evidence));
        }
    }
    for (const char* field : {"keywords", "paths"}) {
        if (!input.contains(field)) continue;
        if (!input[field].is_array()) return {std::string(field) + " 必须是字符串数组", true};
        std::vector<std::string>& target = std::string(field) == "keywords" ? request.keywords : request.paths;
        for (const auto& item : input[field]) {
            if (!item.is_string()) return {std::string(field) + " 必须是字符串数组", true};
            target.push_back(item.get<std::string>());
        }
    }
    auto queued = memory_->EnqueueSave(request);
    if (!queued.has_value()) return {queued.error(), true};
    return {"记忆已排进后台队列: " + *queued, false};
}

}  // namespace lubancode::memory
