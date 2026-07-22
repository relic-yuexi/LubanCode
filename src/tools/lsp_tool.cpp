#include "tools/lsp_tool.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace lubancode::tools {

namespace {

// LSP SymbolKind(1..26)-> 中文名。规范附录里的那张表。
const char* SymbolKindName(int kind) {
    switch (kind) {
        case 1: return "文件";
        case 2: return "模块";
        case 3: return "命名空间";
        case 4: return "包";
        case 5: return "类";
        case 6: return "方法";
        case 7: return "属性";
        case 8: return "字段";
        case 9: return "构造函数";
        case 10: return "枚举";
        case 11: return "接口";
        case 12: return "函数";
        case 13: return "变量";
        case 14: return "常量";
        case 15: return "字符串";
        case 16: return "数字";
        case 17: return "布尔";
        case 18: return "数组";
        case 19: return "对象";
        case 20: return "键";
        case 21: return "空值";
        case 22: return "枚举成员";
        case 23: return "结构体";
        case 24: return "事件";
        case 25: return "运算符";
        case 26: return "类型参数";
        default: return "符号";
    }
}

// DiagnosticSeverity(1..4)-> 中文名。
const char* SeverityName(int severity) {
    switch (severity) {
        case 1: return "错误";
        case 2: return "警告";
        case 3: return "信息";
        case 4: return "提示";
        default: return "诊断";
    }
}

// 从一个 Location/LocationLink 里抠出 (路径, 0基行, 0基列)。抠不出返回 false。
struct PlainLocation {
    std::string path;
    int line = 0;
    int character = 0;
};

bool ExtractLocation(const nlohmann::json& item, PlainLocation& out) {
    if (!item.is_object()) {
        return false;
    }
    std::string uri;
    nlohmann::json range;
    if (item.contains("uri") && item["uri"].is_string() && item.contains("range")) {
        // Location
        uri = item["uri"].get<std::string>();
        range = item["range"];
    } else if (item.contains("targetUri") && item["targetUri"].is_string()) {
        // LocationLink:优先 targetSelectionRange(符号名本身),退而求其次
        // targetRange。
        uri = item["targetUri"].get<std::string>();
        if (item.contains("targetSelectionRange")) {
            range = item["targetSelectionRange"];
        } else if (item.contains("targetRange")) {
            range = item["targetRange"];
        }
    } else {
        return false;
    }
    if (!range.is_object() || !range.contains("start") || !range["start"].is_object()) {
        return false;
    }
    const auto& start = range["start"];
    out.path = lsp::UriToPath(uri);
    out.line = start.value("line", 0);
    out.character = start.value("character", 0);
    return true;
}

// "文件:行:列",1 基输出。
std::string FormatPlainLocation(const PlainLocation& loc) {
    return loc.path + ":" + std::to_string(loc.line + 1) + ":" + std::to_string(loc.character + 1);
}

// 从 DocumentSymbol/SymbolInformation 里抠出起始行(0 基)。
int SymbolStartLine(const nlohmann::json& item) {
    if (item.contains("selectionRange") && item["selectionRange"].is_object()) {
        return item["selectionRange"].value("start", nlohmann::json::object()).value("line", 0);
    }
    if (item.contains("range") && item["range"].is_object()) {
        return item["range"].value("start", nlohmann::json::object()).value("line", 0);
    }
    if (item.contains("location") && item["location"].is_object() && item["location"].contains("range")) {
        return item["location"]["range"].value("start", nlohmann::json::object()).value("line", 0);
    }
    return 0;
}

// 层级式 DocumentSymbol 递归展开,depth 控制缩进。
void AppendSymbol(const nlohmann::json& item, int depth, std::string& out) {
    if (!item.is_object()) {
        return;
    }
    const std::string name = item.value("name", std::string("(无名)"));
    const int kind = item.value("kind", 0);
    const int line = SymbolStartLine(item);
    out.append(static_cast<std::size_t>(depth) * 2, ' ');
    out += "- " + name + " [" + SymbolKindName(kind) + "] 第 " + std::to_string(line + 1) + " 行\n";
    if (item.contains("children") && item["children"].is_array()) {
        for (const auto& child : item["children"]) {
            AppendSymbol(child, depth + 1, out);
        }
    }
}

// 读文件全文(UTF-8 字节串)。读不到给 nullopt。
std::optional<std::string> ReadFileBytes(const std::string& path_utf8) {
    const std::filesystem::path path(
        std::u8string(reinterpret_cast<const char8_t*>(path_utf8.data()), path_utf8.size()));
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || std::filesystem::is_directory(path, ec)) {
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// 从整段文本里取第 zero_based_line 行(不含换行符,去掉行尾 \r)。
std::optional<std::string> LineFromText(const std::string& text, int zero_based_line) {
    int current = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            nl = text.size();
        }
        if (current == zero_based_line) {
            std::string line = text.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return line;
        }
        if (nl >= text.size()) {
            break;
        }
        pos = nl + 1;
        ++current;
    }
    return std::nullopt;
}

