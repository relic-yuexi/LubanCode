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

// extra_body 要在 ProviderConfig/Config 里存一份实打实的 nlohmann::json
// object(不是指针/引用),json_fwd.hpp 那种前向声明不够用,得换成全量头。
#include <nlohmann/json.hpp>

namespace lubancode::config {

// 说哪种"方言"跟模型对话:Anthropic 的 Messages API,还是 OpenAI 的
// Responses API，还是兼容面最广的 OpenAI Chat Completions。默认 anthropic。
enum class Wire { Anthropic, Responses, ChatCompletions };

// 一个字段的值最终是从哪一级配置来的,--config 诊断输出用。
// 配置文件分两级:项目级 <cwd>/.lubancode/config.json 压过全局
// <主目录>/.lubancode/config.json。老的笼统 ConfigFile 拆成这两级——旧断言
// 里凡是"单文件 = 配置文件来源"的,一律映射成 ProjectConfigFile(3 参
// MergeConfig 便捷包装把那一份当项目级看待,见下)。
enum class Source {
    LubancodeEnv,       // LUBANCODE_ 专属环境变量
    ProjectConfigFile,  // <cwd>/.lubancode/config.json(项目级,压过全局)
    GlobalConfigFile,   // <主目录>/.lubancode/config.json(全局)
    GenericEnv,         // ANTHROPIC_*/OPENAI_* 通用环境变量
    Default,            // 内置默认值
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

// max_steps_per_turn(agent 主循环一个 turn 内跟模型来回的步数上限;旧配置
// 名 max_turns)的内置默认值。数值上跟 agent::AgentLoop 构造函数的默认参数
// 保持一致,但 config 层不依赖 agent 层(同 kDefaultMaxContextChars 的理由,
// 依赖只许单向)。
//
// 默认 0 = 无上限。曾经先后是 25、100——但硬闸这个思路本身就旧了:现在的
// 模型常态是跑十几个小时的长程任务,不管定多高的数字,总有正常任务会撞
// 上去,把模型正在做的事硬掐断。跟 Claude Code 同一个待遇:默认不设上限,
// 防跑飞靠用户 ESC/Ctrl+C 打断和成本可见性(usage 汇报),而不是靠一堵矮墙。
// 想要一个硬上限的人(比如管道模式没有 ESC 可打断,想兜底防真死循环),
// 显式配一个正整数就是——闸只在"确实想要"的时候存在。
constexpr int kDefaultMaxStepsPerTurn = 0;

constexpr std::size_t kDefaultMemoryMaxIndexBytes = 16 * 1024;
// 召回预算收紧(规格"召回只送命中，不送整份索引"):index.md 不再随请求
// 注入,正文默认总预算降到 8 KiB、最多 3 条;index 字段只管 index.md 文件
// 本身的大小,不再影响 prompt。
constexpr std::size_t kDefaultMemoryMaxRetrievalBytes = 8 * 1024;
constexpr std::size_t kDefaultMemoryMaxResults = 3;

struct MemoryConfig {
    bool enabled = false;
    bool use = true;
    bool generate = true;
    // 学习档位(0.30.x 候审箱):off 不提候选不写入;review 提候选、用户
    // 审过才入库(默认);auto 自动写入,只认全局配置显式授权——项目配置
    // 只能收窄(off/review),不能替用户升到 auto。老 generate=false 等价
    // 于 learn=off,合并时一并算进去。
    std::string learn = "review";
    std::size_t max_index_bytes = kDefaultMemoryMaxIndexBytes;
    std::size_t max_retrieval_bytes = kDefaultMemoryMaxRetrievalBytes;
    std::size_t max_results = kDefaultMemoryMaxResults;
};

// 文件层保留“字段有没有写”的区别，才能让项目配置只收窄全局开关。
struct MemoryFileConfig {
    std::optional<bool> enabled;
    std::optional<bool> use;
    std::optional<bool> generate;
    std::optional<std::string> learn;
    std::optional<std::size_t> max_index_bytes;
    std::optional<std::size_t> max_retrieval_bytes;
    std::optional<std::size_t> max_results;
};

// 一家模型服务端的会话配置。默认走 key_env(只记密钥所在环境变量的名字，
// 不落明文)；/provider add 向导允许直接贴 key 落盘到 api_key 字段，取值
// 优先级 api_key（非空）> 环境变量 key_env（见 ProviderApiKey）。api_key
// 落了盘就不再是"只记名字"那套安全设计——展示/日志一律走 MaskApiKey 打码，
// 由调用方自己当心。model 可以留空：切过去后仍可用 /model 选。
// model_reasoning_effort 可选：切到这个 provider 时按 /think 同一套机制
// 应用（写进 current_think），留空 = 不动当前档位。
struct ProviderConfig {
    std::string name;
    std::string base_url;
    Wire wire = Wire::Anthropic;
    std::string key_env = "ANTHROPIC_AUTH_TOKEN";
    std::string api_key;               // 可选，非空时优先于 key_env
    std::string model;
    std::string model_reasoning_effort;  // 可选，切到该端时应用的推理档位
    std::size_t context_window_tokens = kDefaultContextWindowTokens;
    // native_web_search:该端若原生支持联网搜索(服务端自己查、把结果编排
    // 进回复文本里,客户端不用实现任何执行逻辑),开这个开关就在请求里带上
    // 声明。默认 false(不是所有兼容端都支持,乱开可能把请求搞坏)。
    // responses 协议塞
    // {"type":"web_search"}(BuildRequestJson,src/api/responses/request.cpp),
    // anthropic 协议塞 {"type":"web_search_日期版本号","name":"web_search"}
    // (BuildRequestJson,src/api/anthropic/client.cpp)。chat_completions 没有
    // 统一的内置搜索形状,靠 extra_body。按 provider 各自开关。
    bool native_web_search = false;
    // stream_usage:该端支持 Chat 流式 stream_options.include_usage(在
    // [DONE] 前多回一只完整 usage chunk)。待遇同 native_web_search:切
    // provider 时从目录镜像,默认 false(有些兼容端不认这个字段)。用户
    // 在 extra_body 里显式写 stream_options 仍可压过它。
    bool stream_usage = false;
    // reasoning_replay:Chat wire 的思考回传策略,空/never/tool_episode
    // (语义见 api/chat/request.hpp)。待遇同 stream_usage:目录是唯一
    // 来源,切 provider 时镜像,默认空 = never。
    std::string reasoning_replay;
    // extra_body:任意厂商私有请求参数(比如 GLM 的 thinking.type + 一个
    // 自定义 reasoning_effort 档位),每次请求前浅合并进请求体顶层——键
    // 冲突时 extra_body 里的值整个覆盖掉内置逻辑算出来的值(不做深合并,
    // 图个简单、行为可预期)。默认空 object,表示"不合并任何东西",跟
    // native_web_search 默认关一个道理:不想让 lubancode 为每家服务商的
    // 偏门参数单独写一段翻译代码,交给用户自己在配置里加。
    nlohmann::json extra_body = nlohmann::json::object();
    // extra_headers:任意"头名 -> 值"表,每次请求追加/覆盖到 HTTP 头里
    // (包括 Authorization——想用自定义鉴权头,用户自己对后果负责)。默认
    // 空,表示不加任何额外头。
    std::map<std::string, std::string> extra_headers;
};

// tool_search(延迟挂载)的阈值默认值:注册表总工具数超过这个数才启用
// 延迟机制(超了才把 MCP/插件这类外挂工具改成"检索后挂载");0 = 永不
// 延迟,一切照旧全量直挂。内置工具撑死十几个,默认 20 意味着不挂一堆
// 外挂工具的用户永远走不到延迟这条路,现状行为零变化。
constexpr int kDefaultToolSearchThreshold = 20;

// M11(网络超时):两个 API 客户端(anthropic/responses)以及 ListModels 共用
// 的超时默认值,毫秒/秒两种单位混用是因为对应的 cpr 选项本身单位不同
// (ConnectTimeout 认毫秒,LowSpeed::time 和 cpr::Timeout 的语义按秒/毫秒
// 分别处理更直观,这里 idle/request 两个字段存"秒",内部换算成毫秒喂给 cpr)。
//
// - kDefaultConnectTimeoutMs:TCP+TLS 握手阶段的上限。连不上时(DNS 解析
//   不动、服务器不回包)靠这个及时报错,不干等。15 秒是"网络稍差也能连上,
//   真连不上也不用等太久"的折中。
// - kDefaultStreamIdleTimeoutSecs:流式(SSE)读空闲超时——不是总时长上限
//   (流式回答本可以很长),是"连续这么多秒一个字节都没收到"就判定连接
//   假死。60 秒给足模型长时间思考/工具调用间隙的余量,又不至于真断线时
//   干等太久。
// - kDefaultRequestTimeoutSecs:非流式请求(目前只有 ListModels 拉模型
//   列表)的整体超时——这类请求响应体小,没有"回复很长"的顾虑,直接给
//   总时长上限。30 秒跟 tools/web_search.cpp、tools/web_fetch.cpp 里已有的
//   cpr::Timeout{30000} 保持一致的量级。
constexpr int kDefaultConnectTimeoutMs = 15000;
constexpr int kDefaultStreamIdleTimeoutSecs = 60;
constexpr int kDefaultRequestTimeoutSecs = 30;

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

// 常驻 status panel 的内置字段编排。字段只负责“摆什么、按什么顺序摆”，
// 具体值由交互会话逐帧补入；项目级 config.json 写了这一整段，就压过
// 全局同名段。内置字段不执行 shell，刷新时不会卡住输入框。
struct StatusPanelConfig {
    std::vector<std::string> items{
        "permission_mode", "model", "cwd", "git_branch", "context", "tokens"};
    std::string separator = " · ";
};

// 子代理(subagent)段的运行配置。max_steps_per_turn(旧配置名
// subagent.max_turns):nullopt = 未单独配置,运行时继承
// config.max_steps_per_turn(默认 0 = 不限步);显式配了(含 0)就是子代理
// 自己的预算,与主代理的分开管。待遇同 hooks:只从配置文件来
// (项目级压全局),没有环境变量、没有内置默认值这两级——0 的语义全路
// 一致,都是不限步(规格"现场四":子代理不再暗藏步数硬闸)。
struct SubagentConfig {
    std::optional<int> max_steps_per_turn;
};

struct Config {
    Wire wire = Wire::Anthropic;
    std::string base_url;
    std::string auth_token;  // 即 api_key
    std::string model;
    // native_web_search:当前生效端是否声明原生联网搜索。anthropic/
    // responses 各自翻译成协议工具；chat_completions 没有统一形状，靠
    // extra_body。见 ProviderConfig::native_web_search。跟 wire/
    // base_url/auth_token/model 一样,是"当前激活端"的运行期状态——只在
    // /provider switch 时从 ProviderConfig::native_web_search 镜像过来,
    // 不走独立的配置文件/环境变量四级合并(provider 才是唯一来源),默认
    // false。
    bool native_web_search = false;
    // stream_usage:同 native_web_search 的待遇——/provider switch 时从
    // ProviderConfig::stream_usage 镜像,默认 false。见 ProviderConfig 注释。
    bool stream_usage = false;
    // reasoning_replay:同上,从 ProviderConfig 同名字段镜像,空 = never。
    std::string reasoning_replay;
    // extra_body/extra_headers:跟 native_web_search 同一套待遇——切
    // provider 时从 ProviderConfig 同名字段镜像过来;但这两个字段还多一条
    // 路:配置文件顶层(不进 providers 数组的"单 provider 配置"写法)也能
    // 直接写 extra_body/extra_headers,那种场景走的是下面 FileConfig 那两
    // 个同名字段、按 hooks/mcpServers 那套"整段回退"合并进来的(见
    // MergeConfig),不是从某个 ProviderConfig 镜像。默认都是空(不合并/
    // 不加任何东西)。
    nlohmann::json extra_body = nlohmann::json::object();
    std::map<std::string, std::string> extra_headers;
    std::size_t max_context_chars = kDefaultMaxContextChars;
    // max_steps_per_turn(旧名 max_turns):agent 主循环一次 Run() 最多跟
    // 模型来回几步(一步一次模型请求)。只从"专属 env / 配置文件 / 默认值"
    // 三级来,没有通用 env 这一级(待遇同 max_context_chars)。语义:不配、
    // 或者显式配 0,都是无上限;配正整数才是硬上限(超过就报错停止)。
    // 负数、非法值在解析阶段就被忽略,落到下一级/默认值,不报错——这是条
    // "救命阀"字段,配置写错不该拦住用户开工。
    int max_steps_per_turn = kDefaultMaxStepsPerTurn;
    std::string theme = kDefaultTheme;   // dark / light / plain,没配到就是 kDefaultTheme
    // i18n:界面语言(zh-CN / en / languages/ 里的语言码)。空串 = 跟系统
    // (启动时 DetectSystemLanguage 探测)。四级合并,env 是 LUBANCODE_LANG。
    std::string language;
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
    StatusPanelConfig status_panel;
    // 子代理段:只从配置文件来(项目级压全局);预算未设(nullopt)
    // 时运行时继承主代理的步数上限。
    SubagentConfig subagent;
    // tool_search:延迟挂载的启用阈值,0 = 永不延迟。只从配置文件读
    // (没有环境变量这一级),没配就是默认 20。
    int tool_search_threshold = kDefaultToolSearchThreshold;
    // M11(网络超时):三个字段只从配置文件读(项目级 > 全局,跟
    // tool_search_threshold 同样待遇,没有环境变量这一级),没配就是上面
    // 那三个内置默认值。connect_timeout_ms 单位毫秒,另外两个单位秒。
    int connect_timeout_ms = kDefaultConnectTimeoutMs;
    int stream_idle_timeout_secs = kDefaultStreamIdleTimeoutSecs;
    int request_timeout_secs = kDefaultRequestTimeoutSecs;
    // 上次成功切换的 provider 名。只记名字，不复制 URL、模型或密钥；
    // LoadFromEnv 合并完配置后再从 providers 里展开成当前运行配置。
    std::string active_provider;
    // 多端配置是配置文件里一个完整段：项目级写了 providers 就压全局那一
    // 段，跟 hooks/mcpServers/lsp 的既有合并法一致。
    std::vector<ProviderConfig> providers;
    MemoryConfig memory;
};

// 每个字段最终来自哪一级,跟 Config 里的字段一一对应。
struct ConfigSources {
    Source wire = Source::Default;
    Source base_url = Source::Default;
    Source auth_token = Source::Default;
    Source model = Source::Default;
    Source max_context_chars = Source::Default;
    Source max_steps_per_turn = Source::Default;
    Source theme = Source::Default;
    Source language = Source::Default;  // i18n:界面语言
    Source system_prompt_file = Source::Default;
    Source context_window_tokens = Source::Default;
    Source compact_model = Source::Default;
    Source think = Source::Default;
    Source soul = Source::Default;
    Source tool_search_threshold = Source::Default;  // tool_search:配置文件或默认,只有这两级
    Source connect_timeout_ms = Source::Default;        // M11:配置文件或默认,只有这两级
    Source stream_idle_timeout_secs = Source::Default;   // 同上
    Source request_timeout_secs = Source::Default;       // 同上
    Source active_provider = Source::Default;
    Source extra_body = Source::Default;
    Source extra_headers = Source::Default;
    Source providers = Source::Default;
    Source status_panel = Source::Default;
    Source subagent = Source::Default;  // 子代理段:配置文件或默认
    Source memory = Source::Default;
};

// settings.local.json 里的 permissions 段,项目级本地权限(不进版本库)。
// 位置 <cwd>/.lubancode/settings.local.json,照 Claude Code / Codex 的路数:
//   - allow_tools:这些工具启动即注入会话"总是允许"集合,直接免确认;
//   - allow_commands:run_command 命令前缀白名单,auto 档里等价 command_safety
//     判成 Safe(补充白名单,不改 command_safety.cpp,在 main 的 auto 分流处
//     叠加判定);
//   - deny_commands:run_command 命令前缀黑名单,confirm/auto 档里前缀命中就
//     永远问一句(压过 allow_commands、压过会话"总是允许";yolo/--yes 是显式
//     全放,deny 不拦);
//   - default_confirm_mode:起手确认档(auto/yolo/confirm),优先级低于
//     --yes/LUBANCODE_CONFIRM_MODE,高于内置默认 confirm。
// 全部字段可选;坏 JSON 只告警跳过,不崩。
struct SettingsLocal {
    std::vector<std::string> allow_tools;
    std::vector<std::string> allow_commands;
    std::vector<std::string> deny_commands;
    std::optional<std::string> default_confirm_mode;  // auto / yolo / confirm

