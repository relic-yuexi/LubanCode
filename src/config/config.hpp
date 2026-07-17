// 配置来源分四级,优先级从高到低:
//   1) LUBANCODE_ 专属环境变量(LUBANCODE_WIRE / LUBANCODE_BASE_URL /
//      LUBANCODE_API_KEY / LUBANCODE_MODEL / LUBANCODE_MAX_CONTEXT)
//   2) 配置文件(先找 cwd,再找用户主目录;新位置 <目录>/.lubancode/config.json,
//      旧位置 <目录>/.lubancode.json,查找顺序见 LoadFileConfig 注释)
//   3) 通用环境变量(ANTHROPIC_*/OPENAI_*,向后兼容老用法)
//   4) 内置默认值
// 按"字段"逐个决,不是整套配置一刀切——比如配置文件只写了 base_url,
// model 照样能从下一级来。密钥绝不硬编码进源码。
//
// M6.5:配置文件挪进 <主目录>/.lubancode/ 目录(往后还要住 plugins/、skills/,
// 是 lubancode 的家)。读到旧位置文件而新位置不存在时自动迁移(见
// MigrateConfigFileIfNeeded),不留旧新两份配置混着用的糊涂账。
//
// lubancode 是通用工具,不绑死哪一家模型服务:内置默认值只有 wire=anthropic
// 和 max_context_chars,base_url/api_key/model 三个字段没有内置默认值,四级
// 都没配到就是空。空着不算错——MergeConfig 不报错,只是留空;真要跟模型
// 对话之前(交互模式的初次配置向导、单发模式的 RequireConfigured)才会因为
// 缺东西而拦下来。
//
// 加载逻辑拆成两半:纯函数 MergeConfig 只管按优先级合并、不碰任何 IO,
// 好单测;LoadFileConfig/LoadFromEnv 才是真正读环境变量、读磁盘文件的地方。

#pragma once

#include <cstddef>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace lubancode::config {

// 说哪种"方言"跟模型对话:Anthropic 的 Messages API,还是 OpenAI 的
// Responses API。默认 anthropic。
enum class Wire { Anthropic, Responses };

// 一个字段的值最终是从哪一级配置来的,--config 诊断输出用。
enum class Source {
    LubancodeEnv,  // LUBANCODE_ 专属环境变量
    ConfigFile,    // .lubancode.json
    GenericEnv,    // ANTHROPIC_*/OPENAI_* 通用环境变量
    Default,       // 内置默认值
};

// 中文说法,--config 打印用。
std::string ToString(Source source);

// max_context_chars 的内置默认值(字符数)。跟 agent::kDefaultMaxContextChars
// 数值上保持一致,但 config 层不依赖 agent 层(依赖只许单向,cli -> agent ->
// api/tools;config 不该反过来牵扯 agent),所以这里单独定义一份。
constexpr std::size_t kDefaultMaxContextChars = 600000;

// theme 字段的内置默认值:三套内置调色板见 cli/theme.hpp,这里只存名字
// (字符串),不依赖 cli 层(依赖只许单向,config 不该反过来牵扯 cli)。
constexpr const char* kDefaultTheme = "dark";

// context_window(上下文窗口 token 数)的内置默认值:256k,decimal 换算
// (k=1000,m=1,000,000,见 ParseContextWindowTokens 注释)。M6.6 新增,是
// /context、/compact 自动触发用的"窗口大小"依据,跟 max_context_chars
// (字符数、老的硬安全网,单位不同、语义也不同)是两回事。
constexpr std::size_t kDefaultContextWindowTokens = 256000;

// tool_search(延迟挂载)的阈值默认值:注册表总工具数超过这个数才启用
// 延迟机制(超了才把 MCP/插件这类外挂工具改成"检索后挂载");0 = 永不
// 延迟,一切照旧全量直挂。内置工具撑死十几个,默认 20 意味着不挂一堆
// 外挂工具的用户永远走不到延迟这条路,现状行为零变化。
constexpr int kDefaultToolSearchThreshold = 20;

