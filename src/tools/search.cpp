// search 工具:grep(按正则搜内容)和 glob(按文件名找文件)合成一个工具,
// 用 mode 参数区分,没有拆成两个工具。理由(报告里也会提):
//   1. 两种模式共用同一套"搜索范围、跳过 .git/build/node_modules、跳过
//      二进制文件"的策略与同一条后端执行路,拆开会有大段重复代码。
//   2. 对模型来说,"要不要搜内容"是一句话就能说清楚的选择(mode 参数),
//      不比记两个相似名字的工具更难选。
//   3. 工具目录越精简,模型每次要在 tools 列表里挑选、组装入参的负担越小。
// 权衡:mode=glob 时 pattern 参数其实用不上 grep 的"正则语义",schema 里
// 用同一个字段名承载两种语义,靠 description 说明清楚,不算完美,但换来
// 代码和工具列表都更简单,这里判断利大于弊。
//
// ripgrep 迁移单 P0-5 起的生产执行路(且只有这一条,不留 fallback):
//   ParseSearchRequest(schema/路径) -> BuildSearchPolicy(边界/硬排除)
//   -> BundledRipgrepRunner::Run(ChildProcess 流式 + 四道墙 + 终态裁决)
//   -> FormatSearchOutput(本文件:产品合同——输出形状、截断注明、哨兵文案)。
// 旧 std::regex/自写 walker 内核已按设计单 §2.1 裁决整段删除:SearchTool 管
// 产品合同,ripgrep 管搜索算法;缺 rg 就是稳定错,不静默退回慢内核。

#include "tools/search.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "tools/observation_filter.hpp"  // 观察边界(P2-5):子代理日志/.evidence 默认不搜
#include "tools/path_utils.hpp"
#include "tools/search_ripgrep.hpp"  // ripgrep 后端:P0-5 起唯一执行路
#include "tools/tool_text.hpp"       // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/

namespace lubancode::tools {

namespace {

// ---- 单文件 root 的 glob 闸门(产品合同,不是搜索引擎) --------------------
// 实测 rg 15.2.0:对显式给定的文件参数,-g 的包含项与排除项一概不生效
//(目录遍历才过滤)。旧内核"单文件 path 照样吃 glob 过滤,配不上就不搜"
// 是 A 类合同(test_search 分栏表 #9/#10),这道闸由宿主自己守。
// 匹配器就是旧内核那枚免回溯状态表(* ? ** 语义,长模式确定上界)——只
// 用在"单个文件名配一个 pattern"的闸门上,不参与目录递归的过滤(那是
// rg globset 的地盘);对 []/{},旧内核本就按字面处理,闸门保持同一口径。
bool SingleFileGlobMatches(const std::string& pattern, const std::string& text) {
    const std::size_t rows = pattern.size() + 1;
    const std::size_t cols = text.size() + 1;
    std::vector<unsigned char> matched(rows * cols, 0);
    const auto at = [&](std::size_t p, std::size_t s) -> unsigned char& {
        return matched[p * cols + s];
    };
    at(pattern.size(), text.size()) = 1;

    for (std::size_t p = pattern.size(); p-- > 0;) {
        for (std::size_t s = text.size() + 1; s-- > 0;) {
            if (pattern[p] == '*' && p + 1 < pattern.size() && pattern[p + 1] == '*') {
                const bool boundary = p == 0 || pattern[p - 1] == '/';
                const bool slash_after = p + 2 < pattern.size() && pattern[p + 2] == '/';
                if (boundary && slash_after) {
                    at(p, s) = at(p + 3, s) || (s < text.size() && at(p, s + 1));
                } else {
                    at(p, s) = at(p + 2, s) || (s < text.size() && at(p, s + 1));
                }
            } else if (pattern[p] == '*') {
                at(p, s) = at(p + 1, s) ||
                           (s < text.size() && text[s] != '/' && at(p, s + 1));
            } else if (pattern[p] == '?') {
                at(p, s) = s < text.size() && text[s] != '/' && at(p + 1, s + 1);
            } else {
                const bool trailing_globstar = pattern[p] == '/' && p + 3 == pattern.size() &&
                                               pattern[p + 1] == '*' && pattern[p + 2] == '*';
                at(p, s) = (trailing_globstar && s == text.size()) ||
                           (s < text.size() && pattern[p] == text[s] && at(p + 1, s + 1));
            }
        }
    }
    return at(0, 0) != 0;
}

// 单文件 root 且带 glob 过滤时:文件名配不上就直接走"没搜到"哨兵,
// 不起 rg(起也是白起——rg 对显式文件参数不做 glob 过滤)。配得上照常
// 搜。返回 true = 闸门拦下,调用方直接出哨兵。
bool SingleFileGlobGateBlocks(const SearchRequest& request) {
    if (!request.root_is_single_file) {
        return false;
    }
    const std::string filename = PathToUtf8(request.root.filename());
    if (request.mode == SearchMode::Grep) {
        return !request.glob.empty() && !SingleFileGlobMatches(request.glob, filename);
    }
    return !SingleFileGlobMatches(request.pattern, filename);
}

// ---- 请求解析(设计单 6.1:schema 与路径归这一层,后端只见 typed 结构) ----

struct ParsedSearchInput {
    SearchRequest request;
};

// schema -> typed request。只管参数形状与路径解析,不碰后端。
// 返回 nullopt 时 error 已是给模型看的稳定人话。
std::optional<ParsedSearchInput> ParseSearchRequest(const nlohmann::json& input, std::string* error) {
    if (!input.contains("mode") || !input.at("mode").is_string()) {
        *error = "缺少必填参数 mode(字符串,\"grep\" 或 \"glob\")";
        return std::nullopt;
    }
    if (!input.contains("pattern") || !input.at("pattern").is_string()) {
        *error = "缺少必填参数 pattern(字符串)";
        return std::nullopt;
    }
    const std::string mode = input.at("mode").get<std::string>();
    const std::string pattern = input.at("pattern").get<std::string>();
    if (pattern.empty()) {
        *error = "pattern 不能是空字符串";
        return std::nullopt;
    }

    SearchRequest request;
    if (mode == "grep") {
        request.mode = SearchMode::Grep;
    } else if (mode == "glob") {
        request.mode = SearchMode::Glob;
    } else {
        *error = "mode 只认 \"grep\" 或 \"glob\",你写的是: " + mode;
        return std::nullopt;
    }
    request.pattern = pattern;

    std::filesystem::path root = std::filesystem::current_path();
    if (auto it = input.find("path"); it != input.end() && !it->is_null() && it->is_string()) {
        const std::string path_str = it->get<std::string>();
        if (!path_str.empty()) {
            root = Utf8ToPath(path_str);
        }
    }
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
        *error = "path 不存在: " + PathToUtf8(root);
        return std::nullopt;
    }
    // 目录、单个文件都收:单文件把"只搜这一个"显式写进 request(builder 是
    // 纯函数,不碰盘),后端据此设 cwd=父目录、scope=文件名。
    request.root = root;
    request.root_is_single_file = std::filesystem::is_regular_file(root, ec);