    bool Empty() const {
        return allow_tools.empty() && allow_commands.empty() && deny_commands.empty() &&
               !default_confirm_mode.has_value();
    }
};

struct ConfigResult {
    Config config;
    ConfigSources sources;
    // 这份配置是从哪个配置文件读出来的(cwd 或用户主目录,新位置
    // <目录>/.lubancode/config.json 或旧位置 <目录>/.lubancode.json),没有配置
    // 文件就是 std::nullopt。跟 sources 不完全一样——sources 是"每个字段最终用了
    // 哪一级",这个字段单纯记"读到的配置文件在哪",供 /model 之类想更新配置
    // 文件的场景用(LoadFromEnv 里填;MergeConfig 本身是纯函数,不碰路径)。
    // 分层之后可能有两份配置文件(项目级 + 全局):config_file_path 是"写回
    // 优先目标"——项目级在就是项目级,否则全局;下面两个字段把两级各自的
    // 路径分开记,/config 展示"这份来自项目级/全局"时用(都没有就都空)。
    std::optional<std::string> config_file_path;
    std::optional<std::string> project_config_file_path;  // <cwd>/.lubancode/config.json
    std::optional<std::string> global_config_file_path;   // <主目录>/.lubancode/config.json
    // 兼容期提示(命名规范第二批):旧名 max_turns / LUBANCODE_MAX_TURNS
    // 被读入、或新旧两键同现冲突时,这里一条一条记给用户看。空 = 本次加载
    // 没碰旧名。MergeConfig(纯函数)填,cli_app 打印。
    std::vector<std::string> deprecation_notices;
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
    // max_steps_per_turn / max_turns(旧):同一预算的新旧两个键,兼容期
    // 双读。非负整数才落进字段(0 = 显式无上限,是合法值);负数或者字段
    // 类型不对,ParseFileConfigJson 静默跳过(留 nullopt),不报错——
    // "救命阀"字段,配置写错不该拦住用户开工。两键同现同值按新名收账;
    // 同现异值在 MergeConfig 明报冲突(新名优先)并打弃用提示。
    std::optional<int> max_steps_per_turn;  // 新键 "max_steps_per_turn"
    std::optional<int> max_turns;           // 旧键 "max_turns"(弃用,至少跨一个明确版本窗后再删)
    std::optional<std::string> theme;               // dark / light / plain
    std::optional<std::string> language;             // i18n:界面语言码
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
    // status_panel 整段回退；items 的顺序就是终端展示顺序。
    std::optional<StatusPanelConfig> status_panel;
    // subagent 段:{"subagent": {"max_steps_per_turn": N}}(新键;旧键
    // max_turns 兼容读入)。非负整数(0 = 显式不限步);负数/类型不对静默
    // 跳过(待遇同主预算字段的"救命阀"取舍)。
    std::optional<int> subagent_max_steps_per_turn;  // 新键
    std::optional<int> subagent_max_turns;           // 旧键(弃用)
    // extra_body/extra_headers:顶层"单 provider 配置"写法专用(不进
    // providers 数组的场景),整段有没有出现在 JSON 里(待遇同 hooks/
    // mcpServers——只从配置文件来,没有环境变量、没有内置默认值这两级)。
    std::optional<nlohmann::json> extra_body;
    std::optional<std::map<std::string, std::string>> extra_headers;
    // tool_search:延迟挂载阈值,非负整数(0 = 永不延迟)。
    std::optional<int> tool_search_threshold;
    // M11(网络超时):三个字段都是正整数,没写就是 std::nullopt(往下一级
    // 找,最终落到内置默认值)。connect_timeout_ms 单位毫秒,另外两个单位秒。
    std::optional<int> connect_timeout_ms;
    std::optional<int> stream_idle_timeout_secs;
    std::optional<int> request_timeout_secs;
    std::optional<std::string> active_provider;
    std::optional<std::vector<ProviderConfig>> providers;
    std::optional<MemoryFileConfig> memory;
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
    std::optional<int> max_steps_per_turn;  // LUBANCODE_MAX_STEPS_PER_TURN,非负整数(0 = 无上限),负数忽略
    std::optional<int> max_turns;           // LUBANCODE_MAX_TURNS(旧名,兼容读入),非负整数(0 = 无上限),负数忽略
    std::optional<std::string> theme;               // LUBANCODE_THEME
    std::optional<std::string> language;             // LUBANCODE_LANG(i18n 界面语言)
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

// 纯函数,不碰任何 IO:按优先级逐字段合并出最终配置,并记录每个字段的
// 来源。分层之后配置文件拆成两级——优先级(高到低):
//   专属 env > 项目级 config.json > 全局 config.json > 通用 env > 内置默认。
// 按"字段"逐个决:项目级缺的字段回退全局,全局也缺再往下一级找。对象型
// 整段(hooks/mcpServers/search/lsp)按"整段"回退——项目级写了就用项目级
// 那一整段,否则用全局那一整段(不做键级混合,语义清楚)。
// 注意:api_key 各级都没有时,这里不报错,只是把 auth_token 留空、来源记成
// Default——校验交给下面的 RequireApiKey,好让 --config 在 api_key 没配好
// 时也能把已经解出来的其它字段和"缺在哪一级"一并打印出来,而不是直接崩掉
// 什么都看不到。
// wire 字段(专属 env 或配置文件)写了不认得的值(不是 anthropic/responses)
// 时才会报错——这个没法留空糊弄过去,下游没法决定用哪个默认端点。
std::expected<ConfigResult, std::string> MergeConfig(const LubancodeEnvValues& lubancode_env,
                                                       const std::optional<FileConfig>& project_file,
                                                       const std::optional<FileConfig>& global_file,
                                                       const GenericEnvValues& generic_env);

// 便捷包装(旧签名):只有一份配置文件时,当项目级看待(全局留空)。
// 老调用方/老单测沿用这个,来源统一记成 ProjectConfigFile。
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

// provider 的 wire 写法沿用既有配置字段：anthropic / responses / chat_completions。
std::expected<Wire, std::string> ParseProviderWire(const std::string& raw);
std::string ProviderWireName(Wire wire);

// /provider set <名字> <字段> <值> 的"值"解析:on/off、true/false、1/0 都
// 认,大小写不敏感(cli 层已经小写化过,这里再兜一道);认不出就报错，
// 错误信息里把认得的写法都列一遍。main.cpp 拿它把用户敲的字符串变 bool。
std::expected<bool, std::string> ParseBoolToggle(const std::string& raw);

// 粗验 provider：名字、地址、密钥环境变量名不许空；地址只要求 http:// 或
// https:// 起首，避免把 URL 语义校得过死。/provider add 写盘前调用。
std::expected<void, std::string> ValidateProviderConfig(const ProviderConfig& provider);

// 粗验 provider 名字本身：非空、只许字母/数字/下划线/点/短横线（不许空白、
// 斜杠这类会跟 slash 命令解析或路径拼接打架的字符），且不与 existing 里
// 任何一个重名。/provider add 向导用（一行式旧用法只查重名，不查字符集，
// 维持既有行为不变）。
std::expected<void, std::string> ValidateProviderName(const std::string& name,
                                                        const std::vector<ProviderConfig>& existing);

// 运行时取密钥：provider.api_key 非空就直接用（向导贴的明文）；否则退到
// provider.key_env 对应的环境变量，没有、或环境变量为空，返回 nullopt。
std::optional<std::string> ProviderApiKey(const ProviderConfig& provider);

const ProviderConfig* FindProvider(const std::vector<ProviderConfig>& providers, const std::string& name);

// 把 Config::active_provider 指向的条目展开到当前运行配置。provider 条目
// 所在配置层级只压过同级或更低字段；LUBANCODE_* 仍居最上。找不到名字
// 时清掉本次运行态选择并返回 false，不让一条旧记录拦住启动。
bool ApplyConfiguredActiveProvider(ConfigResult& result);

// 纯函数,不摸文件:在内存里的 providers 列表找 name 对应那条,把
// native_web_search 改成 enabled。找到就改完返回 true;找不到原样不动、
// 返回 false——调用方(HandleProviderCommand /provider set 分支)据此判断
// 要不要报"找不着 provider"、要不要接着落盘。目前只服务这一个字段,以后
// 要扩展别的可设字段(比如 model_reasoning_effort),照这个函数抄一份或者
// 改成传字段名 + 一个 mutate 回调都行。
bool SetProviderNativeWebSearch(std::vector<ProviderConfig>& providers, const std::string& name, bool enabled);

// 纯函数,不摸文件:跟 SetProviderNativeWebSearch 同一个套路,把 name 对应
// 那条 provider 的 extra_body 整个替换成 body(不是合并——/provider set
// extra_body 这条命令本来就是"整段替换"语义,不是往里加键)。找不到 name
// 返回 false、原样不动。
bool SetProviderExtraBody(std::vector<ProviderConfig>& providers, const std::string& name,
                           const nlohmann::json& body);

// 纯函数,不摸文件:把 name 对应那条 provider 的 extra_headers 里
// header_name 这一条设成 value;value 是空串就删掉这一条(不是"设成空
// 值"的头,而是压根不发)。找不到 name 返回 false、原样不动。
bool SetProviderExtraHeader(std::vector<ProviderConfig>& providers, const std::string& name,
                             const std::string& header_name, const std::string& value);

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

// 同上,只更新 language 字段(/language 切换后问"写进配置文件?"选是时
// 调用,沿用 /model 那套写回)。
std::expected<void, std::string> UpdateLanguageInConfigFile(const std::string& file_path,
                                                              const std::string& language);

// providers 段的纯解析与局部回写。回写只改 providers，其余用户字段原样
// 保留；file_path 不存在时会按需建父目录并起一份 JSON object，便于单测和
// /provider add 复用。
std::expected<std::vector<ProviderConfig>, std::string> ParseProvidersConfig(
    const nlohmann::json& providers_json, const std::string& file_path_for_error);
std::expected<void, std::string> UpdateProvidersInConfigFile(const std::string& file_path,
                                                               const std::vector<ProviderConfig>& providers);

// 只改 active_provider 字段，其余 JSON 原样保留。前者供项目级固定选择，
// 后者供 /provider switch 记住全局的上次选择。
std::expected<void, std::string> UpdateActiveProviderInConfigFile(const std::string& file_path,
                                                                    const std::string& name);
std::expected<std::string, std::string> SetActiveProviderInGlobalConfig(const std::string& name);

// /provider add/remove/set 永远改用户主目录的全局 config.json，不碰项目配置。
// 成功时返回实际写入路径；删除/设置找不到名字时报错，不碰文件。
std::expected<std::string, std::string> AddProviderToGlobalConfig(const ProviderConfig& provider);
std::expected<std::string, std::string> RemoveProviderFromGlobalConfig(const std::string& name);

// /provider set native_web_search 用:把全局配置里对应 provider 的
// native_web_search 改掉再原样落盘(SetProviderNativeWebSearch 做实际改
// 字段那一步),返回落盘路径。找不到这个 provider 名字就报错、不碰文件。
std::expected<std::string, std::string> SetProviderNativeWebSearchInGlobalConfig(const std::string& name,
                                                                                   bool enabled);

// /provider set extra_body 用:整段替换全局配置里对应 provider 的
// extra_body 再落盘。找不到这个 provider 名字就报错、不碰文件。
std::expected<std::string, std::string> SetProviderExtraBodyInGlobalConfig(const std::string& name,
                                                                             const nlohmann::json& body);

// /provider set extra_header 用:设置(或者 value 为空串时删除)全局配置里
// 对应 provider 的某一条 extra_headers,再落盘。找不到这个 provider 名字就
// 报错、不碰文件。
std::expected<std::string, std::string> SetProviderExtraHeaderInGlobalConfig(const std::string& name,
                                                                               const std::string& header_name,
                                                                               const std::string& value);

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

// 纯函数,不碰 IO:校验 config.json 里的 "extra_body" 字段——必须是一个
// JSON object(允许任意嵌套值,具体每个键怎么用是各家服务商自己的事,这里
// 只挡"顶层压根不是 object"这种明显写错的情况)。
std::expected<nlohmann::json, std::string> ParseExtraBodyConfig(const nlohmann::json& extra_body_json,
                                                                  const std::string& file_path_for_error);

// 纯函数,不碰 IO:校验 config.json 里的 "extra_headers" 字段——必须是一个
// 字符串到字符串的 JSON object(HTTP 头名 -> 值),某个值不是字符串就报错。
std::expected<std::map<std::string, std::string>, std::string> ParseExtraHeadersConfig(
    const nlohmann::json& extra_headers_json, const std::string& file_path_for_error);

// 分层加载的结果:项目级(<cwd>)、全局(<主目录>)各一份,都可能缺。
// 两级都读、按字段合并交给 MergeConfig。migration_notice 是两级迁移通知
// 合并成的一段(哪一级发生了旧位置→新位置迁移就记上,都没有就 nullopt)。
struct LoadedFileConfigs {
    std::optional<FileConfig> project;  // <cwd>/.lubancode/config.json
    std::optional<FileConfig> global;   // <主目录>/.lubancode/config.json
    std::optional<std::string> migration_notice;
};

// 找配置文件,项目级与全局各自独立地找一遍:
//   每一级:先看新位置 <目录>/.lubancode/config.json;没有再看旧位置
//   <目录>/.lubancode.json(存在就顺手迁移到新位置,见
//   MigrateConfigFileIfNeeded)。都没有就那一级留 std::nullopt(不算错)。
// cwd 与主目录是同一个目录时(在主目录里跑),只当项目级读一份,全局留空,
// 免得同一份文件读两遍、来源标记打架。找到了但解析失败,返回错误(带路径)。
std::expected<LoadedFileConfigs, std::string> LoadFileConfigs();

// 真正的入口:读 LUBANCODE_ 专属环境变量、找并读配置文件、读通用环境变量,
// 按四级优先级合并出最终配置。
std::expected<ConfigResult, std::string> LoadFromEnv();

// 读 --system-prompt / system_prompt_file 指向的人格文件,UTF-8 原样读入,
// 整篇作为字符串返回。文件打不开(不存在、没权限……)或者内容是空的,
// 都返回带路径的可读错误——语义上这两种情况都没法拿来当人格段用。
std::expected<std::string, std::string> ReadSystemPromptFile(const std::string& path);

// -------- settings.local.json:项目级本地权限(不进版本库) --------

// <cwd>/.lubancode/settings.local.json 的路径(cwd_dir 传 CurrentDirUtf8())。
std::string SettingsLocalPath(const std::string& cwd_dir);

// 纯函数,不碰 IO:解析 settings.local.json 文本。顶层要有 "permissions"
// object(没有就返回空 SettingsLocal,不算错);其中 allow_tools /
// allow_commands / deny_commands 是字符串数组(非字符串元素跳过),
// default_confirm_mode 是字符串(auto/yolo/confirm,别的值原样留着交给
// 调用方判)。全部字段可选。坏 JSON、顶层不是 object 才返回错误(调用方
// 打一行警告后当没配置,不崩)。
std::expected<SettingsLocal, std::string> ParseSettingsLocal(const std::string& json_text,
                                                              const std::string& path_for_error);

// 读 <cwd>/.lubancode/settings.local.json。文件不存在返回 std::nullopt
// (不算错);读到了就解析。坏 JSON 把可读错误往上抛(调用方告警跳过)。
std::expected<std::optional<SettingsLocal>, std::string> LoadSettingsLocal(const std::string& cwd_dir);

// 把一个工具名永久写进 <cwd>/.lubancode/settings.local.json 的
// permissions.allow_tools(去重)。目录/文件不存在则按需创建(项目级
// .lubancode/ 只在这"首次持久化 permissions"时才落地),已有的别的字段
// (含不认得的)原样保留。成功返回写入的完整路径;建目录/写文件失败返回
// 可读错误。gitignore 由调用方另行处理(EnsureGitignoreCoversSettingsLocal),
// 好把"追加了一行 / 提示手动加"的反馈打给用户看。
std::expected<std::string, std::string> AddAllowedToolToSettingsLocal(const std::string& cwd_dir,
                                                                       const std::string& tool_name);

// 一条命令过 settings.local 的 permissions 前缀判定后的裁定。deny 压过 allow
// (两边都命中时算 Deny)。None = 两个名单都没命中,交给别处(command_safety /
// 逐个问)决定。
enum class CommandPermission { None, Allow, Deny };

// 纯函数:命令去掉前导空白后,先看 deny_commands 有没有前缀命中(命中即
// Deny,压过一切),再看 allow_commands(命中即 Allow),都没命中 None。
// 空前缀跳过;原始命令串直接比,不做 shell 解析(前缀白/黑名单本就是"这几条
// 命令开头"的朴素约定,跟 command_safety 的保守解析各管一摊)。main 的 auto
// 分流处叠加用:Deny → 永远问(mode 门槛在 main 那边),Allow → auto 档等价
// command_safety 的 Safe。
CommandPermission ClassifyCommandByPermissions(const std::string& command,
                                               const std::vector<std::string>& allow_commands,
                                               const std::vector<std::string>& deny_commands);

// 保证 <cwd>/.gitignore 挡住 .lubancode/settings.local.json:.gitignore 存在
// 且没挡就追加一行(返回 "appended");已经挡住返回 "already";.gitignore
// 压根不存在就不硬塞,返回一行给用户看的提示("hint:...",教他手动加)。
// 纯做文件这一件事,不抛错(读写失败也只是不动 .gitignore)。
std::string EnsureGitignoreCoversSettingsLocal(const std::string& cwd_dir);

}  // namespace lubancode::config
