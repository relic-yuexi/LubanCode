// search 工具的 ripgrep 后端合同(SearchTool 内置 Ripgrep 后端迁移单
// P0-2/P0-3):typed 请求/策略/结果/错误、随包定位器、runner 接口、argv
// 纯函数构造器与观察边界→排除 glob 的策略构造器。
//
// 本批的红线:SearchTool 的生产行为一字不差——`search` 仍走内置
// std::regex 内核(P0-4 前唯一生产路径),这里落的只是装配注入口与合同。
// 生产定位只有一条:ExecutableDir()/libexec/rg(.exe)。不搜 PATH、不读
// LUBANCODE_RG_PATH、不运行时下载——search 是免确认只读工具,能从 PATH
// 或环境变量捡一枚任意程序,就等于开了一个不经确认的代码执行口。
//
// argv 约定(设计单 4.2/6.3/6.4):
//   - argv 数组直起进程,不经过 cmd.exe/PowerShell//bin/sh,无转义层;
//   - pattern 与 path 落在 `--` 之后,开头是 `-` 也只是 positional;
//   - 用户 glob 作为 `-g <glob>` 的独立值元素跟在 `-g` 后,不会被解析成
//     flag;首字符 `!` 按字面转义成 `[!]`,不许偷变成 rg 的排除规则。

#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tools/tool.hpp"  // ToolExecutionContext:runner 合同的取消旗出口

