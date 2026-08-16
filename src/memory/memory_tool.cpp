#include "memory/memory_tool.hpp"

#include <string>

namespace lubancode::memory {

std::string MemorySaveTool::name() const {
    return "memory_save";
}

std::string MemorySaveTool::description() const {
    return "把一条小而稳定的项目事实、用户明确偏好或用户明说的行事纠正排进后台记忆(正式入库,不经待审区)。"
           "只在信息已经由源码、工具结果或用户明说证实时调用;fact 必须在 paths 或 evidence 里"
           "给出可核验证据;feedback 只收用户当场明说的纠正(如版本节奏、验收习惯),confidence 须"
           "user-stated,模型推断不得直写。不要保存当前任务进度、猜测、日志、网页/MCP 原文、密钥或个人数据。"
           "已有同主题时沿用索引里的 id 做更新。自动候选走回合总结,不经过这个工具。";
}

nlohmann::json MemorySaveTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties",
         {
             {"kind", {{"type", "string"}, {"enum", {"fact", "preference", "feedback"}},
                       {"description", "fact=可核验的项目事实；preference=用户明确说出的本项目偏好；"
                                       "feedback=用户明说的行事纠正(须 user-stated)"}}},
             {"id", {{"type", "string"}, {"description", "可选。更新已有记忆时用索引里的稳定 id"}}},
             {"title", {{"type", "string"}, {"description", "一个可独立更新的短主题"}}},
             {"summary", {{"type", "string"}, {"description", "索引里的一行摘要"}}},
             {"content", {{"type", "string"}, {"description", "精炼正文，写事实、证据与注意事项，不抄大段源码"}}},
             {"keywords", {{"type", "array"}, {"items", {{"type", "string"}}},
                           {"description", "函数名、类名、命令等精确检索词，最多 16 项"}}},
             {"paths", {{"type", "array"}, {"items", {{"type", "string"}}},
                        {"description", "支撑事实的项目内相对路径，最多 24 项；fact 必填至少一项"}}},
             {"confidence", {{"type", "string"}, {"enum", {"user-stated", "verified", "inferred"}},
                             {"description", "user-stated=用户明说的偏好；verified=已核验的事实；"
                                             "inferred=推断(只该出现在待审候选，不该走本工具)"}}},
             {"scope", {{"type", "object"},
                        {"properties",
                         {
                             {"kind", {{"type", "string"}, {"enum", {"project", "subtree", "path"}},
                                       {"description", "记忆适用的范围；subtree/path 须配 value"}}},
                             {"value", {{"type", "string"}, {"description", "项目内相对路径(subtree/path 时必填)"}}},
                         }},
                        {"description", "可选。当前工作目录不在范围内时不注入，防串味"}}},
             {"evidence", {{"type", "array"},
                           {"items",
                            {{"type", "object"},
                             {"properties",
                              {
                                  {"path", {{"type", "string"}, {"description", "项目内相对路径"}}},
                                  {"symbol", {{"type", "string"}, {"description", "可选:函数/类/配置键"}}},
                              }},
                             {"required", {"path"}}}},
                           {"description", "可选。可核验证据，最多 24 项；fact 建议给出"}}},
             {"expires_at", {{"type", "string"},
                             {"description", "可选。临时规约的到期日(YYYY-MM-DD 或 ISO 时间);到期后不再召回"}}},
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
