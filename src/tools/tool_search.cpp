#include "tools/tool_search.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

#include "tools/tool_text.hpp"  // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/

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

// P0(动态工具 PromptCache 守恒单·§十三)起 trace 展示位:details 走
// Tool::Result 已有的结构化诊断字段(逐枚追踪单),ExecutionFinished 栅栏
// 原样透传成 ToolTraceEvent.details,/trace 与 export 都看得到,不改
// content 正文、不改任何执行判断。P1 起 proxy 构造的实例标 proxy_reference,
// legacy 构造照旧标 legacy_expand——模式是构造时定的,不每调现猜。
Tool::Result MakeSearchResult(std::string text, bool is_error, const char* mode) {
    Tool::Result result(std::move(text), is_error);
    result.details["deferred_tool_mode"] = mode;
    return result;
}

// 单项 schema 的安全展开上限(proxy 路):超了报 schema_too_large,不铸
// ref、不悄悄截断(单子 §5.3)。延迟工具本就是外挂大 schema 的主力,
// 32 KiB 已是单条 tool result 里能安全摊的量级;更大的该走专用方案。
constexpr std::size_t kMaxDiscoverySchemaBytes = 32 * 1024;

// 来源标签(与 agent 侧 ToolSourceKind 的 ToString 同一张表;tools 层不引
// agent 头,这里照抄映射,新来源两边一起加)。
std::string SourceKindLabel(ToolSourceKind kind) {
    switch (kind) {
        case ToolSourceKind::Builtin: return "builtin";
        case ToolSourceKind::Mcp: return "mcp";
        case ToolSourceKind::Lsp: return "lsp";
        case ToolSourceKind::PluginLua: return "plugin-lua";
        case ToolSourceKind::PluginNative: return "plugin-native";
        case ToolSourceKind::Agent: return "agent";
        case ToolSourceKind::Ptc: return "ptc";
        case ToolSourceKind::Deferred: return "deferred";
    }
    return "builtin";
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
    // 文案在 src/prompts/tools/<语言>/tool_search.md,兜底是迁移前的原文。
    return ToolText("tool_search", "description",
                    "按关键词检索延迟挂载的工具(MCP/插件等外挂工具不直接进工具表,只在系统提示的索引段里露名字)。"
                    "对工具名和描述做大小写不敏感的分词匹配,命中的工具立即挂载,本轮之后即可直接调用。"
                    "当索引段里有你需要的能力、或怀疑有外挂工具能干这件事时,先用这个搜。");
}

nlohmann::json ToolSearchTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();

    nlohmann::json query_prop = nlohmann::json::object();
    query_prop["type"] = "string";
    query_prop["description"] =
        ToolText("tool_search", "param.query",
                 "关键词,空格分隔多个词;对延迟工具的名字和描述做大小写不敏感的子串匹配,"
                 "按命中词数排序。");
    properties["query"] = query_prop;

    nlohmann::json limit_prop = nlohmann::json::object();
    limit_prop["type"] = "integer";
    limit_prop["description"] = ToolText("tool_search", "param.limit", "最多返回并挂载几个,不填默认 5。");
    properties["limit"] = limit_prop;

    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"query"});

    return schema;
}

Tool::Result ToolSearchTool::execute(const nlohmann::json& input) {
    return resolver_ != nullptr ? ExecuteProxy(input) : ExecuteLegacy(input);
}

