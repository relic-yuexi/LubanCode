#include "tools/tool_search.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace lubancode::tools {

namespace {

// 只小写 ASCII 字母,UTF-8 多字节序列(>= 0x80)原样不动——中文描述照常
// 按字节子串匹配,不会被"转小写"搅坏编码。
std::string AsciiLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// query 按空白切词,顺手小写。
std::vector<std::string> Tokenize(const std::string& query) {
    std::vector<std::string> tokens;
    std::istringstream iss(query);
    std::string word;
    while (iss >> word) {
        tokens.push_back(AsciiLower(word));
    }
    return tokens;
}

// UTF-8 按码点截断到 max_chars 个字符,截了补一个省略号。不完整字节序列
// 不会被拦腰斩——只在码点边界(非 0b10xxxxxx 起始字节)上数。
std::string TruncateUtf8(const std::string& s, std::size_t max_chars) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while (pos < s.size()) {
        if (count == max_chars) {
            return s.substr(0, pos) + "…";
        }
        const auto byte = static_cast<unsigned char>(s[pos]);
        std::size_t len = 1;
        if ((byte & 0x80U) == 0x00U) {
            len = 1;
        } else if ((byte & 0xE0U) == 0xC0U) {
            len = 2;
        } else if ((byte & 0xF0U) == 0xE0U) {
            len = 3;
        } else if ((byte & 0xF8U) == 0xF0U) {
            len = 4;
        }
        pos = (std::min)(s.size(), pos + len);
        ++count;
    }
    return s;
}

// 描述里的换行压成空格——索引段一个工具占一行,描述里夹换行会把格式冲散。
std::string SingleLine(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c == '\n' || c == '\r' || c == '\t') {
            c = ' ';
        }
    }
    return out;
}

// 从 input_schema 拼一行参数概要:"a(number, 必填), b(number)"。schema 不像
// JSON Schema(没有 properties)就给"(无参数说明)"。
std::string SummarizeSchema(const nlohmann::json& schema) {
    if (!schema.is_object() || !schema.contains("properties") || !schema["properties"].is_object()) {
        return "(无参数说明)";
    }
    std::set<std::string> required;
    if (schema.contains("required") && schema["required"].is_array()) {
        for (const auto& r : schema["required"]) {
            if (r.is_string()) {
                required.insert(r.get<std::string>());
            }
        }
    }
    std::string out;
    for (const auto& [key, prop] : schema["properties"].items()) {
        if (!out.empty()) {
            out += ", ";
        }
        std::string type = "any";
        if (prop.is_object() && prop.contains("type") && prop["type"].is_string()) {
            type = prop["type"].get<std::string>();
        }
        out += key + "(" + type + (required.count(key) != 0 ? ", 必填" : "") + ")";
    }
    if (out.empty()) {
        return "(无参数)";
    }
    return out;
}

}  // namespace

std::string BuildDeferredToolsIndexSegment(const ToolRegistry& registry, const std::set<std::string>& loaded) {
    std::vector<const Tool*> pending;
    for (const auto& tool : registry.All()) {
        if (tool->deferred() && loaded.count(tool->name()) == 0) {
            pending.push_back(tool.get());
        }
    }
    if (pending.empty()) {
        return std::string();
    }
    std::string out = "另有 " + std::to_string(pending.size()) +
                      " 个延迟挂载的工具不在当前工具表里,需要先用 tool_search 按关键词检索、挂载后才能调用"
                      "(直接调用会报错)。索引如下(名字: 简述):\n";
    for (const Tool* tool : pending) {
        out += "- " + tool->name() + ": " + TruncateUtf8(SingleLine(tool->description()), 80) + "\n";
    }
    return out;
}

std::string ToolSearchTool::name() const {
    return "tool_search";
}

std::string ToolSearchTool::description() const {
    return "按关键词检索延迟挂载的工具(MCP/插件等外挂工具不直接进工具表,只在系统提示的索引段里露名字)。"
           "对工具名和描述做大小写不敏感的分词匹配,命中的工具立即挂载,本轮之后即可直接调用。"
           "当索引段里有你需要的能力、或怀疑有外挂工具能干这件事时,先用这个搜。";
}

