// search 工具 ripgrep 后端合同的实现(P0-2/P0-3 部分):定位器、文件校验、
// 版本 smoke、argv 纯函数构造器与边界策略。流式执行(ChildProcess 起真 rg、
// JSONL/NUL 分帧、四道墙、终态裁决)在 search_ripgrep_run.cpp。

#include "tools/search_ripgrep.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <system_error>
#include <utility>

#include "platform/paths.hpp"    // platform::ExecutablePath:定位随包 rg
#include "platform/process.hpp"  // RunProcess:smoke 的 --version 真探针
#include "tools/observation_filter.hpp"
#include "tools/path_utils.hpp"  // PathToUtf8(tools 层共用口)

#ifdef _WIN32
#include <wchar.h>  // _wcsicmp
#else
#include <unistd.h>  // access(X_OK)
#endif

namespace lubancode::tools {

// ---- 稳定错误码 -----------------------------------------------------------

std::string_view ToString(SearchBackendError error) {
    switch (error) {
        case SearchBackendError::BackendMissing:
            return "search_backend_missing";
        case SearchBackendError::NotExecutable:
            return "search_backend_not_executable";
        case SearchBackendError::VersionMismatch:
            return "search_backend_version_mismatch";
        case SearchBackendError::SpawnFailed:
            return "search_backend_spawn_failed";
        case SearchBackendError::ProtocolError:
            return "search_backend_protocol_error";
        case SearchBackendError::PatternInvalid:
            return "search_pattern_invalid";
        case SearchBackendError::Cancelled:
            return "search_cancelled";
        case SearchBackendError::Timeout:
            return "search_timeout";
        case SearchBackendError::OutputLimit:
            return "search_output_limit";
        case SearchBackendError::RunFailed:
            return "search_backend_run_failed";
    }
    return "search_backend_unknown";
}

std::string_view ToString(RipgrepSmokeStatus status) {
    switch (status) {
        case RipgrepSmokeStatus::Ready:
            return "ready";
        case RipgrepSmokeStatus::Missing:
            return "missing";
        case RipgrepSmokeStatus::NotExecutable:
            return "not_executable";
        case RipgrepSmokeStatus::VersionMismatch:
            return "version_mismatch";
        case RipgrepSmokeStatus::SmokeFailed:
            return "smoke_failed";
    }
    return "unknown";
}

std::string_view ToString(RipgrepSource source) {
    switch (source) {
        case RipgrepSource::Bundled:
            return "bundled";
        case RipgrepSource::UserStage:
            return "rg-stage";
        case RipgrepSource::Path:
            return "path";
    }
    return "unknown";
}

// ---- 定位器(第 1 层:只拼 ExecutableDir/libexec,不搜 PATH 不读 env) --------

std::optional<std::filesystem::path> BundledRipgrepLocator::BundledRipgrepPath() {
    // 唯一的一条:exe 同目录 libexec/ 下。不搜 PATH、不读任何环境变量、
    // 不猜 cwd。ExecutablePath 拿不到时如实缺件,不退而求其次。
    const std::optional<std::filesystem::path> exe = platform::ExecutablePath();
    if (!exe.has_value()) {
        return std::nullopt;
    }
#ifdef _WIN32
    return exe->parent_path() / "libexec" / "rg.exe";
#else
    return exe->parent_path() / "libexec" / "rg";
#endif
}

BundledRipgrepLocator::BundledRipgrepLocator(std::filesystem::path exe_override)
    : exe_override_(std::move(exe_override)) {}

std::optional<std::filesystem::path> BundledRipgrepLocator::Locate() const {
    if (exe_override_.has_value()) {
        return exe_override_;  // 注入路径原样奉还:测试要看的就是"不回退"
    }
    return BundledRipgrepPath();
}

RipgrepFileStatus CheckRipgrepFile(const std::filesystem::path& exe) {
    std::error_code ec;
    if (!std::filesystem::exists(exe, ec)) {
        return RipgrepFileStatus::Missing;
    }
    if (!std::filesystem::is_regular_file(exe, ec)) {
        // 目录、设备、残链:件在,但不是能起进程的东西。
        return RipgrepFileStatus::NotExecutable;
    }
#ifdef _WIN32
    // Windows 没有执行位,按"CreateProcess 认的形态"判:可执行文件扩展名。
    // 这道预检挡的是"libexec/rg 被写成了解压出的 README"这类安装损坏。
    const std::wstring native = exe.wstring();
    return native.size() >= 4 &&
                   _wcsicmp(native.c_str() + native.size() - 4, L".exe") == 0
               ? RipgrepFileStatus::Ok
               : RipgrepFileStatus::NotExecutable;
#else
    const std::string native = exe.string();
    return access(native.c_str(), X_OK) == 0 ? RipgrepFileStatus::Ok
                                             : RipgrepFileStatus::NotExecutable;
#endif
}

// ---- 三层发现(搜索兜底单) --------------------------------------------------

namespace {

// 本平台的 rg 可执行名(Windows 认 .exe 形态,POSIX 认裸名)。
std::filesystem::path RipgrepExecutableName() {
#ifdef _WIN32
    return std::filesystem::path(L"rg.exe");
#else
    return std::filesystem::path("rg");
#endif
}

}  // namespace

std::vector<RipgrepCandidate> CollectRipgrepCandidates() {
    std::vector<RipgrepCandidate> out;

    // 第 1 层:随包(exe 旁 libexec)。exe 路径都解析不到时这层缺席,
    // 不猜 cwd、不退环境变量。
    if (const std::optional<std::filesystem::path> bundled = BundledRipgrepLocator::BundledRipgrepPath()) {
        out.push_back({*bundled, RipgrepSource::Bundled});
    }

    // 第 2 层:用户级 rg-stage(scripts/fetch_ripgrep.py 的一次性产物,
    // 全机共享;构建侧 CMake 早就在探同一路径当离线分期兜底)。
    if (const std::optional<std::string> home = platform::HomeDir()) {
        out.push_back({platform::Utf8ToPath(*home) / ".lubancode" / "rg-stage" / "libexec" /
                           RipgrepExecutableName(),
                       RipgrepSource::UserStage});
    }

    // 第 3 层:系统 PATH 的 rg,逐项展开(同层按 PATH 顺序)。空的 PATH
    // 条目跳过;条目按原字节当 UTF-8 解,解不动的目录自然错过。
    if (const std::optional<std::string> path_value = platform::GetEnvVar("PATH")) {
#ifdef _WIN32
        constexpr char kSeparator = ';';
#else
        constexpr char kSeparator = ':';
#endif
        std::size_t begin = 0;
        while (begin <= path_value->size()) {
            const std::size_t end = path_value->find(kSeparator, begin);
            const std::string entry =
                path_value->substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            if (!entry.empty()) {
                out.push_back({platform::Utf8ToPath(entry) / RipgrepExecutableName(), RipgrepSource::Path});
            }
            if (end == std::string::npos) {
                break;
            }
            begin = end + 1;
        }
    }
    return out;
}

RipgrepDiscovery DiscoverRipgrep(const std::vector<RipgrepCandidate>& candidates) {
    RipgrepDiscovery out;
    for (const RipgrepCandidate& candidate : candidates) {
        RipgrepTierStatus tier;
        tier.candidate = candidate;
        tier.status = CheckRipgrepFile(candidate.exe);
        // 第一枚 Ok 记命中,但账照走全量——三层态势是 doctor 与全缺报错
        // 的共同材料,stat 几十枚 PATH 条目不值一提。
        if (tier.status == RipgrepFileStatus::Ok && !out.hit.has_value()) {
            out.hit = candidate;
        }
        out.tiers.push_back(std::move(tier));
    }
    return out;
}

std::string FormatRipgrepAllMissingGuidance(const std::vector<RipgrepTierStatus>& tiers) {
    // 逐层聚账:随包/rg-stage 各一行;PATH 聚一行(逐项倒出来是一屏噪声)。
    auto describe = [&tiers](RipgrepSource source) {
        std::string text;
        std::size_t path_entries = 0;
        std::size_t path_present = 0;
        for (const RipgrepTierStatus& tier : tiers) {
            if (tier.candidate.source != source) {
                continue;
            }
            if (source == RipgrepSource::Path) {
                ++path_entries;
                if (tier.status != RipgrepFileStatus::Missing) {
                    ++path_present;
                }
                continue;
            }
            if (!text.empty()) {
                text += ";";
            }
            text += PathToUtf8(tier.candidate.exe);
            text += tier.status == RipgrepFileStatus::Missing ? "(缺)"
                    : tier.status == RipgrepFileStatus::NotExecutable ? "(在,不可执行)"
                                                                      : "(可用)";
        }
        if (source == RipgrepSource::Path) {
            return "系统 PATH 共 " + std::to_string(path_entries) + " 项," +
                   std::to_string(path_present) + " 处有 rg 但不可用";
        }
        return text;
    };

    std::string out = "ripgrep 三层全缺,search 后端不可用。已探:随包 libexec " +
                      describe(RipgrepSource::Bundled) + ";用户级 rg-stage " +
                      describe(RipgrepSource::UserStage) + ";" + describe(RipgrepSource::Path) +
                      "。修复:跑 /doctor search 看逐层诊断;或手动补一份用户级 staging(一次,全机共享):\n"
                      "  python scripts/fetch_ripgrep.py --target <home>/.lubancode/rg-stage\n"
                      "补齐后无需随包,search 自动兜底命中。";
    return out;
}

// ---- 版本 smoke -------------------------------------------------------------

RipgrepVersionProbe DefaultRipgrepVersionProbe() {
    return [](const std::filesystem::path& exe) -> std::expected<std::string, SearchBackendErrorInfo> {
        // argv[0] 给绝对路径:Windows CreateProcessW(应用名空、命令行首 token
        // 含目录路径)不搜 PATH,POSIX execvp 对含 '/' 的名字不搜 PATH——
        // PATH 前排放一枚假 rg 也轮不到它起。RunProcess 不经过任何 shell,
        // 参数原样传递,无引号/转义层。
        const std::vector<std::string> argv = {PathToUtf8(exe), "--version"};
        const platform::ProcessResult run =
            platform::RunProcess(argv, /*timeout_ms=*/10'000);
        if (run.spawn_failed) {
            return std::unexpected(SearchBackendErrorInfo{
                SearchBackendError::SpawnFailed, "起 rg --version 失败: " + run.spawn_error});
        }
        if (run.timed_out || run.cancelled) {
            return std::unexpected(SearchBackendErrorInfo{
                SearchBackendError::SpawnFailed, "rg --version 超时或被取消"});
        }
        if (run.exit_code != 0) {
            return std::unexpected(SearchBackendErrorInfo{
                SearchBackendError::SpawnFailed,
                "rg --version 退出码 " + std::to_string(run.exit_code)});
        }
        return run.output;
    };
}

std::optional<std::string> ParseRipgrepVersion(const std::string& version_output) {
    // 首行形如 "ripgrep 15.2.0 (rev e89fff89ac)"(rev 段随构建变,不钉)。
    // 版本号取首行第二枚记号,精确比对由调用方做。
    const std::size_t line_end = version_output.find('\n');
    std::string first = version_output.substr(0, line_end);
    if (!first.empty() && first.back() == '\r') {
        first.pop_back();
    }
    constexpr std::string_view kProgram = "ripgrep ";
    if (first.rfind(kProgram.data(), 0) != 0) {
        return std::nullopt;
    }
    const std::size_t version_begin = kProgram.size();
    const std::size_t version_end = first.find(' ', version_begin);
    std::string version =
        first.substr(version_begin, version_end == std::string::npos ? std::string::npos
                                                                     : version_end - version_begin);
    if (version.empty()) {
        return std::nullopt;
    }
    return version;
}

RipgrepSmokeResult RunRipgrepSmoke(const std::filesystem::path& exe, const RipgrepVersionProbe& probe) {
    RipgrepSmokeResult out;
    out.exe = exe;

    const RipgrepFileStatus file_status = CheckRipgrepFile(exe);
    if (file_status == RipgrepFileStatus::Missing) {
        out.status = RipgrepSmokeStatus::Missing;
        out.code = SearchBackendError::BackendMissing;
        out.message = "ripgrep 缺件: " + PathToUtf8(exe);
        return out;
    }
    if (file_status == RipgrepFileStatus::NotExecutable) {
        out.status = RipgrepSmokeStatus::NotExecutable;
        out.code = SearchBackendError::NotExecutable;
        out.message = "ripgrep 不可执行(安装损坏): " + PathToUtf8(exe);
        return out;
    }

    const RipgrepVersionProbe effective = probe != nullptr ? probe : DefaultRipgrepVersionProbe();
    const std::expected<std::string, SearchBackendErrorInfo> output = effective(exe);
    if (!output.has_value()) {
        out.status = RipgrepSmokeStatus::SmokeFailed;
        out.code = output.error().code;
        out.message = output.error().message;
        return out;
    }
    const std::optional<std::string> parsed = ParseRipgrepVersion(*output);
    if (!parsed.has_value()) {
        // 件在、起得来,但吐的不是 rg 的版本行:按身份/版本不合报,提醒用户
        // 包里的件坏了,不当普通冒烟失败吞掉。输出截短——垃圾可能一屏。
        std::string head = output->substr(0, 120);
        out.status = RipgrepSmokeStatus::VersionMismatch;
        out.code = SearchBackendError::VersionMismatch;
        out.message = "rg --version 输出认不出 ripgrep 版本行: " + head;
        return out;
    }
    out.found_version = *parsed;
    if (*parsed != kBundledRipgrepVersion) {
        out.status = RipgrepSmokeStatus::VersionMismatch;
        out.code = SearchBackendError::VersionMismatch;
        out.message = "ripgrep 版本不合: 要 " + std::string(kBundledRipgrepVersion) + ",实得 " + *parsed;
        return out;
    }
    out.status = RipgrepSmokeStatus::Ready;
    out.message = "ripgrep " + *parsed + " 就绪";
    return out;
}

// ---- 生产 runner 的流式执行在 search_ripgrep_run.cpp(P0-4):这里只留
// 定位/smoke/argv/策略;runner 的构造、smoke 缓存与 Run 挪去那边,与
// ChildProcess 分帧、终态裁决住一处。

// ---- argv 纯函数构造器 ------------------------------------------------------

namespace {

std::string NormalizeSlashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

// scope/cwd 分发:单文件 root 把文件名当 scope、父目录当 cwd;目录 root 用
// "." 与 root 本身。这样 rg 输出的路径天然接近旧合同(相对 root),parser
// 再剥开头的 "./" 即可(那是 P0-4 的事)。
struct ScopeAndCwd {
    std::string scope;
    std::string cwd_utf8;
};

ScopeAndCwd ResolveScopeAndCwd(const SearchRequest& request) {
    if (request.root_is_single_file) {
        return {PathToUtf8(request.root.filename()), PathToUtf8(request.root.parent_path())};
    }
    return {".", PathToUtf8(request.root)};
}

}  // namespace

// 绝不由任何代码路径生成的 rg flag(设计单 4.4 首版不开放清单里没有 policy
// 开关的那一半):--pre/--pre-glob 会起外部程序,--search-zip 扩搜索面,
// --engine 除 default 外/-u/--pcre2 会换掉线性时间引擎,--type-add 开任意
// 类型。builder 源码里没有生成它们的分支,测试扫 argv 钉死这道墙。
// 注意 --follow/--text/--no-ignore 不在此列——它们有 policy 开关
//(follow_symlinks/search_binary/respect_ignore_files),生产 policy 恒默认
//(false/false/true),所以"默认 policy 下不出现"由测试单独钉。
bool IsNeverAllowedRipgrepFlag(std::string_view arg) {
    return arg == "--pre" || arg == "--pre-glob" || arg == "--search-zip" || arg == "--engine" ||
           arg == "--auto-hybrid-regex" || arg == "--type-add" || arg == "-u" || arg == "--unrestricted" ||
           arg == "--pcre2" || arg == "--regexp" || arg == "-e";
}

RipgrepInvocation BuildGrepArgv(const SearchRequest& request, const SearchPolicy& policy,
                                const std::filesystem::path& rg_exe) {
    RipgrepInvocation out;
    out.exe = rg_exe;
    out.args = {
        // 每次调用都带 --no-config:父进程有 RIPGREP_CONFIG_PATH 时,用户的
        // 颜色/preprocessor/编码/额外 glob 也进不来。
        "--no-config",
        "--json",           // grep 走 JSON Lines 事件流(P0-4 分帧)
        "--line-buffered",  // 行级冲刷,满额提前收树时尾巴最小
        "--color=never",    // 输出是数据,不是终端演出
        // --hidden:.github/.clang-format 等项目文件可搜;遵守 .gitignore/
        // .ignore/.rgignore 是 rg 默认(.git 由宿主硬排除,见 BuildSearchPolicy)
        "--hidden",
        // 默认 Rust regex 引擎:线性时间边界正是本次迁移买来的东西,
        // look-around/backreference 不开 PCRE2 后门
        "--engine=default",
        "--no-multiline",  // 命中按行计,不开多行模式
    };
    if (!policy.include_hidden) {
        // 去掉 --hidden(策略位,宿主构造;生产恒 true)
        out.args.erase(std::find(out.args.begin(), out.args.end(), "--hidden"));
    }
    if (request.fixed_strings) {
        out.args.push_back("--fixed-strings");
    }
    if (!request.glob.empty()) {
        // 用户正向 glob:首字符 '!' 转义成 [!],按字面匹配,不许偷变排除
        out.args.push_back("-g");
        out.args.push_back(SanitizeUserIncludeGlob(request.glob));
    }
    for (const std::string& exclude : policy.exclude_globs) {
        if (exclude.empty()) {
            continue;
        }
        out.args.push_back("-g");
        out.args.push_back(exclude);  // 宿主自己生成的排除项,原样(带 !)
    }
    if (policy.respect_ignore_files == false) {
        out.args.push_back("--no-ignore");
    }
    if (policy.follow_symlinks) {
        out.args.push_back("--follow");
    }
    if (policy.search_binary) {
        out.args.push_back("--text");
    }
    const ScopeAndCwd scope = ResolveScopeAndCwd(request);
    out.args.push_back("--");  // 参数边界:后面的 pattern/scope 开头是 '-' 也只是 positional
    out.args.push_back(request.pattern);
    out.args.push_back(scope.scope);
    out.cwd_utf8 = scope.cwd_utf8;
    return out;
}

RipgrepInvocation BuildGlobArgv(const SearchRequest& request, const SearchPolicy& policy,
                                const std::filesystem::path& rg_exe) {
    RipgrepInvocation out;
    out.exe = rg_exe;
    out.args = {
        "--no-config",
        "--files",  // 只枚举文件,不搜内容
        // --files --json 实测仍吐普通路径,必须 --null 按 NUL 分帧——POSIX
        // 文件名可含换行,按换行拆会裂
        "--null",
        "--hidden",
    };
    if (!policy.include_hidden) {
        out.args.erase(std::find(out.args.begin(), out.args.end(), "--hidden"));
    }
    // 用户 pattern 是正向过滤(glob 模式没有独立 glob 字段)。空 pattern
    // 不生成 -g(枚举全部文件)——schema 层 pattern 必填,这里是纯函数的
    // 防御位,不替调用方报错。
    if (!request.pattern.empty()) {
        out.args.push_back("-g");
        out.args.push_back(SanitizeUserIncludeGlob(request.pattern));
    }
    for (const std::string& exclude : policy.exclude_globs) {
        if (exclude.empty()) {
            continue;
        }
        out.args.push_back("-g");
        out.args.push_back(exclude);
    }
    if (policy.respect_ignore_files == false) {
        out.args.push_back("--no-ignore");
    }
    if (policy.follow_symlinks) {
        out.args.push_back("--follow");
    }
    if (policy.search_binary) {
        out.args.push_back("--text");
    }
    const ScopeAndCwd scope = ResolveScopeAndCwd(request);
    out.args.push_back("--");
    out.args.push_back(scope.scope);
    out.cwd_utf8 = scope.cwd_utf8;
    return out;
}

// ---- glob 转义与观察边界排除 ------------------------------------------------

std::string EscapeGlobLiteral(const std::string& text) {
    // globset 元字符逐个前置反斜杠;输出只当字面。用于把"路径片段"变 glob
    // (观察边界目录名),不是处理用户写的 glob(用户 glob 用 globset 语法,
    // 语法本身不替用户转义)。
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '\\':
            case '*':
            case '?':
            case '[':
            case ']':
            case '{':
            case '}':
            case '!':
                out += '\\';
                out += c;
                break;
            default:
                out += c;
        }
    }
    return out;
}

