// 配置来源分四级,优先级从高到低:
//   1) LUBANCODE_ 专属环境变量(LUBANCODE_WIRE / LUBANCODE_BASE_URL /
//      LUBANCODE_API_KEY / LUBANCODE_MODEL / LUBANCODE_MAX_CONTEXT)
//   2) 配置文件 .lubancode.json(先找 cwd,再找用户主目录)
//   3) 通用环境变量(ANTHROPIC_*/OPENAI_*,向后兼容老用法)
//   4) 内置默认值
// 按"字段"逐个决,不是整套配置一刀切——比如配置文件只写了 base_url,
// model 照样能从下一级来。密钥绝不硬编码进源码。
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
#include <optional>
#include <string>

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

struct Config {
    Wire wire = Wire::Anthropic;
    std::string base_url;
    std::string auth_token;  // 即 api_key
    std::string model;
    std::size_t max_context_chars = kDefaultMaxContextChars;
};

// 每个字段最终来自哪一级,跟 Config 里的字段一一对应。
struct ConfigSources {
    Source wire = Source::Default;
    Source base_url = Source::Default;
    Source auth_token = Source::Default;
    Source model = Source::Default;
    Source max_context_chars = Source::Default;
};

struct ConfigResult {
    Config config;
    ConfigSources sources;
    // 这份配置是从哪个 .lubancode.json 读出来的(cwd 或用户主目录),没有配置文件
    // 就是 std::nullopt。跟 sources 不完全一样——sources 是"每个字段最终用了哪一级",
    // 这个字段单纯记"读到的配置文件在哪",供 /model 之类想更新配置文件的场景用
    // (LoadFromEnv 里填;MergeConfig 本身是纯函数,不碰路径)。
    std::optional<std::string> config_file_path;
};

// .lubancode.json 解析出来的字段,全部可选,缺的字段留 std::nullopt。
// source_path 是这份配置读自哪个文件(诊断/报错用),不参与合并逻辑。
struct FileConfig {
    std::optional<std::string> wire;
    std::optional<std::string> base_url;
    std::optional<std::string> api_key;
    std::optional<std::string> model;
    std::optional<std::size_t> max_context_chars;
    std::string source_path;
};

// LUBANCODE_ 专属环境变量读出来的值,全部可选(没设置、或者设置了空串,
// 都算"没有")。
struct LubancodeEnvValues {
    std::optional<std::string> wire;
    std::optional<std::string> base_url;
    std::optional<std::string> api_key;
    std::optional<std::string> model;
    std::optional<std::size_t> max_context_chars;
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

// 把 config 写成 JSON,保存到用户主目录的 .lubancode.json(找不到主目录时
// 报错)。成功时返回写入的完整路径。初次配置向导选择保存时调用这个。
std::expected<std::string, std::string> SaveConfigFile(const Config& config);

// 只更新一份已存在的配置文件里的 model 字段,其余字段原样保留(哪怕是
// 这份 FileConfig 结构体不认得的字段,也不会被冲掉——直接读原始 JSON、
// 改一个字段、写回去,不经过 FileConfig 这层)。/model 切换后问"写进配置
// 文件?"选是,就调这个。file_path 打不开、内容不是合法 JSON,都报错。
std::expected<void, std::string> UpdateModelInConfigFile(const std::string& file_path, const std::string& model);

// 把一段 JSON 文本解析成 FileConfig。file_path_for_error 只用来拼错误信息,
// 不影响解析本身。JSON 坏了、或者顶层不是一个 object,都返回带路径的错误。
std::expected<FileConfig, std::string> ParseFileConfigJson(const std::string& json_text,
                                                             const std::string& file_path_for_error);

// 找配置文件:先 cwd 的 .lubancode.json,找不到再找用户主目录(Windows 取
// %USERPROFILE%)的 .lubancode.json。两处都没有,返回 std::nullopt(不算错)。
// 找到了但解析失败,返回错误(带路径)。
std::expected<std::optional<FileConfig>, std::string> LoadFileConfig();

// 真正的入口:读 LUBANCODE_ 专属环境变量、找并读配置文件、读通用环境变量,
// 按四级优先级合并出最终配置。
std::expected<ConfigResult, std::string> LoadFromEnv();

}  // namespace lubancode::config