nlohmann::json ToolSearchTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();

    nlohmann::json query_prop = nlohmann::json::object();
    query_prop["type"] = "string";
    query_prop["description"] = "关键词,空格分隔多个词;对延迟工具的名字和描述做大小写不敏感的子串匹配,"
                                "按命中词数排序。";
    properties["query"] = query_prop;

    nlohmann::json limit_prop = nlohmann::json::object();
    limit_prop["type"] = "integer";
    limit_prop["description"] = "最多返回并挂载几个,不填默认 5。";
    properties["limit"] = limit_prop;

    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"query"});

    return schema;
}

Tool::Result ToolSearchTool::execute(const nlohmann::json& input) {
    if (!input.contains("query") || !input.at("query").is_string()) {
        return {"缺少必填参数 query(字符串)", true};
    }
    const std::vector<std::string> tokens = Tokenize(input.at("query").get<std::string>());
    if (tokens.empty()) {
        return {"query 不能是空白字符串,给几个关键词(空格分隔)", true};
    }

    std::size_t limit = 5;
    if (const auto it = input.find("limit"); it != input.end() && !it->is_null()) {
        if (!it->is_number_integer() || it->get<int>() < 1) {
            return {"limit 得是正整数", true};
        }
        limit = static_cast<std::size_t>(it->get<int>());
    }

    // 候选:全部延迟工具(不管加载没加载——重复检索已挂载的工具无害,
    // 照常列出来,免得模型搜到一半以为工具没了)。按命中词数排序,
    // 同分保持注册顺序(stable_sort)。
    struct Hit {
        const Tool* tool = nullptr;
        std::size_t score = 0;
    };
    std::vector<Hit> hits;
    std::vector<const Tool*> deferred_tools;
    for (const auto& tool : registry_.All()) {
        if (!tool->deferred()) {
            continue;
        }
        deferred_tools.push_back(tool.get());
        const std::string haystack = AsciiLower(tool->name()) + "\n" + AsciiLower(tool->description());
        std::size_t score = 0;
        for (const auto& token : tokens) {
            if (haystack.find(token) != std::string::npos) {
                ++score;
            }
        }
        if (score > 0) {
            hits.push_back({tool.get(), score});
        }
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    if (hits.size() > limit) {
        hits.resize(limit);
    }

    if (hits.empty()) {
        // 近似建议兜底:名字前缀匹配(工具名常是 mcp__server__tool 这种长串,
        // 也拿 token 反向试——名字是 token 的前缀,或 token 是名字的前缀)。
        std::vector<const Tool*> suggestions;
        for (const Tool* tool : deferred_tools) {
            const std::string lower_name = AsciiLower(tool->name());
            for (const auto& token : tokens) {
                if (lower_name.compare(0, token.size(), token) == 0 ||
                    token.compare(0, lower_name.size(), lower_name) == 0) {
                    suggestions.push_back(tool);
                    break;
                }
            }
        }
        std::string out = "没有命中任何延迟工具。";
        if (!suggestions.empty()) {
            out += "名字相近的有:\n";
            for (const Tool* tool : suggestions) {
                out += "- " + tool->name() + ": " + TruncateUtf8(SingleLine(tool->description()), 80) + "\n";
            }
            out += "换这些名字里的词再搜一次即可挂载。";
        } else if (deferred_tools.empty()) {
            out += "当前没有任何延迟工具可搜。";
        } else {
            out += "换个关键词试试(共 " + std::to_string(deferred_tools.size()) +
                   " 个延迟工具,系统提示的索引段里有全部名字)。";
        }
        return {out, false};
    }

    std::string out = "命中 " + std::to_string(hits.size()) + " 个工具:\n";
    for (const auto& hit : hits) {
        loaded_->insert(hit.tool->name());
        out += "- " + hit.tool->name() + ": " + SingleLine(hit.tool->description()) + "\n";
        out += "  参数: " + SummarizeSchema(hit.tool->input_schema()) + "\n";
    }
    out += "以上工具已挂载,可直接调用。";
    return {out, false};
}

}  // namespace lubancode::tools