namespace lubancode::tools {

// ---- 稳定错误码(设计单 7.1) ---------------------------------------------
// 码值即对外字符串(ToString 给的 `search_*` 名),P0-4 起错误路径逐枚接上;
// 本批先钉住缺件/不可执行/版本错/spawn 失败四路。

enum class SearchBackendError {
    BackendMissing,   // search_backend_missing:libexec 下没有 rg
    NotExecutable,    // search_backend_not_executable:件在,不是可执行文件
    VersionMismatch,  // search_backend_version_mismatch:--version 对不上钉死版本
    SpawnFailed,      // search_backend_spawn_failed:起进程失败
    ProtocolError,    // search_backend_protocol_error(P0-4 接)
    PatternInvalid,   // search_pattern_invalid(P0-4 接)
    Cancelled,        // search_cancelled(P0-4 接)
    Timeout,          // search_timeout(P0-4 接)
    OutputLimit,      // search_output_limit(P0-4 接)
    NotWired,         // search_backend_not_wired:本批专用——流式执行是 P0-4,
                      // 前置校验全过之后如实回这枚,不冒充缺件、不偷偷退回旧内核
};

// 稳定字符串码(error_code 落 Tool::Result 用),如 "search_backend_missing"。
std::string_view ToString(SearchBackendError error);

struct SearchBackendErrorInfo {
    SearchBackendError code = SearchBackendError::BackendMissing;
    std::string message;  // 人话诊断(可含路径);给模型看的正文由调用层再洗
};

// ---- typed 请求与策略(设计单 6.1) ----------------------------------------

enum class SearchMode { Grep, Glob };

struct SearchRequest {
    SearchMode mode = SearchMode::Grep;
    std::string pattern;             // grep:Rust regex 语法;glob:用户正向 glob
    std::filesystem::path root;      // 搜索根:目录或单文件(分发见 root_is_single_file)
    bool root_is_single_file = false;  // 单文件 root:cwd=parent、scope=filename。
                                       // is_regular_file 的盘上判断归请求解析层
                                       //(SearchTool 已有同款分发),builder 保持
                                       // 纯函数不碰盘。
    std::string glob;                // 仅 grep:按文件名/路径过滤的正向 glob
    bool fixed_strings = false;      // true -> --fixed-strings(P0-5 起接 schema)
    std::size_t max_results = 100;   // 全局命中行上限(glob 模式按文件计)
};

struct SearchPolicy {
    bool include_hidden = true;         // true -> --hidden(.github 等项目文件可搜)
    bool respect_ignore_files = true;   // true -> 尊重 .gitignore/.ignore/.rgignore(rg 默认)
    bool follow_symlinks = false;       // false -> 不传 --follow(rg 默认不跟)
    bool search_binary = false;         // false -> 不传 --text(跳过二进制,rg 默认)
    std::vector<std::string> exclude_globs;  // 宿主排除项,逐条生成 `-g !<glob>`
};

// ---- 结果形状(设计单 6.2;流式填充在 P0-4,先立合同) -----------------------

struct SearchHit {
    std::string path;      // 相对 root,分隔符统一 '/'
    long long line_number = 0;  // glob 模式无行号,留 0
    std::string text;      // 命中行正文(清洗后)
};

struct RipgrepStats {
    // rg --json summary 事件的诊断字段(P0-4 解析器填),不拿它改命中正文。
    double elapsed_total_secs = 0.0;
    std::uint64_t searches = 0;
    std::uint64_t searches_with_match = 0;
    std::uint64_t matched_lines = 0;
};

struct RipgrepRunResult {
    std::vector<SearchHit> hits;
    bool truncated = false;    // 宿主收满上限主动收树,不是失败
    bool cancelled = false;    // 取消令牌置位收树
    bool timed_out = false;    // 墙钟超时收树
    int exit_code = 0;
    std::string stderr_text;   // 捕获的 stderr(截断到合同上限,P0-4 接)
    std::optional<RipgrepStats> stats;
};

// ---- runner 接口(设计单 6.2) ----------------------------------------------
// SearchTool 默认持生产 BundledRipgrepRunner,单测注入 fake;工具不直接
// new ChildProcess——参数、解析、进程与展示不许揉回一档。

class IRipgrepRunner {
public:
    virtual ~IRipgrepRunner() = default;
    virtual std::expected<RipgrepRunResult, SearchBackendErrorInfo>
    Run(const SearchRequest& request, const SearchPolicy& policy, const ToolExecutionContext& context) = 0;
};

// ---- 随包定位与文件校验(设计单 4.3) ---------------------------------------

// 钉死的随包版本。运行时校验用这枚编译期常量;与 third_party/ripgrep/
// manifest.json 的一致性由 scripts/fetch_ripgrep.py 与 Release 流水线对账,
// doctor 显示不一致时明报。
inline constexpr std::string_view kBundledRipgrepVersion = "15.2.0";

// 定位器:只认 ExecutableDir()/libexec/rg(.exe),一条路。
//   - 不搜 PATH:PATH 前排放一枚假 rg 也轮不到它;
//   - 不读 LUBANCODE_RG_PATH 或任何环境变量;
//   - 不运行时下载、不调系统包管理器。
// 只拼路径,不查盘上是否存在——存在/可执行/版本的校验归 runner 与 doctor
// smoke(见 CheckRipgrepFile / RunRipgrepSmoke)。
class BundledRipgrepLocator {
public:
    // 生产唯一口:ExecutableDir()/libexec/rg.exe(Windows)/rg(POSIX)。
    // ExecutablePath() 拿不到(极罕见:exe 路径解析失败)返回 nullopt,
    // 调用方按缺件报,不猜 cwd、不退 PATH。
    static std::optional<std::filesystem::path> BundledRipgrepPath();

