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

#include "tools/observation_filter.hpp"  // 观察边界(P2-5):子代理日志/.evidence 默认不搜
#include "tools/path_utils.hpp"
#include "tools/tool_text.hpp"  // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/

namespace lubancode::tools {

namespace {

constexpr std::size_t kMaxResults = 100;

const std::vector<std::string>& SkipDirNames() {
    // .evidence 是 P2-5 补的:运行时证据目录(会话/子代理产物)默认不搜,
    // 免得 Agent 把自己的观察记录吞回上下文。显式把 path 点名到边界内
    // (根在边界内/单文件)不受这道限制,见 WalkFiles 与 CollectSearchFiles。
    static const std::vector<std::string> names = {".git", "build", "node_modules", ".evidence"};
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

// 把简单通配符转成 std::regex(ECMAScript),语义照 gitignore/ripgrep 那一套:
//   *     匹配任意长度但不跨目录(不匹配 '/')
//   ?     匹配单个非 '/' 字符
//   **/   出现在开头或紧跟在 '/' 后面、后面又跟着 '/' 时,当"零层或多层目录"
//         整体处理,比如 "**/*.md" 既要中根目录的 a.md,也要中 sub/b.md。
//   /**   出现在结尾、前面是 '/' 时,当"这层目录本身,或者它底下任意深度"处理。
//   其余出现的 "**"(前后凑不成上面两种干净边界的)退化成普通的 "任意字符"(.*)。
// 其余字符按字面量转义。
std::regex GlobToRegex(const std::string& glob_pattern) {
    std::string regex_str = "^";
    const std::size_t n = glob_pattern.size();
    std::size_t i = 0;
    while (i < n) {
        const char c = glob_pattern[i];
        if (c == '*' && i + 1 < n && glob_pattern[i + 1] == '*') {
            const std::size_t after = i + 2;
            const bool prev_boundary = (i == 0) || (glob_pattern[i - 1] == '/');
            const bool next_slash = (after < n) && (glob_pattern[after] == '/');
            const bool next_end = (after == n);

            if (prev_boundary && next_slash) {
                // "**/" 在开头,或紧跟在 '/' 后面:零层或多层目录都算数
                regex_str += "(?:.*/)?";
                i = after + 1;  // 把 "**" 后面那个 '/' 也一起吃掉
                continue;
            }
            if (i > 0 && glob_pattern[i - 1] == '/' && next_end) {
                // 路径末尾的 "/**":吞掉前面已经写进去的那个字面 '/',
                // 换成"这层目录本身,或者它底下任意内容"
                if (!regex_str.empty() && regex_str.back() == '/') {
                    regex_str.pop_back();
                }
                regex_str += "(?:/.*)?";
                i = after;
                continue;
            }
            // 孤立的 "**"(前后不构成上面两种干净边界),退化成普通任意匹配
            regex_str += ".*";
            i = after;
            continue;
        }
        if (c == '*') {
            regex_str += "[^/]*";
            ++i;
        } else if (c == '?') {
            regex_str += "[^/]";
            ++i;
        } else if (std::string(".^$+(){}|[]\\").find(c) != std::string::npos) {
            regex_str += '\\';
            regex_str += c;
            ++i;
        } else {
            regex_str += c;
            ++i;
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
// root 不是目录(不存在、或是单个文件)时由调用方另行分发,这里只管目录。
// 观察边界(P2-5):root 在边界外时,边界内的目录与文件(子代理日志、
// .evidence 产物、运行时登记的日志目录)默认不进结果;root 本身在边界内
// = path 显式点名到了证据区,照常搜——默认过滤只挡"无意间搜到"。
template <typename Visit>
void WalkFiles(const std::filesystem::path& root, const Visit& visit) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return;
    }
    const bool root_in_boundary = PathInObservationBoundary(root);

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
            if (ShouldSkipDir(entry.path().filename()) ||
                (!root_in_boundary && PathInObservationBoundary(entry.path()))) {
                it.disable_recursion_pending();
            }
        } else if (entry.is_regular_file(entry_ec)) {
            if (!root_in_boundary && PathInObservationBoundary(entry.path())) {
                it.increment(ec);
                if (ec) {
                    return;
                }
                continue;
            }
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

// 统一的取文件入口:root 是目录就走 WalkFiles 递归遍历;是单个文件就只看
// 它这一个(rel 即文件名)。模型揣着具体文件路径来搜是常有的事(ripgrep 也认
// rg pattern file.cpp),与其报错把人挡回去,不如把单文件当作"只含它自己的
// 搜索范围"。grep/glob 两种模式共用这一套分发。
template <typename Visit>
void CollectSearchFiles(const std::filesystem::path& root, const Visit& visit) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(root, ec)) {
        visit(root, root.filename());
        return;
    }
    WalkFiles(root, visit);
}

Tool::Result RunGrep(const std::filesystem::path& root, const std::string& pattern_str, const std::string& glob_filter) {
    std::regex pattern;
    try {
        pattern = std::regex(pattern_str, std::regex::ECMAScript);
    } catch (const std::regex_error& e) {
        return {"pattern 不是合法的正则表达式(ECMAScript 语法): " + std::string(e.what()), true};
    }

    std::optional<std::regex> filter_regex;
    // 跟 glob 模式一致:filter 里不带 '/' 就只拿文件名(basename)去配,
    // 这样 "*.cpp" 才能递归找到所有目录下的 .cpp,不用非得写成 "**/*.cpp"。
    // filter 里带 '/' 才拿相对 root 的完整路径去配。
    const bool filter_basename_only = glob_filter.find('/') == std::string::npos;
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

    // P2-5:单文件点名到观察边界内 = 显式点名,照常搜,但正文前给一行
    // 体积提示(超过 256KB 劝阻)。目录根不用提示——过滤逻辑在 WalkFiles
    // 里,边界内的文件默认根本进不来。
    std::string notice;
    {
        std::error_code file_ec;
        if (std::filesystem::is_regular_file(root, file_ec)) {
            std::error_code size_ec;
            const std::uintmax_t size = std::filesystem::file_size(root, size_ec);
            if (!size_ec) {
                notice = ObservationReadNotice(root, size);
            }
        }
    }

    CollectSearchFiles(root, [&](const std::filesystem::path& abs_path, const std::filesystem::path& rel_path) -> bool {
        if (hit_count >= kMaxResults) {
            truncated = true;
            return false;
        }
        const std::string rel_utf8 = NormalizeSlashes(PathToUtf8(rel_path));
        if (filter_regex.has_value()) {
            const std::string match_target = filter_basename_only ? PathToUtf8(abs_path.filename()) : rel_utf8;
            if (!std::regex_match(match_target, *filter_regex)) {
                return true;
            }
        }
        if (LooksBinary(abs_path)) {
            return true;
        }

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
        return {notice + "没搜到匹配的内容", false};
    }
    std::string content = notice + out.str();
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

    CollectSearchFiles(root, [&](const std::filesystem::path& abs_path, const std::filesystem::path& rel_path) -> bool {
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
    // 文案在 src/prompts/tools/<语言>/search.md,兜底与 zh-CN 档同文。
    return ToolText("search", "description",
                    "在目录或单个文件里搜索,两种模式:mode=\"grep\" 按正则(ECMAScript 语法)搜文件内容,"
                    "命中的行按 文件:行号:行内容 返回;mode=\"glob\" 按文件名通配(支持 * ? **)找文件,"
                    "返回相对路径列表。默认从当前工作目录开始搜,自动跳过 .git/、build/、"
                    "node_modules/、.evidence/(运行时观察记录)和二进制文件。结果超过 100 条会截断并注明;"
                    "要搜观察记录,把 path 逐字点名到具体文件或目录即可。");
}

nlohmann::json SearchTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();

    nlohmann::json mode_prop = nlohmann::json::object();
    mode_prop["type"] = "string";
    mode_prop["enum"] = nlohmann::json::array({"grep", "glob"});
    mode_prop["description"] = ToolText("search", "param.mode",
                                        "\"grep\" 搜文件内容(正则),\"glob\" 按文件名找文件(通配符)");
    properties["mode"] = mode_prop;

    nlohmann::json pattern_prop = nlohmann::json::object();
    pattern_prop["type"] = "string";
    pattern_prop["description"] =
        ToolText("search", "param.pattern",
                 "mode=grep 时是 ECMAScript 正则表达式;mode=glob 时是文件名通配符(支持 * ? **)。"
                 "不带 '/' 的写法(如 *.md)按文件名匹配,会递归找出整个目录树下所有同名文件,"
                 "不管它在哪层子目录里;带 '/' 的写法(如 src/**/*.hpp、docs/**)按相对路径匹配,"
                 "'**/' 表示零层或多层目录,写在开头就是'不管在不在根目录都算'。");
    properties["pattern"] = pattern_prop;

    nlohmann::json path_prop = nlohmann::json::object();
    path_prop["type"] = "string";
    path_prop["description"] = ToolText("search", "param.path",
                                        "从哪里开始搜:给目录就递归遍历,给单个文件就只搜这一个。"
                                        "不填默认当前工作目录");
    properties["path"] = path_prop;

    nlohmann::json glob_prop = nlohmann::json::object();
    glob_prop["type"] = "string";
    glob_prop["description"] =
        ToolText("search", "param.glob",
                 "仅 mode=grep 有效:按文件名或路径过滤要搜索的文件,不填就搜所有非二进制文件。"
                 "语义跟 pattern 的 glob 写法一样:*.cpp 这种不带 '/' 的按文件名递归匹配任意目录下的文件;"
                 "src/**/*.hpp 这种带 '/' 的按相对路径匹配。");
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
    // 目录、单个文件都收:分发逻辑见 CollectSearchFiles。

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