    if (request.mode == SearchMode::Grep) {
        if (auto it = input.find("glob"); it != input.end() && !it->is_null() && it->is_string()) {
            request.glob = it->get<std::string>();
        }
        if (auto it = input.find("fixed_strings"); it != input.end() && !it->is_null()) {
            if (!it->is_boolean()) {
                *error = "fixed_strings 只认布尔 true/false";
                return std::nullopt;
            }
            request.fixed_strings = it->get<bool>();
        }
    }
    if (auto it = input.find("max_results"); it != input.end() && !it->is_null()) {
        if (!it->is_number_integer() || it->get<long long>() <= 0) {
            *error = "max_results 只认正整数(软请求,只能调低不能调高,缺省 100)";
            return std::nullopt;
        }
        request.max_results = static_cast<std::size_t>(it->get<long long>());
    }
    return ParsedSearchInput{std::move(request)};
}

// ---- 输出投影(产品合同:形状、截断注明、哨兵文案,全在宿主这边) ----------

// 后端错误 -> 给模型看的文本。稳定码留在正文里(search_* 可 grep),人话
// 跟在后面;取消保持旧合同的"搜索已取消"口吻。
std::string BackendErrorText(const SearchBackendErrorInfo& info) {
    if (info.code == SearchBackendError::Cancelled) {
        return "搜索已取消(search_cancelled)";
    }
    return "搜索失败(" + std::string(ToString(info.code)) + "): " + info.message;
}

// 单文件点名到观察边界内 = 显式点名,照常搜,但正文前给一行体积提示
//(超过 256KB 劝阻)。目录根不用提示——排除策略在 walker 里,边界内的
// 文件默认根本进不来。
std::string ObservationNoticeForRoot(const SearchRequest& request) {
    if (!request.root_is_single_file) {
        return std::string();
    }
    std::error_code size_ec;
    const std::uintmax_t size = std::filesystem::file_size(request.root, size_ec);
    if (size_ec) {
        return std::string();
    }
    return ObservationReadNotice(request.root, size);
}

Tool::Result FormatSearchOutput(const SearchRequest& request, const RipgrepRunResult& run) {
    const std::string notice = ObservationNoticeForRoot(request);

    if (run.hits.empty()) {
        // 无命中(exit 1)是成功:rg 的合同与旧内核一致,回"没搜到"哨兵。
        // 截断不可能与零命中同时发生(四道墙只截已到手的结果)。
        const char* sentinel = request.mode == SearchMode::Grep ? "没搜到匹配的内容" : "没找到匹配的文件";
        return {notice + sentinel, false};
    }

    std::string out = notice;
    if (request.mode == SearchMode::Grep) {
        for (const SearchHit& hit : run.hits) {
            out += hit.path;
            out += ':';
            out += std::to_string(hit.line_number);
            out += ':';
            out += hit.text;
            out += '\n';
        }
    } else {
        for (const SearchHit& hit : run.hits) {
            out += hit.path;
            out += '\n';
        }
    }
    if (run.truncated) {
        out += "……(结果超过 " + std::to_string(run.hits.size()) +
               " 条,已截断,建议缩小 pattern 或 path 范围)\n";
    }
    return {out, false};
}

}  // namespace