    // 测试/特殊装配注入口:显式指 exe 路径(fake filesystem 效果)。
    // Locate() 仍只回这枚路径,不做任何回退。
    explicit BundledRipgrepLocator(std::filesystem::path exe_override);
    BundledRipgrepLocator() = default;
    std::optional<std::filesystem::path> Locate() const;

private:
    std::optional<std::filesystem::path> exe_override_;
};

// 盘上的 rg 件校验(无版本校验;那要起进程,见 smoke)。
enum class RipgrepFileStatus {
    Ok,             // 常规文件且本平台可执行(Windows:.exe;POSIX:access X_OK)
    Missing,        // 路径上没有东西
    NotExecutable,  // 有东西但不是可执行的常规文件(目录/裸文本/无执行位)
};

RipgrepFileStatus CheckRipgrepFile(const std::filesystem::path& exe);

// ---- 版本 smoke(设计单 P0-2:启动/doctor 执行 --version 精确校版本) -------

// 版本探针:执行 <exe> --version,回 stdout 原始字节。独立出来是为了单测
// 注入假探针——缺件/版本错/spawn fail 的单测不起真进程,稳定可重复。
using RipgrepVersionProbe =
    std::function<std::expected<std::string, SearchBackendErrorInfo>(const std::filesystem::path&)>;

// 默认真探针:RunProcess 起 <exe> --version(argv 直起,绝对路径不经 PATH
// 搜索;超时 10 秒杀树)。输出按 UTF-8 尽力解(版本行是 ASCII,稳)。
RipgrepVersionProbe DefaultRipgrepVersionProbe();

// 解析 `rg --version` 输出:首行第二枚记号是版本("ripgrep 15.2.0 (rev
// e89fff89ac)" -> "15.2.0";首行实测带 rev 段,不钉整行)。认不出(空输出/
// 形状不合)返回 nullopt。
std::optional<std::string> ParseRipgrepVersion(const std::string& version_output);

// doctor 状态字(设计单 7.2 的 ready|missing|version_mismatch|smoke_failed,
// 另加 not_executable——件在但不可执行,对用户是"安装损坏"的更准诊断)。
enum class RipgrepSmokeStatus { Ready, Missing, NotExecutable, VersionMismatch, SmokeFailed };
std::string_view ToString(RipgrepSmokeStatus status);

struct RipgrepSmokeResult {
    RipgrepSmokeStatus status = RipgrepSmokeStatus::Missing;
    std::optional<SearchBackendError> code;  // 稳定错误码;Ready 时为空
    std::string found_version;  // 实测解析出的版本(认不出为空),诊断显示用
    std::string message;        // 人话一句
    std::filesystem::path exe;  // 受检路径(诊断显示;hash 缩略归显示层)
};

// 完整 smoke:存在性 -> 可执行 -> 起进程 -> 版本精确校验。probe 传空用默认
// 真探针;单测传假探针(不起进程)。
RipgrepSmokeResult RunRipgrepSmoke(const std::filesystem::path& exe, const RipgrepVersionProbe& probe = nullptr);

// ---- 生产 runner(装配注入口;流式执行是 P0-4) -----------------------------
// Run 的前置段本批就真做:定位 -> 缺件/不可执行/版本 smoke,四路各有稳定
// 错误码;前置全过之后流式执行未接,如实回 NotWired(P0-4 换实现,P0-5 切
// 主路)。SearchTool::execute 在 P0-5 之前不调用本类——生产路径与从前一字
// 不差,本类是装配口与 doctor 诊断的真账。
class BundledRipgrepRunner : public IRipgrepRunner {
public:
    // exe_override/probe 均为测试注入口(fake filesystem/假探针);生产构造
    // 全默认:定位走 BundledRipgrepPath(),探针走真进程。
    explicit BundledRipgrepRunner(std::filesystem::path exe_override = {},
                                  RipgrepVersionProbe version_probe = nullptr);

    std::expected<RipgrepRunResult, SearchBackendErrorInfo>
    Run(const SearchRequest& request, const SearchPolicy& policy,
        const ToolExecutionContext& context) override;

    // smoke 结果缓存出口(doctor /doctor search 复用,不重复起进程)。返回
    // 拷贝:实例共享、多线程可并发 Run,不往外递内部锁保护下的引用。
    RipgrepSmokeResult smoke_result() const;

private:
    void EnsureSmoke();