// M9:一条钩子。matcher 只有 pre_tool/post_tool 才有意义——工具名精确匹配,
// 或者 "*" 匹配所有工具;session_start/session_end 没有这个概念,解析时留
// 空串,执行时也不看这个字段。command 是要跑的一整条命令行,直接交给
// cmd.exe 执行(不像 run_command 工具那样还分 powershell/cmd 两种 shell、
// 也不需要用户确认这一步)。
struct HookEntry {
    std::string matcher;
    std::string command;
};

// M9:hooks 配置整体——只从配置文件来,没有环境变量、也没有内置默认值这
// 两级(跟其余字段不一样,config.json 里没写就是空,四个数组都是空的)。
struct HooksConfig {
    std::vector<HookEntry> pre_tool;
    std::vector<HookEntry> post_tool;
    std::vector<HookEntry> session_start;
    std::vector<HookEntry> session_end;

    bool Empty() const {
        return pre_tool.empty() && post_tool.empty() && session_start.empty() && session_end.empty();
    }
};

// websearch:搜索服务配置,只从配置文件来(跟 hooks/mcpServers 同样待遇,
// 没有环境变量、没有内置默认值这两级)。provider 只认 tavily/brave/serper
// 三家;api_key 必填非空。没配这一段,web_search 工具压根不注册(模型
// 看不见,不会瞎调)。api_key 不打日志、展示时走 MaskApiKey 打码。
struct SearchConfig {
    std::string provider;
    std::string api_key;

    bool Configured() const { return !provider.empty() && !api_key.empty(); }
};

// M8:一个 MCP 服务器的配置。command 必填,args/env 可选(缺省空)。
// env 用 vector<pair> 而不是 map——直接对得上 tools::RunProcess 的
// extra_env 入参类型(顺序在这里不重要,但没必要多一趟转换)。
struct McpServerConfig {
    std::string command;
    std::vector<std::string> args;
    std::vector<std::pair<std::string, std::string>> env;
};

// LSP:一个语言服务器的配置。command 必填;args 可选(缺省空);extensions
// 必填(非空字符串数组,按扩展名把文件路由到这个服务器,比如 [".cpp",".h"]);
// idle_minutes 可选(缺省 10,闲置多少分钟后关停进程,下次用到再拉起)。
// 键是语言名("cpp"/"python"...),同时充当 textDocument/didOpen 的
// languageId。跟 hooks/mcpServers 一样只从配置文件来——没配置 = 不启用 =
// lsp 工具不注册。
struct LspServerConfig {
    std::string command;
    std::vector<std::string> args;
    std::vector<std::string> extensions;
    int idle_minutes = 10;
};

struct Config {
    Wire wire = Wire::Anthropic;
    std::string base_url;
    std::string auth_token;  // 即 api_key
    std::string model;
    std::size_t max_context_chars = kDefaultMaxContextChars;
    std::string theme = kDefaultTheme;   // dark / light / plain,没配到就是 kDefaultTheme
    std::string system_prompt_file;      // --system-prompt / system_prompt_file,没配到就是空串
    std::size_t context_window_tokens = kDefaultContextWindowTokens;  // M6.6:上下文窗口 token 数
    std::string compact_model;  // M6.6:压缩用的模型,空串 = 跟当前会话模型一致
    std::string think;          // M6.6:推理强度,none/low/medium/high,空串 = 不发这个参数
    // 魂法分家:启动时用哪个"魂"(风格叠加层)。空串或 "default" = 主目录
    // 的 SOUL.md;"off" = 不叠加;别的名字 = souls/<名字>.md。/soul 切换后
    // 选"写进配置"就记在这儿。
    std::string soul;
    HooksConfig hooks;          // M9:钩子,只从配置文件读,没配就是四个空数组
    // M8:MCP 服务器,键是服务器名,只从配置文件来(跟 hooks 一样没有
    // 环境变量、没有内置默认值这两级),没配就是空 map。
    std::map<std::string, McpServerConfig> mcp_servers;
    // websearch:搜索服务,只从配置文件来,没配就是空的(Configured()=false,
    // web_search 工具不注册)。
    SearchConfig search;
    // LSP:语言服务器,键是语言名,只从配置文件来(待遇同 hooks/
    // mcpServers),没配就是空 map——空 map 意味着 lsp 工具不注册。
    std::map<std::string, LspServerConfig> lsp_servers;
    // tool_search:延迟挂载的启用阈值,0 = 永不延迟。只从配置文件读
    // (没有环境变量这一级),没配就是默认 20。
    int tool_search_threshold = kDefaultToolSearchThreshold;
};

