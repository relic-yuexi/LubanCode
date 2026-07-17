// search 工具:grep(按正则搜内容)和 glob(按文件名找文件)合成一个工具,
// 用 mode 参数区分,没有拆成两个工具。理由(报告里也会提):
//   1. 两种模式共用同一套"递归遍历目录、跳过 .git/build/node_modules、
//      跳过二进制文件"的逻辑,拆开会有大段重复代码,合在一起天然复用。
//   2. 对模型来说,"要不要搜内容"其实是一句话就能说清楚的选择(mode 参数),
//      不比记两个相似名字的工具(容易记混什么时候该用哪个)更难选。
//   3. 工具目录越精简,模型每次要在 tools 列表里挑选、组装入参的负担越小。
// 权衡:mode=glob 时 pattern 参数其实用不上 grep 的“正则语义”,schema 里
// 用同一个字段名承载两种语义,靠 description 说明清楚,不算完美,但换来
// 代码和工具列表都更简单,这里判断利大于弊。

#include "tools/search.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "tools/path_utils.hpp"

namespace lubancode::tools {

namespace {

constexpr std::size_t kMaxResults = 100;

const std::vector<std::string>& SkipDirNames() {
    static const std::vector<std::string> names = {".git", "build", "node_modules"};
    return names;
}

bool ShouldSkipDir(const std::filesystem::path& dir_name) {
    const std::string name = PathToUtf8(dir_name);
    const auto& skip = SkipDirNames();
    return std::find(skip.begin(), skip.end(), name) != skip.end();
}

// 读文件头 8KB,含 \0 就当二进制文件,跳过不搜。
bool LooksBinary(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return true;  // 打不开就别搜了,当它是"不能搜"处理
    }
    char buf[8192];
    file.read(buf, sizeof(buf));
    const std::streamsize n = file.gcount();
    for (std::streamsize i = 0; i < n; ++i) {
        if (buf[i] == '\0') {
            return true;
        }
    }
    return false;
}

// 把简单通配符转成 std::regex(ECMAScript):
//   * 匹配任意长度但不跨目录(不匹配 '/')
//   ** 匹配任意长度,可以跨目录
//   ? 匹配单个非 '/' 字符
// 其余字符按字面量转义。
std::regex GlobToRegex(const std::string& glob_pattern) {
    std::string regex_str = "^";
    for (std::size_t i = 0; i < glob_pattern.size(); ++i) {
        const char c = glob_pattern[i];
        if (c == '*') {
            if (i + 1 < glob_pattern.size() && glob_pattern[i + 1] == '*') {
                regex_str += ".*";
                ++i;
            } else {
                regex_str += "[^/]*";
            }
        } else if (c == '?') {
            regex_str += "[^/]";
        } else if (std::string(".^$+(){}|[]\\").find(c) != std::string::npos) {
            regex_str += '\\';
            regex_str += c;
        } else {
            regex_str += c;
        }
    }
    regex_str += "$";
    return std::regex(regex_str, std::regex::ECMAScript);
}

std::string NormalizeSlashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

// 递归遍历 root 下所有文件,跳过 .git/build/node_modules 目录,把每个文件的
// (绝对路径, 相对 root 的路径) 丢给 visit。visit 返回 false 表示"够了,别再
// 找了"(用来在拿满上限结果之后尽早收手,免得大仓库遍历半天)。
template <typename Visit>
void WalkFiles(const std::filesystem::path& root, const Visit& visit) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return;
    }

    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
        return;
    }
    const std::filesystem::recursive_directory_iterator end;

    while (it != end) {
        const std::filesystem::directory_entry entry = *it;

        std::error_code entry_ec;
        if (entry.is_directory(entry_ec)) {
            if (ShouldSkipDir(entry.path().filename())) {
                it.disable_recursion_pending();
            }
        } else if (entry.is_regular_file(entry_ec)) {
            std::error_code rel_ec;
            const std::filesystem::path rel = std::filesystem::relative(entry.path(), root, rel_ec);
            if (!rel_ec) {
                if (!visit(entry.path(), rel)) {
                    return;
                }
            }
        }

        it.increment(ec);
        if (ec) {
            return;
        }
    }
}

Tool::Result RunGrep(const std::filesystem::path& root, const std::string& pattern_str, const std::string& glob_filter) {
    std::regex pattern;
    try {
        pattern = std::regex(pattern_str, std::regex::ECMAScript);
    } catch (const std::regex_error& e) {
        return {"pattern 不是合法的正则表达式(ECMAScript 语法): " + std::string(e.what()), true};
    }

    std::optional<std::regex> filter_regex;
    if (!glob_filter.empty()) {
        try {
            filter_regex = GlobToRegex(glob_filter);
        } catch (const std::regex_error& e) {
            return {"glob 过滤表达式不合法: " + std::string(e.what()), true};
        }
    }

    std::ostringstream out;
    std::size_t hit_count = 0;
    bool truncated = false;

    WalkFiles(root, [&](const std::filesystem::path& abs_path, const std::filesystem::path& rel_path) -> bool {
        if (hit_count >= kMaxResults) {
            truncated = true;
            return false;
        }
        if (filter_regex.has_value()) {
            const std::string basename = PathToUtf8(abs_path.filename());
            if (!std::regex_match(basename, *filter_regex)) {
                return true;
            }
        }
        if (LooksBinary(abs_path)) {
            return true;
        }

        const std::string rel_utf8 = NormalizeSlashes(PathToUtf8(rel_path));

        std::ifstream file(abs_path, std::ios::binary);
        if (!file.is_open()) {
            return true;
        }
        std::string line;
        long long line_no = 0;
        while (std::getline(file, line)) {
            ++line_no;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (std::regex_search(line, pattern)) {
                out << rel_utf8 << ":" << line_no << ":" << line << "\n";
                ++hit_count;
                if (hit_count >= kMaxResults) {
                    truncated = true;
                    return false;
                }
            }
        }
        return true;
    });

    if (hit_count == 0) {
        return {"没搜到匹配的内容", false};
    }
    std::string content = out.str();
    if (truncated) {
        content += "……(结果超过 " + std::to_string(kMaxResults) + " 条,已截断,建议缩小 pattern 或 path 范围)\n";
    }
    return {content, false};
}