std::string TrimCopy(const std::string& s) {
    std::size_t begin = 0;
    while (begin < s.size() && (s[begin] == ' ' || s[begin] == '\t')) {
        ++begin;
    }
    std::size_t end = s.size();
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r')) {
        --end;
    }
    return s.substr(begin, end - begin);
}

}  // namespace

std::string FormatLspDefinition(const nlohmann::json& result, const LspLineReader& line_reader) {
    std::vector<PlainLocation> locations;
    if (result.is_object()) {
        PlainLocation loc;
        if (ExtractLocation(result, loc)) {
            locations.push_back(std::move(loc));
        }
    } else if (result.is_array()) {
        for (const auto& item : result) {
            PlainLocation loc;
            if (ExtractLocation(item, loc)) {
                locations.push_back(std::move(loc));
            }
        }
    }
    if (locations.empty()) {
        return "没找到定义。";
    }
    std::string out;
    for (const auto& loc : locations) {
        out += FormatPlainLocation(loc);
        if (line_reader) {
            if (const auto line_text = line_reader(loc.path, loc.line); line_text.has_value()) {
                out += "\n    " + TrimCopy(*line_text);
            }
        }
        out += "\n";
    }
    return out;
}

std::string FormatLspReferences(const nlohmann::json& result, std::size_t max_items) {
    std::vector<PlainLocation> locations;
    if (result.is_array()) {
        for (const auto& item : result) {
            PlainLocation loc;
            if (ExtractLocation(item, loc)) {
                locations.push_back(std::move(loc));
            }
        }
    }
    if (locations.empty()) {
        return "没找到引用。";
    }
    std::string out = "共 " + std::to_string(locations.size()) + " 处引用";
    if (locations.size() > max_items) {
        out += "(只列前 " + std::to_string(max_items) + " 处)";
    }
    out += ":\n";
    const std::size_t shown = locations.size() < max_items ? locations.size() : max_items;
    for (std::size_t i = 0; i < shown; ++i) {
        out += "  " + FormatPlainLocation(locations[i]) + "\n";
    }
    return out;
}

std::string FormatLspSymbols(const nlohmann::json& result) {
    if (!result.is_array() || result.empty()) {
        return "没找到符号。";
    }
    std::string out;
    for (const auto& item : result) {
        AppendSymbol(item, 0, out);
    }
    if (out.empty()) {
        return "没找到符号。";
    }
    return out;
}

std::string FormatLspDiagnostics(const nlohmann::json& diagnostics) {
    if (!diagnostics.is_array() || diagnostics.empty()) {
        return "没有诊断问题。";
    }
    std::string out = "共 " + std::to_string(diagnostics.size()) + " 条诊断:\n";
    for (const auto& item : diagnostics) {
        if (!item.is_object()) {
            continue;
        }
        const int severity = item.value("severity", 0);
        int line = 0;
        if (item.contains("range") && item["range"].is_object()) {
            line = item["range"].value("start", nlohmann::json::object()).value("line", 0);
        }
        const std::string message = item.value("message", std::string());
        out += "  [" + std::string(SeverityName(severity)) + "] 第 " + std::to_string(line + 1) + " 行: " + message +
               "\n";
    }
    return out;
}

std::string FormatLspDiagnostics(const std::optional<nlohmann::json>& diagnostics) {
    if (!diagnostics.has_value()) {
        return "暂无诊断(等了 2 秒服务器还没推送;稍后再查一次,或者这个文件确实没被诊断)。";
    }
    return FormatLspDiagnostics(*diagnostics);
}

LspTool::LspTool(lsp::Manager& manager) : manager_(manager) {}

std::string LspTool::name() const {
    return "lsp";
}

std::string LspTool::description() const {
    return "用 LSP 语言服务器做语义查询:mode=definition 查定义(需要 line/character),"
           "mode=references 查引用(需要 line/character),mode=symbols 列文件里的符号,"
           "mode=diagnostics 看文件的诊断(错误/警告)。line/character 是 1 基,跟编辑器显示一致。"
           "只有 config 的 lsp 段配置过的语言(按文件扩展名路由)才能查。";
}