// ---------------------------------------------------------------------------
// legacy 路:命中写 loaded 集合,下一轮 schema 扩写回顶层 tools。兼容行为,
// P0 的现状回归册钉着,一字不动。
// ---------------------------------------------------------------------------
Tool::Result ToolSearchTool::ExecuteLegacy(const nlohmann::json& input) {
    if (!input.contains("query") || !input.at("query").is_string()) {
        return MakeSearchResult("缺少必填参数 query(字符串)", true, "legacy_expand");
    }
    const std::vector<std::string> tokens = Tokenize(input.at("query").get<std::string>());
    if (tokens.empty()) {
        return MakeSearchResult("query 不能是空白字符串,给几个关键词(空格分隔)", true, "legacy_expand");
    }

    std::size_t limit = 5;
    if (const auto it = input.find("limit"); it != input.end() && !it->is_null()) {
        if (!it->is_number_integer() || it->get<int>() < 1) {
            return MakeSearchResult("limit 得是正整数", true, "legacy_expand");
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
        return MakeSearchResult(out, false, "legacy_expand");
    }

    std::string out = "命中 " + std::to_string(hits.size()) + " 个工具:\n";
    for (const auto& hit : hits) {
        loaded_->insert(hit.tool->name());
        out += "- " + hit.tool->name() + ": " + SingleLine(hit.tool->description()) + "\n";
        out += "  参数: " + SummarizeSchema(hit.tool->input_schema()) + "\n";
    }
    out += "以上工具已挂载,可直接调用。";
    return MakeSearchResult(out, false, "legacy_expand");
}

// ---------------------------------------------------------------------------
// proxy 路(单子 §5.2/§5.3):结果是一份结构化 JSON——catalog_revision +
// matches[](tool_ref/name/description/input_schema/schema_digest/source/
// source_instance),给人看的短文只是展示,不作恢复事实源(恢复走宿主的
// DiscoveryLedger)。不写 loaded、不碰顶层 tools,搜索只读 catalog。
// ---------------------------------------------------------------------------
Tool::Result ToolSearchTool::ExecuteProxy(const nlohmann::json& input) {
    if (!input.contains("query") || !input.at("query").is_string()) {
        return MakeSearchResult("缺少必填参数 query(字符串)", true, "proxy_reference");
    }
    const std::vector<std::string> tokens = Tokenize(input.at("query").get<std::string>());
    if (tokens.empty()) {
        return MakeSearchResult("query 不能是空白字符串,给几个关键词(空格分隔)", true, "proxy_reference");
    }

    // limit 收紧合同(§5.2):默认 5,硬上限 20——proxy 结果每条都带完整
    // schema,上界不掐死会一次摊出几万 token 的正文。legacy 路不带这个
    // 闸,兼容行为一字不动。
    std::size_t limit = 5;
    if (const auto it = input.find("limit"); it != input.end() && !it->is_null()) {
        if (!it->is_number_integer() || it->get<int>() < 1) {
            return MakeSearchResult("limit 得是正整数", true, "proxy_reference");
        }
        limit = static_cast<std::size_t>(it->get<int>());
    }
    constexpr std::size_t kProxyLimitHardCap = 20;
    limit = (std::min)(limit, kProxyLimitHardCap);

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

    const std::string catalog_revision = DeferredToolResolver::CatalogRevisionOf(registry_);
    nlohmann::json out = nlohmann::json::object();
    out["catalog_revision"] = catalog_revision;
    nlohmann::json matches = nlohmann::json::array();

    if (hits.empty()) {
        out["matches"] = matches;
        out["note"] = "没有命中任何延迟工具(共 " + std::to_string(deferred_tools.size()) +
                      " 枚可搜);换个关键词再试。";
        return MakeSearchResult(out.dump(), false, "proxy_reference");
    }

    for (const auto& hit : hits) {
        const Tool& tool = *hit.tool;
        const std::string digest = DeferredToolResolver::SchemaDigestOf(tool);
        const std::string dumped = tool.input_schema().dump();
        // schema 很大不能悄悄截断(§5.3):单项超上限就报 schema_too_large,
        // 不铸 ref——模型该走专用方案或显式 legacy,不该拿到半截 schema。
        if (dumped.size() > kMaxDiscoverySchemaBytes) {
            nlohmann::json item = nlohmann::json::object();
            item["name"] = tool.name();
            item["schema_digest"] = digest;
            item["error"] = "schema_too_large";
            item["hint"] = "该工具的 input_schema 超过 discovery 结果的安全展开上限(" +
                           std::to_string(kMaxDiscoverySchemaBytes) +
                           " 字节),未发 tool_ref;请改走专用方案,或由用户切回 legacy_expand 模式。";
            matches.push_back(std::move(item));
            continue;
        }
        // 同 digest 已摊过全文:回短引用,不再塞正文(§5.3);ref 复用同一枚。
        const auto existing = resolver_->ledger().FindLive(tool.name(),
                                                           registry_.RegistrationOf(tool.name()) != nullptr
                                                               ? registry_.RegistrationOf(tool.name())->source_instance
                                                               : std::string(),
                                                           digest);
        const DeferredToolRefRecord record =
            resolver_->Discover(tool, registry_.RegistrationOf(tool.name()), catalog_revision,
                                /*discovered_event_id=*/std::string(), /*schema_expanded=*/true);
        nlohmann::json item = nlohmann::json::object();
        item["tool_ref"] = record.tool_ref;
        item["name"] = tool.name();
        item["schema_digest"] = digest;
        if (existing.has_value() && existing->schema_expanded) {
            item["repeat"] = true;
            item["note"] = "本会话已发过该工具的完整 schema,此处只回短引用;需要全文时按 name 找早前结果。";
        } else {
            item["description"] = SingleLine(tool.description());
            item["input_schema"] = tool.input_schema();
        }
        const ToolRegistration* registration = registry_.RegistrationOf(tool.name());
        if (registration != nullptr) {
            item["source"] = SourceKindLabel(registration->source_kind);
            if (!registration->source_instance.empty()) {
                item["source_instance"] = registration->source_instance;
            }
        }
        matches.push_back(std::move(item));
    }
    out["matches"] = matches;
    // 正文是模型吃的机器事实(结构化、可解析);人话只有 note 一句,不作
    // 恢复事实源。调用下一枚命中工具用 tool_invoke({tool_ref, arguments})。
    Tool::Result result(out.dump(), false);
    result.details["deferred_tool_mode"] = "proxy_reference";
    result.details["catalog_revision"] = catalog_revision;
    result.details["match_count"] = matches.size();
    return result;
}

// ---------------------------------------------------------------------------
// tool_invoke 固定 wire 壳(单子 §5.4/§6.1):定义常驻、schema 恒定;真正的
// 执行在 AgentLoop 规范化之后只对真实目标走一次 RunOneTool。直接调进这只
// 壳的入口(PTC 等未接规范化的路)得到稳定拒绝——不在壳里 target->execute,
// 那是第二条执行暗道(单子红线 5)。
// ---------------------------------------------------------------------------
std::string ToolInvokeTool::name() const {
    return "tool_invoke";
}

std::string ToolInvokeTool::description() const {
    return ToolText(
        "tool_invoke", "description",
        "调用一枚此前经 tool_search 发现的延迟工具。tool_ref 用 tool_search 结果里发出的那枚引用,"
        "不可自行拼装;arguments 是要传给目标工具的参数对象,须符合发现结果里的 input_schema。"
        "宿主会按 tool_ref 找回真实工具、用其当下 schema 复验参数后再执行。");
}

nlohmann::json ToolInvokeTool::input_schema() const {
    // 顶层定义一个字节不变(单子 §5.1/§5.4):同一 cache epoch 内数量、次序、
    // name/description/schema 全部恒定。arguments 细校验不靠这层宽 schema。
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";
    nlohmann::json properties = nlohmann::json::object();
    nlohmann::json ref_prop = nlohmann::json::object();
    ref_prop["type"] = "string";
    ref_prop["description"] =
        ToolText("tool_invoke", "param.tool_ref", "tool_search 结果里发出的不透明工具引用。");
    properties["tool_ref"] = ref_prop;
    nlohmann::json args_prop = nlohmann::json::object();
    args_prop["type"] = "object";
    args_prop["description"] = ToolText("tool_invoke", "param.arguments",
                                        "传给目标工具的参数对象,按发现结果里的 input_schema 填写。");
    properties["arguments"] = args_prop;
    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"tool_ref", "arguments"});
    schema["additionalProperties"] = false;
    return schema;
}

Tool::Result ToolInvokeTool::execute(const nlohmann::json& /*input*/) {
    Tool::Result refused(
        "tool_invoke 是代理壳,不是执行口:本入口未接引用规范化,不在此执行任何目标工具(稳定码 " +
            std::string(kErrToolInvokeDirectCall) +
            ")。模型侧请照常发起 tool_use 调用,由宿主解引用后从执行正门跑;宿主侧入口请接 "
            "DeferredToolResolver 后再调 RunOneTool。",
        true);
    refused.outcome = "host_error";
    refused.error_code = kErrToolInvokeDirectCall;
    refused.details["deferred_tool_mode"] = "proxy_reference";
    return refused;
}

}  // namespace lubancode::tools