    std::optional<std::filesystem::path> exe_override_;
    RipgrepVersionProbe version_probe_;
    mutable std::mutex smoke_mutex_;
    RipgrepSmokeResult smoke_result_;
    bool smoke_done_ = false;
};

// ---- argv 纯函数构造器(设计单 6.3/6.4,P0-3) ------------------------------
// 同一 typed request 三平台同一语义 argv,差别只在 exe 后缀与路径编码。
// 不读全局状态、不碰盘、不看环境——边界测试直接钉 argv。

struct RipgrepInvocation {
    std::filesystem::path exe;     // rg 可执行(调用方给,通常绝对)
    std::vector<std::string> args; // exe 之后的参数(argv[1..])
    std::string cwd_utf8;          // 子进程工作目录(UTF-8);空 = 继承本进程
};

// grep 模式:
//   --no-config --json --line-buffered --color=never --hidden --engine=default
//   --no-multiline [--fixed-strings] [-g <用户 glob 转义>] [-g !<排除>]...
//   -- <pattern> <scope>
// 目录 root:cwd=root,scope="."。单文件 root:cwd=parent,scope=filename。
// include_hidden=false 去 --hidden;respect_ignore_files=false 加 --no-ignore;
// follow_symlinks=true 加 --follow;search_binary=true 加 --text(policy 由
// 宿主构造,生产恒默认值——首版不开放的 rg 能力没有输入面)。
RipgrepInvocation BuildGrepArgv(const SearchRequest& request, const SearchPolicy& policy,
                                const std::filesystem::path& rg_exe);

// glob 模式(--files --json 实测仍吐普通路径,必须 --null 按 NUL 分帧):
//   --no-config --files --null --hidden [-g <用户正向 glob 转义>]
//   [-g !<排除>]... -- <scope>
// 用户 pattern 只作正向过滤:首字符 `!` 转义成 `[!]` 按字面匹配,绝不偷变
// rg 排除规则。
RipgrepInvocation BuildGlobArgv(const SearchRequest& request, const SearchPolicy& policy,
                                const std::filesystem::path& rg_exe);

// ---- 策略构造器(设计单 5.3/5.4,P0-3) ------------------------------------
// 只管观察边界、硬排除与显式点名;读 ObservationBoundary 登记账(全局),
// 输出纯 policy——argv builder 保持纯函数,可测边界都推到这里。
//
// 排除账(与旧内核 WalkFiles/ShouldSkipDir 逐条对账):
//   1. 硬排除任意深度的 .git/build/node_modules/.evidence 目录名
//      (`**/<名>/**`)——旧内核 ShouldSkipDir 的同名行为;
//   2. root 不在观察边界内时,登记账里落在 root 之下的目录转 root-relative
//      排除 glob(`!<rel>/**`)——旧内核 WalkFiles 的边界过滤;
//   3. root 本身在观察边界内 = path 显式点名到证据区,不生观察排除,改走
//      读取提示(与旧内核 root_in_boundary 分支一致);
//   4. 显式点名 build/ 等目录时,排除 glob 按 root-relative 语义天然不再
//      咬根下文件(旧内核同款:点名后照常搜)。
SearchPolicy BuildSearchPolicy(const SearchRequest& request);

// globset 字面转义(设计单 5.4.4 的独立 helper):把路径/名字里的 glob 元
// 字符(`\ * ? [ ] { } !`)逐个前置反斜杠,输出只当字面匹配。测试直查。
std::string EscapeGlobLiteral(const std::string& text);

// 用户正向 glob 的防偷换转义:仅首字符 `!` 换成 `[!]`(globset 只在开头认
// 排除义;中间的 `!` 本就是字面),其余原样——用户 glob 写的是 globset
// 语法(设计单 5.2 明写迁移),语法本身不替用户转义。
std::string SanitizeUserIncludeGlob(const std::string& glob);

// 绝不允许出现在 rg argv 里的 flag(设计单 4.4 首版不开放,且没有 policy
// 开关):--pre/--pre-glob/--search-zip/--engine(非 default)/--pcre2/
// --auto-hybrid-regex/--type-add/-u/--unrestricted/-e/--regexp。builder
// 没有生成它们的分支;导出给测试扫 argv,两处共用一份真账。--follow/
// --text/--no-ignore 有 policy 开关、生产恒关,不在此列。
bool IsNeverAllowedRipgrepFlag(std::string_view arg);

// 观察边界登记目录 -> root-relative 排除 glob 的纯函数段(BuildSearchPolicy
// 的可测内核):只收落在 root 之下的目录,转 UTF-8、统一 '/'、字面转义,
// 逐条产 `!<rel>/**`。root 本身在边界内时调用方不进来(显式点名)。
std::vector<std::string> BuildObservationExcludes(const std::filesystem::path& root,
                                                  const std::vector<std::filesystem::path>& registered_dirs);

}  // namespace lubancode::tools