// 每个字段最终来自哪一级,跟 Config 里的字段一一对应。
struct ConfigSources {
    Source wire = Source::Default;
    Source base_url = Source::Default;
    Source auth_token = Source::Default;
    Source model = Source::Default;
    Source max_context_chars = Source::Default;
    Source theme = Source::Default;
    Source system_prompt_file = Source::Default;
    Source context_window_tokens = Source::Default;
    Source compact_model = Source::Default;
    Source think = Source::Default;
    Source soul = Source::Default;
    Source tool_search_threshold = Source::Default;  // tool_search:配置文件或默认,只有这两级
};

struct ConfigResult {
    Config config;
    ConfigSources sources;
    // 这份配置是从哪个配置文件读出来的(cwd 或用户主目录,新位置
    // <目录>/.lubancode/config.json 或旧位置 <目录>/.lubancode.json),没有配置
    // 文件就是 std::nullopt。跟 sources 不完全一样——sources 是"每个字段最终用了
    // 哪一级",这个字段单纯记"读到的配置文件在哪",供 /model 之类想更新配置
    // 文件的场景用(LoadFromEnv 里填;MergeConfig 本身是纯函数,不碰路径)。
    std::optional<std::string> config_file_path;
    // 本次加载时如果发生了"旧位置 .lubancode.json 挪到新位置
    // .lubancode/config.json"这件事,这里是要打印给用户看的那一行通知
    // (成功或失败都会有一行);没发生迁移就是 std::nullopt。LoadFromEnv 里填。
    std::optional<std::string> migration_notice;
};

// .lubancode.json 解析出来的字段,全部可选,缺的字段留 std::nullopt。
// source_path 是这份配置读自哪个文件(诊断/报错用),不参与合并逻辑。
struct FileConfig {
    std::optional<std::string> wire;
    std::optional<std::string> base_url;
    std::optional<std::string> api_key;
    std::optional<std::string> model;
    std::optional<std::size_t> max_context_chars;
    std::optional<std::string> theme;               // dark / light / plain
    std::optional<std::string> system_prompt_file;   // 人格文件路径
    std::optional<std::string> context_window;        // "256k"/"512k"/"1m"/裸数字,原始字符串,解析交给 MergeConfig
    std::optional<std::string> compact_model;         // 压缩用的模型
    std::optional<std::string> think;                 // none/low/medium/high
    std::optional<std::string> soul;                  // 魂的名字(default/off/souls 里的文件名)
    std::optional<HooksConfig> hooks;                  // M9:hooks 段,整段有没有出现在 JSON 里
    // M8:mcpServers 段,整段有没有出现在 JSON 里(跟 hooks 同样待遇——
    // 只从配置文件来,没有环境变量、没有内置默认值)。
    std::optional<std::map<std::string, McpServerConfig>> mcp_servers;
    // websearch:search 段,整段有没有出现在 JSON 里(同上,只从配置文件来)。
    std::optional<SearchConfig> search;
    // LSP:lsp 段,整段有没有出现在 JSON 里(待遇同 mcpServers)。
    std::optional<std::map<std::string, LspServerConfig>> lsp_servers;
    // tool_search:延迟挂载阈值,非负整数(0 = 永不延迟)。
    std::optional<int> tool_search_threshold;
    std::string source_path;
    // 这份 FileConfig 是不是从"旧位置迁移到新位置"这个动作里读出来的;
    // 有值就是要打印给用户看的那一行通知(LoadFileConfig 填,LoadFromEnv
    // 原样搬到 ConfigResult::migration_notice 上)。
    std::optional<std::string> migration_notice;
};