std::string SanitizeUserIncludeGlob(const std::string& glob) {
    // globset 只在 pattern 开头认 '!'(排除规则);用户意图是正向过滤,
    // 开头的 '!' 必须按字面转义成 [!](纯字符类,匹配一个字面 '!')。
    // 中间的 '!' 本就是字面,不动。
    if (!glob.empty() && glob.front() == '!') {
        return "[!]" + glob.substr(1);
    }
    return glob;
}

namespace {

// 与 observation_filter.cpp 的 NormalizeAbsolute 同款(那边在匿名命名空间,
// 不-export):weakly_canonical 失败退 absolute,两侧同一道才比得齐。
std::filesystem::path NormalizeForBoundary(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return canonical;
    }
    std::error_code abs_ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, abs_ec);
    if (!abs_ec) {
        return absolute.lexically_normal();
    }
    return path.lexically_normal();
}

// 路径段相等(Windows 盘不区分大小写,按小写比;POSIX 按原文)。同款思路
// 来自 observation_filter.cpp 的 SamePathComponent。
bool SameSegment(const std::filesystem::path& left, const std::filesystem::path& right) {
#ifdef _WIN32
    std::string l = PathToUtf8(left);
    std::string r = PathToUtf8(right);
    std::transform(l.begin(), l.end(), l.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return l == r;
#else
    return left == right;
#endif
}

}  // namespace