std::string SearchTool::name() const {
    return "search";
}

SearchTool::SearchTool() : ripgrep_runner_(std::make_shared<BundledRipgrepRunner>()) {
    // P0-5 起默认构造即生产装配:持随包 ripgrep runner(定位只认
    // exe-dir/libexec,构造零动作,smoke 懒做)。没有第二条执行路。
}

SearchTool::SearchTool(std::shared_ptr<IRipgrepRunner> ripgrep_runner)
    : ripgrep_runner_(std::move(ripgrep_runner)) {
    // 装配注入口:生产传 BundledRipgrepRunner(默认构造已带),单测传 fake。
}

std::string SearchTool::description() const {
    // 文案在 src/prompts/tools/<语言>/search.md,兜底与 zh-CN 档同文。
    return ToolText("search", "description",
                    "在目录或单个文件里搜索,两种模式:mode=\"grep\" 按正则(Rust regex 语法,与 ripgrep 同款)搜文件内容,"
                    "命中的行按 文件:行号:行内容 返回;mode=\"glob\" 按文件名通配(支持 * ? **)找文件,"
                    "返回相对路径列表。默认从当前工作目录开始搜,遵守 .gitignore/.ignore/.rgignore,"
                    "自动跳过 .git/、build/、node_modules/、.evidence/(运行时观察记录)和二进制文件;"
                    "隐藏的项目文件(如 .github/、.clang-format)仍可搜。要搜被忽略的文件或观察记录,"
                    "把 path 逐字点名到具体文件或目录即可。结果默认超过 100 条会截断并注明"
                    "(可用 max_results 提前声明要多少);单条超长命中行截断到 16 KiB。"
                    "搜索一律用本工具,不要在 run_command 里跑 rg 或 grep。");
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
                 "mode=grep 时是 Rust regex 正则表达式(ripgrep 同款:不支持 lookahead 与 backreference,"
                 "支持 \\p{Han} 等 Unicode 属性转义;要按字面搜正则元字符,配 fixed_strings=true);"
                 "mode=glob 时是文件名通配符(支持 * ? **,ripgrep globset 语法)。"
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

    nlohmann::json fixed_prop = nlohmann::json::object();
    fixed_prop["type"] = "boolean";
    fixed_prop["description"] =
        ToolText("search", "param.fixed_strings",
                 "仅 mode=grep 有效:true 时 pattern 按字面量逐字匹配,不解析正则元字符;"
                 "默认 false,按 Rust regex 语法解析");
    properties["fixed_strings"] = fixed_prop;

    nlohmann::json max_results_prop = nlohmann::json::object();
    max_results_prop["type"] = "integer";
    max_results_prop["minimum"] = 1;
    max_results_prop["maximum"] = 100;
    max_results_prop["description"] =
        ToolText("search", "param.max_results",
                 "事前声明这次要多少条结果(软请求,只能调低不能调高):到数即停并注明已截断,"
                 "缺省 100 条封顶。grep 按命中行计,glob 按文件计");
    properties["max_results"] = max_results_prop;

    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"mode", "pattern"});

    return schema;
}

namespace {

// 执行主路(两条 execute 重载共用):解析 -> 策略 -> runner -> 投影。
Tool::Result ExecuteSearchImpl(const nlohmann::json& input,
                               std::shared_ptr<IRipgrepRunner>& runner,
                               const ToolExecutionContext& context) {
    std::string parse_error;
    const std::optional<ParsedSearchInput> parsed = ParseSearchRequest(input, &parse_error);
    if (!parsed.has_value()) {
        return {parse_error, true};
    }
    // runner 缺位只可能是装配断了(生产默认构造必带):按缺件稳定错收口,
    // 不静默退回任何本地内核——那条禁路没有分支可走。
    if (runner == nullptr) {
        return {BackendErrorText(SearchBackendErrorInfo{
                    SearchBackendError::BackendMissing, "search 后端未装配(安装损坏)"}),
                true};
    }

    const SearchRequest& request = parsed->request;
    // 单文件 glob 闸门(A 类合同 #9/#10):配不上就不搜,直接走哨兵文案。
    if (SingleFileGlobGateBlocks(request)) {
        const char* sentinel =
            request.mode == SearchMode::Grep ? "没搜到匹配的内容" : "没找到匹配的文件";
        return {sentinel, false};
    }
    const SearchPolicy policy = BuildSearchPolicy(request);

    const auto run = runner->Run(request, policy, context);
    if (!run.has_value()) {
        return {BackendErrorText(run.error()), true};
    }
    return FormatSearchOutput(request, *run);
}

}  // namespace

Tool::Result SearchTool::execute(const nlohmann::json& input) {
    return ExecuteSearchImpl(input, ripgrep_runner_, ToolExecutionContext{});
}

Tool::Result SearchTool::execute(const nlohmann::json& input, const ToolExecutionContext& context) {
    return ExecuteSearchImpl(input, ripgrep_runner_, context);
}

}  // namespace lubancode::tools