// LUBANCODE_ 专属环境变量读出来的值,全部可选(没设置、或者设置了空串,
// 都算"没有")。
struct LubancodeEnvValues {
    std::optional<std::string> wire;
    std::optional<std::string> base_url;
    std::optional<std::string> api_key;
    std::optional<std::string> model;
    std::optional<std::size_t> max_context_chars;
    std::optional<std::string> theme;               // LUBANCODE_THEME
    std::optional<std::string> system_prompt_file;   // LUBANCODE_SYSTEM_PROMPT_FILE
    std::optional<std::string> context_window;        // LUBANCODE_CONTEXT_WINDOW,原始字符串
    std::optional<std::string> compact_model;         // LUBANCODE_COMPACT_MODEL
    std::optional<std::string> think;                 // LUBANCODE_THINK
    std::optional<std::string> soul;                  // LUBANCODE_SOUL
};

// 通用环境变量(ANTHROPIC_*/OPENAI_*)读出来的值。两组都传全,MergeConfig
// 内部解出 wire 之后自己挑该用哪一组——这样 MergeConfig 就不用依赖调用方
// 提前算好 wire,整个函数保持纯粹、一次调用就能测完四级优先级。
struct GenericEnvValues {
    std::optional<std::string> anthropic_base_url;
    std::optional<std::string> anthropic_auth_token;
    std::optional<std::string> anthropic_model;
    std::optional<std::string> openai_base_url;
    std::optional<std::string> openai_api_key;
    std::optional<std::string> openai_model;
};

// 纯函数,不碰任何 IO:按四级优先级(专属 env > 配置文件 > 通用 env > 内置
// 默认)逐字段合并出最终配置,并记录每个字段的来源。
// 注意:api_key 四级都没有时,这里不报错,只是把 auth_token 留空、来源记成
// Default——校验交给下面的 RequireApiKey,好让 --config 在 api_key 没配好
// 时也能把已经解出来的其它字段和"缺在哪一级"一并打印出来,而不是直接崩掉
// 什么都看不到。
// wire 字段(专属 env 或配置文件)写了不认得的值(不是 anthropic/responses)
// 时才会报错——这个没法留空糊弄过去,下游没法决定用哪个默认端点。
std::expected<ConfigResult, std::string> MergeConfig(const LubancodeEnvValues& lubancode_env,
                                                       const std::optional<FileConfig>& file_config,
                                                       const GenericEnvValues& generic_env);

// 解析 context_window 的字符串取值:"256k"/"512k"/"1m"(大小写不敏感),
// 或者裸数字(直接就是 token 数)。k/m 按十进制换算(k=1000,m=1,000,000)——
// 选十进制不选 1024 进制,是图一个整数、好心算,跟这行字段本来就是"档位"
// 性质(不是精确到字节的量)相配。裸数字、k/m 后缀两种写法之外的输入
// (空串、非数字、0、负数、"abc" 这种)都报错,错误信息里说明这个坏值本身
// 是什么,不带"从哪儿来"(调用方 MergeConfig/ParseFileConfigJson 自己拼上
// "环境变量 LUBANCODE_CONTEXT_WINDOW 里的" 这类前缀)。
std::expected<std::size_t, std::string> ParseContextWindowTokens(const std::string& raw);

// 纯函数:检查合并结果里的 api_key 是不是空的。空的话报错,错误信息里把
// 四级来源都提一遍(按 result.config.wire 挑出对应的通用环境变量名),
// 让人知道去哪儿配。真正要跟模型对话之前(AskOnce/InteractiveLoop 之前)
// 才需要调这个;--config 只是看一眼配置,不需要。
std::expected<void, std::string> RequireApiKey(const ConfigResult& result);

// 纯函数:非交互场景(单发模式 `lubancode "问题"`、管道模式)在真正发请求前的
// 最后一关——base_url、api_key、model 三个字段都不许是空的(三个都没有内置
// 默认值)。缺哪个说哪个,统一给出三条配置途径:交互模式走初次配置向导 /
// 用户主目录放一份 .lubancode.json / 设 LUBANCODE_* 环境变量。
// 跟 RequireApiKey 的区别:RequireApiKey 只管 api_key 一个字段(给
// --config 之外的地方单独复用),这个管三个字段一起,是非交互路径实际会
// 调用的那个。
std::expected<void, std::string> RequireConfigured(const ConfigResult& result);