Tool::Result RunGlobSearch(const std::filesystem::path& root, const std::string& pattern_str) {
    std::regex pattern;
    const bool match_basename_only = pattern_str.find('/') == std::string::npos;
    try {
        pattern = GlobToRegex(pattern_str);
    } catch (const std::regex_error& e) {
        return {"pattern 不是合法的通配符表达式: " + std::string(e.what()), true};
    }

    std::vector<std::string> hits;
    bool truncated = false;

    WalkFiles(root, [&](const std::filesystem::path& abs_path, const std::filesystem::path& rel_path) -> bool {
        if (hits.size() >= kMaxResults) {
            truncated = true;
            return false;
        }
        const std::string rel_utf8 = NormalizeSlashes(PathToUtf8(rel_path));
        const std::string match_target = match_basename_only ? PathToUtf8(abs_path.filename()) : rel_utf8;
        if (std::regex_match(match_target, pattern)) {
            hits.push_back(rel_utf8);
        }
        return true;
    });

    if (hits.empty()) {
        return {"没找到匹配的文件", false};
    }
    std::ostringstream out;
    for (const auto& hit : hits) {
        out << hit << "\n";
    }
    if (truncated) {
        out << "……(结果超过 " << kMaxResults << " 条,已截断,建议把 pattern 写得更具体)\n";
    }
    return {out.str(), false};
}

}  // namespace

std::string SearchTool::name() const {
    return "search";
}

std::string SearchTool::description() const {
    return "在目录里搜索,两种模式:mode=\"grep\" 按正则(ECMAScript 语法)搜文件内容,"
           "命中的行按 文件:行号:行内容 返回;mode=\"glob\" 按文件名通配(支持 * ? **)找文件,"
           "返回相对路径列表。默认从当前工作目录开始搜,自动跳过 .git/、build/、"
           "node_modules/ 和二进制文件。结果超过 100 条会截断并注明。";
}

nlohmann::json SearchTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();

    nlohmann::json mode_prop = nlohmann::json::object();
    mode_prop["type"] = "string";
    mode_prop["enum"] = nlohmann::json::array({"grep", "glob"});
    mode_prop["description"] = "\"grep\" 搜文件内容(正则),\"glob\" 按文件名找文件(通配符)";
    properties["mode"] = mode_prop;

    nlohmann::json pattern_prop = nlohmann::json::object();
    pattern_prop["type"] = "string";
    pattern_prop["description"] =
        "mode=grep 时是 ECMAScript 正则表达式;mode=glob 时是文件名通配符(如 *.cpp、src/**/*.hpp)";
    properties["pattern"] = pattern_prop;

    nlohmann::json path_prop = nlohmann::json::object();
    path_prop["type"] = "string";
    path_prop["description"] = "从哪个目录开始搜,不填默认当前工作目录";
    properties["path"] = path_prop;

    nlohmann::json glob_prop = nlohmann::json::object();
    glob_prop["type"] = "string";
    glob_prop["description"] = "仅 mode=grep 有效:按文件名过滤要搜索的文件,比如 *.cpp,不填就搜所有非二进制文件";
    properties["glob"] = glob_prop;

    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"mode", "pattern"});

    return schema;
}

Tool::Result SearchTool::execute(const nlohmann::json& input) {
    if (!input.contains("mode") || !input.at("mode").is_string()) {
        return {"缺少必填参数 mode(字符串,\"grep\" 或 \"glob\")", true};
    }
    if (!input.contains("pattern") || !input.at("pattern").is_string()) {
        return {"缺少必填参数 pattern(字符串)", true};
    }
    const std::string mode = input.at("mode").get<std::string>();
    const std::string pattern = input.at("pattern").get<std::string>();
    if (pattern.empty()) {
        return {"pattern 不能是空字符串", true};
    }

    std::filesystem::path root = std::filesystem::current_path();
    if (auto it = input.find("path"); it != input.end() && !it->is_null() && it->is_string()) {
        const std::string path_str = it->get<std::string>();
        if (!path_str.empty()) {
            root = Utf8ToPath(path_str);
        }
    }

    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
        return {"path 不存在: " + PathToUtf8(root), true};
    }
    if (!std::filesystem::is_directory(root, ec)) {
        return {"path 得是个目录: " + PathToUtf8(root), true};
    }

    if (mode == "grep") {
        std::string glob_filter;
        if (auto it = input.find("glob"); it != input.end() && !it->is_null() && it->is_string()) {
            glob_filter = it->get<std::string>();
        }
        return RunGrep(root, pattern, glob_filter);
    }
    if (mode == "glob") {
        return RunGlobSearch(root, pattern);
    }
    return {"mode 只认 \"grep\" 或 \"glob\",你写的是: " + mode, true};
}

}  // namespace lubancode::tools
