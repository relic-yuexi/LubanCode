#include "memory/memory_tool.hpp"

#include <string>

namespace lubancode::memory {

std::string MemorySaveTool::name() const {
    return "memory_save";
}

std::string MemorySaveTool::description() const {
    return "把一条小而稳定的项目事实或用户明确偏好排进后台记忆。只在信息已经由源码、工具结果或用户明说证实时调用；"
           "不要保存当前任务进度、猜测、日志、网页/MCP 原文、密钥或个人数据。已有同主题时沿用索引里的 id 做更新。";
}

nlohmann::json MemorySaveTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties",
         {
             {"kind", {{"type", "string"}, {"enum", {"fact", "preference"}},
                       {"description", "fact=可核验的项目事实；preference=用户明确说出的本项目偏好"}}},
             {"id", {{"type", "string"}, {"description", "可选。更新已有记忆时用索引里的稳定 id"}}},
             {"title", {{"type", "string"}, {"description", "一个可独立更新的短主题"}}},
             {"summary", {{"type", "string"}, {"description", "索引里的一行摘要"}}},
             {"content", {{"type", "string"}, {"description", "精炼正文，写事实、证据与注意事项，不抄大段源码"}}},
             {"keywords", {{"type", "array"}, {"items", {{"type", "string"}}},
                           {"description", "函数名、类名、命令等精确检索词，最多 16 项"}}},
             {"paths", {{"type", "array"}, {"items", {{"type", "string"}}},
                        {"description", "支撑事实的项目内相对路径，最多 24 项"}}},
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