// 把 api_key 打码:只留前 8 位 + "...",没设置就显示 (未设置)。--config、
// 初次配置向导的汇总展示都用这个,统一打码规则。
std::string MaskApiKey(const std::string& api_key);

// 用户主目录:Windows 取 %USERPROFILE%,别的平台取 $HOME。找不到返回
// std::nullopt。LoadFileConfig 内部用,也供初次配置向导/SaveConfigFile
// 拼路径用,所以导出成公开函数。
std::optional<std::string> HomeDir();

// <主目录>/.lubancode 这个目录的路径——lubancode 的"家",配置文件放
// <主目录>/.lubancode/config.json,往后 plugins/、skills/ 也会住这儿。
// 找不到主目录时返回 std::nullopt。
std::optional<std::string> HomeLubancodeDir();

// 一次"旧位置配置文件挪到新位置"的处理结果。
struct ConfigMigrationOutcome {
    // 之后应该实际去读的路径:新位置已存在、或者迁移成功,是新位置;
    // 迁移失败,是旧位置(继续用旧的,不能让用户没法用);旧位置也不存在
    // (压根没有配置文件),是空字符串。
    std::string effective_path;
    // 要打印给用户看的一行通知(迁移成功或失败都有);什么都没发生
    // (新位置本来就存在,或者旧位置也不存在)时是 std::nullopt。
    std::optional<std::string> notice;
};

// 纯粹处理文件系统这一件事,不解析 JSON:new_path 已经存在就什么都不干,直接
// 说"用 new_path"；new_path 不存在但 old_path 存在,就尝试迁移——建目录、
// std::filesystem::rename,跨盘导致 rename 失败就退化成 copy_file + remove;
// 迁移失败(建目录失败、复制也失败)不算错误,不崩,继续用旧路径,notice
// 里说明失败原因。两个路径都不存在,effective_path 留空、notice 留空
// (调用方按"没有配置文件"处理)。
// 单独导出成公开函数,是为了让"临时目录造旧文件,验证挪动逻辑"这条能绕开
// 真实用户主目录直接单测。
ConfigMigrationOutcome MigrateConfigFileIfNeeded(const std::string& old_path, const std::string& new_path);

// 把 config 写成 JSON,保存到用户主目录的 .lubancode/config.json(目录不存在
// 就先建;找不到主目录时报错)。成功时返回写入的完整路径。初次配置向导
// 选择保存时调用这个。
std::expected<std::string, std::string> SaveConfigFile(const Config& config);

// 只更新一份已存在的配置文件里的 model 字段,其余字段原样保留(哪怕是
// 这份 FileConfig 结构体不认得的字段,也不会被冲掉——直接读原始 JSON、
// 改一个字段、写回去,不经过 FileConfig 这层)。/model 切换后问"写进配置
// 文件?"选是,就调这个。file_path 打不开、内容不是合法 JSON,都报错。
std::expected<void, std::string> UpdateModelInConfigFile(const std::string& file_path, const std::string& model);

// 同上,只更新 soul 字段(/soul 切换后问"写进配置?"选是时调用),其余
// 字段一个都不碰。
std::expected<void, std::string> UpdateSoulInConfigFile(const std::string& file_path, const std::string& soul);

// 把一段 JSON 文本解析成 FileConfig。file_path_for_error 只用来拼错误信息,
// 不影响解析本身。JSON 坏了、或者顶层不是一个 object,都返回带路径的错误。
std::expected<FileConfig, std::string> ParseFileConfigJson(const std::string& json_text,
                                                             const std::string& file_path_for_error);

