#include "tools/context_tools.hpp"

#include <algorithm>
#include <utility>

namespace lubancode::tools {

namespace {

std::string NoStoreText() {
    return "当前会话还没有可追回的 artifact(尚无超长工具结果落盘)。";
}

std::string NotFoundText(const std::string& artifact_id) {
    return "找不到 artifact_id=\"" + artifact_id +
           "\"。只认当前会话落盘的 artifact,请在 [artifact aNNNN ...] 标记里取 id。";
}

}  // namespace

ContextSearchTool::ContextSearchTool(std::shared_ptr<lubancode::agent::ContextArtifactStore> store)
    : store_(std::move(store)) {}

std::string ContextSearchTool::description() const {
    return "在先前工具输出的落盘全文(artifact)里按关键词检索。工具结果太长时,请求里只留"
           "[artifact aNNNN ...] 引用(头尾预览);预览不够就用本工具搜全文,拿命中行号与块 id,"
           "再用 context_read 读出上下文。不可把预览的省略号当全文。";
}

nlohmann::json ContextSearchTool::input_schema() const {
    return nlohmann::json{
        {"type", "object"},
        {"properties",
         {
             {"artifact_id",
              {{"type", "string"}, {"description", "[artifact aNNNN ...] 标记里的 aNNNN"}}},
             {"query", {{"type", "string"}, {"description", "关键词(ASCII 大小写不敏感,中文按原文)"}}},
             {"max_results",
              {{"type", "integer"}, {"minimum", 1}, {"maximum", 32}, {"description", "最多回几条命中(默认 8)"}}},
         }},
        {"required", {"artifact_id", "query"}},
    };
}

tools::Tool::Result ContextSearchTool::execute(const nlohmann::json& input) {
    if (store_ == nullptr || !store_->active()) {
        return {NoStoreText(), true};
    }
    const std::string artifact_id = input.value("artifact_id", std::string());
    const std::string query = input.value("query", std::string());
    const int max_results = input.value("max_results", 8);
    if (artifact_id.empty() || query.empty()) {
        return {"artifact_id 与 query 都必填。", true};
    }
    const auto* ref = store_->Find(artifact_id);
    if (ref == nullptr) {
        return {NotFoundText(artifact_id), true};
    }
    std::string error;
    const auto hits = store_->Search(*ref, query, max_results, &error);
    if (!hits.has_value()) {
        return {error, true};
    }
    if (hits->empty()) {
        return {"artifact " + artifact_id + "(" + ref->tool_name + "," +
                std::to_string(ref->lines) + " 行)里没有命中 \"" + query + "\"。", false};
    }
    std::string out = "artifact " + artifact_id + "(" + ref->tool_name + ",共 " + std::to_string(ref->lines) +
                      " 行)命中 " + std::to_string(hits->size()) + " 行(按命中次数排序):\n";
    for (const auto& hit : *hits) {
        out += "  行 " + std::to_string(hit.line) +
               (hit.chunk_id.empty() ? std::string() : "(块 " + hit.chunk_id + ")") + ": " + hit.snippet + "\n";
    }
    out += "用 context_read(artifact_id, chunk_id 或 line_start+line_count)读出所需段落。";
    return {out, false};
}

ContextReadTool::ContextReadTool(std::shared_ptr<lubancode::agent::ContextArtifactStore> store)
    : store_(std::move(store)) {}

std::string ContextReadTool::description() const {
    return "按稳定 id 读先前工具输出落盘全文(artifact)的一段:给 chunk_id(context_search 命中给的)"
           "或 line_start(1 起)+line_count。单次最多 32 KiB,超了会拒绝并给可用范围。"
           "全文真本按 sha256 校验,hash 不合的内容不会被供给。";
}

nlohmann::json ContextReadTool::input_schema() const {
    return nlohmann::json{
        {"type", "object"},
        {"properties",
         {
             {"artifact_id",
              {{"type", "string"}, {"description", "[artifact aNNNN ...] 标记里的 aNNNN"}}},
             {"chunk_id", {{"type", "string"}, {"description", "块 id(如 c0003);给了就按块读"}}},
             {"line_start", {{"type", "integer"}, {"minimum", 1}, {"description", "起始行(1 起;与 chunk_id 二选一)"}}},
             {"line_count",
              {{"type", "integer"}, {"minimum", 0}, {"description", "读几行;0 = 读到结尾"}}},
         }},
        {"required", {"artifact_id"}},
    };
}

tools::Tool::Result ContextReadTool::execute(const nlohmann::json& input) {
    if (store_ == nullptr || !store_->active()) {
        return {NoStoreText(), true};
    }
    const std::string artifact_id = input.value("artifact_id", std::string());
    const std::string chunk_id = input.value("chunk_id", std::string());
    const long long line_start = input.value("line_start", 0LL);
    const long long line_count = input.value("line_count", 0LL);
    if (artifact_id.empty()) {
        return {"artifact_id 必填。", true};
    }
    const auto* ref = store_->Find(artifact_id);
    if (ref == nullptr) {
        return {NotFoundText(artifact_id), true};
    }
    if (chunk_id.empty() && line_start < 1) {
        return {"chunk_id 与 line_start 至少给一个(line_start 从 1 起)。", true};
    }
    const auto result = store_->Read(*ref, chunk_id, static_cast<std::size_t>(line_start),
                                     static_cast<std::size_t>(std::max<long long>(line_count, 0)));
    if (!result.ok) {
        std::string out = result.error;
        if (!result.available.empty()) {
            out += "。" + result.available;
        }
        return {out, true};
    }
    std::string header = "artifact " + artifact_id + "(" + ref->tool_name + ")第 " +
                         std::to_string(result.line_start) + "-" +
                         std::to_string(result.line_start + result.line_count - 1) + " 行" +
                         (result.chunk_id.empty() ? std::string() : "(块 " + result.chunk_id + ")") + ":\n";
    return {header + result.text, false};
}

}  // namespace lubancode::tools