nlohmann::json LspTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties",
         {{"mode",
           {{"type", "string"},
            {"enum", {"definition", "references", "symbols", "diagnostics"}},
            {"description", "查询类型"}}},
          {"file", {{"type", "string"}, {"description", "要查询的文件路径(相对或绝对)"}}},
          {"line", {{"type", "integer"}, {"description", "行号,1 基(definition/references 必填)"}}},
          {"character", {{"type", "integer"}, {"description", "列号,1 基(definition/references 必填)"}}}}},
        {"required", {"mode", "file"}},
    };
}

tools::Tool::Result LspTool::execute(const nlohmann::json& input) {
    const std::string mode = input.value("mode", std::string());
    if (mode != "definition" && mode != "references" && mode != "symbols" && mode != "diagnostics") {
        return Result{"mode 参数不对: " + mode + "(只认 definition / references / symbols / diagnostics)", true};
    }
    const std::string file = input.value("file", std::string());
    if (file.empty()) {
        return Result{"缺少必填参数 file(要查询的文件路径)", true};
    }

    const bool positional = (mode == "definition" || mode == "references");
    int line = 0;
    int character = 0;
    if (positional) {
        if (!input.contains("line") || !input["line"].is_number_integer() || !input.contains("character") ||
            !input["character"].is_number_integer()) {
            return Result{"mode=" + mode + " 需要 line 和 character 两个整数参数(1 基)", true};
        }
        line = input["line"].get<int>();
        character = input["character"].get<int>();
        if (line < 1 || character < 1) {
            return Result{"line/character 是 1 基,必须 >= 1(收到 line=" + std::to_string(line) +
                              ", character=" + std::to_string(character) + ")",
                          true};
        }
    }

    const auto language = manager_.LanguageForFile(file);
    if (!language.has_value()) {
        return Result{"文件 " + file + " 的扩展名没有对应的 LSP 配置(config.json 的 lsp 段里没配管这个扩展的语言)",
                      true};
    }

    // 路径转绝对(LSP 的 URI 得是绝对路径),再读盘拿内容做 didOpen。
    std::error_code ec;
    const std::filesystem::path fs_path(
        std::u8string(reinterpret_cast<const char8_t*>(file.data()), file.size()));
    std::filesystem::path abs_path = std::filesystem::absolute(fs_path, ec);
    if (ec) {
        abs_path = fs_path;
    }
    const std::u8string abs_u8 = abs_path.u8string();
    const std::string abs_utf8(reinterpret_cast<const char*>(abs_u8.data()), abs_u8.size());

    const auto content = ReadFileBytes(abs_utf8);
    if (!content.has_value()) {
        return Result{"读不到文件: " + abs_utf8 + "(不存在、是目录,或没有读权限)", true};
    }

    const auto client_result = manager_.AcquireClient(*language);
    if (!client_result.has_value()) {
        return Result{client_result.error(), true};
    }
    lsp::Client* client = *client_result;

    const std::string uri = lsp::PathToUri(abs_utf8);
    client->EnsureDidOpen(uri, *language, *content);

    // 真读盘的行文本读取器(定义结果展示目标行内容用)。目标可能在别的
    // 文件里(比如头文件),不能复用上面 didOpen 那份内容。
    const LspLineReader line_reader = [](const std::string& path, int zero_based_line) -> std::optional<std::string> {
        const auto text = ReadFileBytes(path);
        if (!text.has_value()) {
            return std::nullopt;
        }
        return LineFromText(*text, zero_based_line);
    };

    if (mode == "definition") {
        const auto result = client->Definition(uri, line - 1, character - 1);
        if (!result.has_value()) {
            return Result{result.error(), true};
        }
        return Result{FormatLspDefinition(*result, line_reader), false};
    }
    if (mode == "references") {
        const auto result = client->References(uri, line - 1, character - 1);
        if (!result.has_value()) {
            return Result{result.error(), true};
        }
        return Result{FormatLspReferences(*result), false};
    }
    if (mode == "symbols") {
        const auto result = client->DocumentSymbol(uri);
        if (!result.has_value()) {
            return Result{result.error(), true};
        }
        return Result{FormatLspSymbols(*result), false};
    }
    // diagnostics:didOpen 之后服务器会主动推,读缓存,最多等 2s。
    const auto diagnostics = client->WaitDiagnostics(uri, 2000);
    return Result{FormatLspDiagnostics(diagnostics), false};
}

}  // namespace lubancode::tools