// 纯函数,不碰 IO:解析 config.json 里的 "hooks" 字段(整个 JSON object)。
// pre_tool/post_tool 是数组,每项 {"matcher": "工具名或 *(缺省当 *)",
// "command": "..."(必填,字符串)};session_start/session_end 也是数组,
// 每项只认 {"command": "..."}(写了 matcher 也不看,不报错)。
// hooks_json 不是 object、四个字段里任意一个不是数组、数组元素不是
// object、缺 command、字段类型不对……都直接报错,错误信息带上
// file_path_for_error 和具体是第几项,方便定位。
std::expected<HooksConfig, std::string> ParseHooksConfig(const nlohmann::json& hooks_json,
                                                           const std::string& file_path_for_error);

// 纯函数,不碰 IO:解析 config.json 里的 "mcpServers" 字段(整个 JSON
// object)。每个键是服务器名,值是 {"command": "...(必填,字符串)",
// "args": ["...", ...](可选,字符串数组,缺省空),
// "env": {"K": "V", ...}(可选,字符串到字符串的 object,缺省空)}。
// mcp_servers_json 不是 object、某个服务器的值不是 object、缺 command、
// args/env 类型不对……都直接报错,错误信息带上 file_path_for_error 和
// 具体是哪个服务器的哪个字段,方便定位。
std::expected<std::map<std::string, McpServerConfig>, std::string> ParseMcpServersConfig(
    const nlohmann::json& mcp_servers_json, const std::string& file_path_for_error);

// 纯函数,不碰 IO:解析 config.json 里的 "search" 字段(整个 JSON object)。
// 格式 {"provider": "tavily"|"brave"|"serper", "api_key": "..."},两个字段
// 都必填。search_json 不是 object、provider 不是这三家之一、api_key 缺失或
// 空串……都直接报错,错误信息带上 file_path_for_error,方便定位。
std::expected<SearchConfig, std::string> ParseSearchConfig(const nlohmann::json& search_json,
                                                             const std::string& file_path_for_error);
// 纯函数,不碰 IO:解析 config.json 里的 "lsp" 字段(整个 JSON object)。
// 每个键是语言名,值是 {"command": "...(必填,字符串)",
// "args": ["...", ...](可选,字符串数组,缺省空),
// "extensions": [".cpp", ...](必填,非空字符串数组),
// "idle_minutes": 10(可选,正整数,缺省 10)}。
// lsp_json 不是 object、某个语言的值不是 object、缺 command、extensions
// 缺失/空/类型不对、idle_minutes 不是正整数……都直接报错,错误信息带上
// file_path_for_error 和具体是哪个语言的哪个字段,方便定位。
std::expected<std::map<std::string, LspServerConfig>, std::string> ParseLspServersConfig(
    const nlohmann::json& lsp_json, const std::string& file_path_for_error);

// 找配置文件,查找顺序:
//   1) cwd 的新位置  <cwd>/.lubancode/config.json
//   2) 主目录的新位置 <主目录>/.lubancode/config.json
//   3) cwd 的旧位置  <cwd>/.lubancode.json          (存在就顺手迁移到 1)
//   4) 主目录的旧位置 <主目录>/.lubancode.json        (存在就顺手迁移到 2)
// 命中旧位置时,自动挪到对应的新位置(见 MigrateConfigFileIfNeeded),
// 迁移的通知记在返回值的 FileConfig::migration_notice 里。四处都没有,
// 返回 std::optional<FileConfig>(std::nullopt)(不算错)。找到了但解析
// 失败,返回错误(带路径)。
std::expected<std::optional<FileConfig>, std::string> LoadFileConfig();

// 真正的入口:读 LUBANCODE_ 专属环境变量、找并读配置文件、读通用环境变量,
// 按四级优先级合并出最终配置。
std::expected<ConfigResult, std::string> LoadFromEnv();

// 读 --system-prompt / system_prompt_file 指向的人格文件,UTF-8 原样读入,
// 整篇作为字符串返回。文件打不开(不存在、没权限……)或者内容是空的,
// 都返回带路径的可读错误——语义上这两种情况都没法拿来当人格段用。
std::expected<std::string, std::string> ReadSystemPromptFile(const std::string& path);

}  // namespace lubancode::config