std::vector<std::string> BuildObservationExcludes(const std::filesystem::path& root,
                                                  const std::vector<std::filesystem::path>& registered_dirs) {
    // 只收落在 root 之下的目录(设计单 5.4.2);root 之外的登记与本轮搜索
    // 无关,不生成(rg glob 相对 cwd,生成也咬不到,白噪声)。
    std::vector<std::string> out;
    const std::filesystem::path root_abs = NormalizeForBoundary(root);
    for (const std::filesystem::path& dir : registered_dirs) {
        const std::filesystem::path dir_abs = NormalizeForBoundary(dir);
        // 前缀判断:dir 逐段以 root 开头,且 dir 比 root 长(严格在之下)。
        auto root_it = root_abs.begin();
        auto dir_it = dir_abs.begin();
        while (root_it != root_abs.end()) {
            if (dir_it == dir_abs.end() || !SameSegment(*dir_it, *root_it)) {
                break;
            }
            ++root_it;
            ++dir_it;
        }
        if (root_it != root_abs.end() || dir_it == dir_abs.end()) {
            continue;  // root 不是 dir 的前缀,或 dir == root(显式点名,不进来)
        }
        std::error_code ec;
        const std::filesystem::path rel = std::filesystem::relative(dir_abs, root_abs, ec);
        if (ec || rel.empty() || rel == ".") {
            continue;
        }
        out.push_back("!" + EscapeGlobLiteral(NormalizeSlashes(PathToUtf8(rel))) + "/**");
    }
    // 排序去重:登记账已规范化并过重,这里再兜一层,保 argv 跨调用确定。
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

SearchPolicy BuildSearchPolicy(const SearchRequest& request) {
    SearchPolicy policy;  // NSDMI 即生产值:--hidden、尊重 ignore、不跟链接、跳二进制

    // 硬排除:任意深度的 .git/build/node_modules/.evidence 目录——与旧内核
    // SkipDirNames 同一张表。globset 语义:排除项必须带 `!` 前缀
    // (`-g '!**/<名>/**'`);裸 `**/<名>/**` 在 rg 眼里是"只搜这些目录"的
    // 包含项,P0-4 真机差分实翻过这车(搜出来的命中恰好只剩 .git/build
    // 里的)——观察边界的排除项一直是带 ! 的,硬排除这四条当年漏了前缀。
    // .evidence 在这里承担"名字口径"(与 ObservationBoundary 的登记账口径
    // 分工一致:名字不走账,天然常开)。
    for (const char* name : {".git", "build", "node_modules", ".evidence"}) {
        policy.exclude_globs.push_back(std::string("!**/") + name + "/**");
    }

    // 观察边界:root 本身在边界内 = path 显式点名到证据区,不生观察排除,
    // 改走读取提示(与旧内核 WalkFiles 的 root_in_boundary 分支一致);默认
    // 从上层递归时,登记账里落在 root 之下的目录才转排除 glob,真正剪枝。
    if (!PathInObservationBoundary(request.root)) {
        const std::vector<std::filesystem::path> snapshot =
            ObservationBoundary::Instance().ExcludedDirsSnapshot();
        for (std::string& exclude : BuildObservationExcludes(request.root, snapshot)) {
            policy.exclude_globs.push_back(std::move(exclude));
        }
    }
    return policy;
}

}  // namespace lubancode::tools
