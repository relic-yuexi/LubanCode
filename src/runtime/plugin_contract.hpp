// 本地 Tool 插件运行时的冻结合同(plugins 运行时单第 1 步)。
//
// 这一层只放"合同"——数据形状、错误码、请求/响应帧、manifest 的静态
// 校验规则。不管进程怎么起、Lua 怎么嵌、DLL 怎么载(那是 ProcessPluginRuntime
// / EmbeddedLuaRuntime / NativeLibraryRuntime 的事,分别在后续批次落地)。
//
// 三条铁律(单子「核心定案」「Schema 的方向不能倒」各节):
//   1. Schema 只从插件作者手里来(manifest 静态真账),模型输入不是权限
//      来源;Schema 坏了拒绝加载,绝不悄悄宽化成 {"type":"object"}。
//   2. 模型只见 name/description/input_schema。language/command/env/
//      timeout/permissions 一概不进 prompt——定义导出时宿主元数据剥干净。
//   3. 完整工具名 `plugin__<id>__<tool>`,重复名在注册前拒绝,注册表里
//      绝不留两枚同名。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "platform/paths.hpp"
#include "platform/text_encoding.hpp"

namespace lubancode::runtime {

// ---------------------------------------------------------------------------
// 错误码:宿主侧终态的唯一口径(plugins 单「本地进程协议 v1」与「验收」
// 各条)。进程调用链上每一种死法各有各的码,ItemCompleted 用它落账,不靠
// 解析文案猜。值即 ABI,只增不改。
// ---------------------------------------------------------------------------
enum class PluginErrorCode {
    Ok = 0,            // 成功
    ManifestInvalid,   // plugin.json 解析/校验失败(manifest 是门,坏了拒绝加载)
    DuplicateTool,     // 工具重名:同一插件内或跨插件撞了完整名 plugin__<id>__<tool>
    SpawnFailed,       // 进程起不来(命令不存在/权限/管道建失败)
    ToolExitNonZero,   // 进程非零退出(插件自己业务失败,退出码在附注里)
    ToolSignaled,      // 进程被信号杀死/崩溃(crash)
    TimedOut,          // 超时:温和终止 + grace 后杀整棵进程树
    Cancelled,         // ESC/取消链,与超时同一条收尾路但分型独立
    BadUtf8,           // stdout 不是合法 UTF-8(不做编码猜测,协议错)
    BadJson,           // stdout 不是恰好一份合法 JSON(混日志/多帧/空/坏文法)
    CallIdMismatch,    // 响应 call_id 与请求对不上
    OutputTooLarge,    // stdout/stderr 超字节帽,停读杀进程
    UnknownContent,    // 响应 content 里有 v1 不认的 type(只认 text)
    PluginReportedError,  // 响应 ok=false:插件自己报的错(信插件的话)
    InternalError,    // 宿主侧兜底(不该发生;发生即 bug)
    // 工具结果图片回喂单(v2):响应里的 image 块没能落账——坏 base64/
    // 伪 MIME(魔数对不上)/超字节帽/没有 artifact 落盘地。枚举只增不改,
    // 故追加在尾。
    ImageRejected,
};

// 错误码的人读名字(稳定,诊断/测试断言用)。
inline std::string_view PluginErrorCodeName(PluginErrorCode code) {
    switch (code) {
        case PluginErrorCode::Ok:
            return "ok";
        case PluginErrorCode::ManifestInvalid:
            return "manifest_invalid";
        case PluginErrorCode::DuplicateTool:
            return "duplicate_tool";
        case PluginErrorCode::SpawnFailed:
            return "spawn_failed";
        case PluginErrorCode::ToolExitNonZero:
            return "tool_exit_non_zero";
        case PluginErrorCode::ToolSignaled:
            return "tool_signaled";
        case PluginErrorCode::TimedOut:
            return "timed_out";
        case PluginErrorCode::Cancelled:
            return "cancelled";
        case PluginErrorCode::BadUtf8:
            return "bad_utf8";
        case PluginErrorCode::BadJson:
            return "bad_json";
        case PluginErrorCode::CallIdMismatch:
            return "call_id_mismatch";
        case PluginErrorCode::OutputTooLarge:
            return "output_too_large";
        case PluginErrorCode::UnknownContent:
            return "unknown_content";
        case PluginErrorCode::PluginReportedError:
            return "plugin_reported_error";
        case PluginErrorCode::InternalError:
            return "internal_error";
        case PluginErrorCode::ImageRejected:
            return "image_rejected";
    }
    return "internal_error";
}

// ---------------------------------------------------------------------------
// RuntimeKind:manifest 里 runtime.kind 的解析产物。language 不当 runtime
// (Python 也能冻成 executable,Rust 也能出 dylib)——分派只看 kind +
// command/args。v1 只落 embedded-lua 与 process;native-library 是后续
// 批次的占位,manifest 里写了就按"当前宿主不支持"拒绝,不静默宽化。
// ---------------------------------------------------------------------------
enum class RuntimeKind {
    EmbeddedLua,     // 内嵌 Lua(现有 *.lua 一文件一工具的归处)
    Process,         // 默认主路:stdin JSON -> stdout JSON 的短命进程
    NativeLibrary,   // 跨平台原生库(第 5/6 步的活,先占位)
};

inline std::string_view RuntimeKindName(RuntimeKind kind) {
    switch (kind) {
        case RuntimeKind::EmbeddedLua:
            return "embedded-lua";
        case RuntimeKind::Process:
            return "process";
        case RuntimeKind::NativeLibrary:
            return "native-library";
    }
    return "process";
}

// ---------------------------------------------------------------------------
// 代码点规矩:id/name/language 用这一个:只收 [A-Za-z0-9_-],首字符须是
// 字母或数字,长度 [1, max_len]。工具完整名 plugin__<id>__<tool> 的三段
// 都过这一道,模型看到的工具名就是 ASCII 安全串,不经任何 shell。
// version 另有自己的规矩(IsValidPluginVersion:再多收一个 '.')。
// ---------------------------------------------------------------------------
bool IsValidPluginIdentifier(std::string_view value, std::size_t max_len);
bool IsValidPluginVersion(std::string_view value, std::size_t max_len);

// 工具完整名:plugin__<id>__<tool>。
std::string BuildPluginToolName(std::string_view plugin_id, std::string_view tool_name);

// ---------------------------------------------------------------------------
// ToolWireName(统一 Package 封装单·契约 docs/reference/packages.md §6.1)。
// packaged 组件的 canonical id(如 moontide.full-stack:dom-analyzer 的工具
// ID 段 moontide.full-stack.dom-analyzer)里有点号,进不了各 provider 的
// [A-Za-z0-9_-] 收口。故定一枚可逆百分号编码:canonical 名里每个落在
// [A-Za-z0-9_-] 之外的字符按字节换成 %HH(大写十六进制)。'%' 本身不在
// 合法集里,原文从不出现裸 '%',解码唯一、可逆。全仓唯一实现——别处不
// 许各写字符串替换(点改下划线不可逆,明令禁止)。
// ---------------------------------------------------------------------------
// ID 段编码:moontide.full-stack.dom-analyzer -> moontide%2Efull-stack%2Edom-analyzer。
std::string EncodeToolWireId(std::string_view component_id);
// 解码(可逆性验收):遇到非法 %HH 或裸 '%' 返回 nullopt,不猜。
std::optional<std::string> DecodeToolWireId(std::string_view encoded);

// 打包组件工具的两枚名字。wire 名发给 provider 与权限账;display 名给人
// 看(/tools、/mcp、/package show 用)。kind_prefix 是 "plugin" 或 "mcp"。
std::string BuildPackagedToolWireName(std::string_view kind_prefix, std::string_view package_id,
                                      std::string_view local_id, std::string_view tool);
std::string BuildPackagedToolDisplayName(std::string_view kind_prefix, std::string_view package_id,
                                         std::string_view local_id, std::string_view tool);
// 编码后的完整工具名长度帽(契约 §6.1:超 64 字符 doctor 报错)。
inline constexpr std::size_t kToolWireNameMaxLength = 64;

// ---------------------------------------------------------------------------
// PluginDefinition:一件工具的中立定义。模型可见的只有 name/description/
// input_schema 三样;其余字段是宿主元数据,ToolRegistry 拼请求时一概剥掉
// (PluginToolAdapter 只导出这三样)。execution 是 manifest 里该工具的
// entry 名,进程协议请求里原样带 tool/entry,脚本自行分派。
// ---------------------------------------------------------------------------
struct PluginDefinition {
    std::string plugin_id;        // 插件 id(manifest.id)
    std::string name;             // 工具短名(manifest.tools[].name)
    std::string full_name;        // plugin__<id>__<tool>(构造时拼好)
    std::string description;      // 模型可见说明
    std::string entry;            // manifest.tools[].entry(脚本侧分派键)
    nlohmann::json input_schema;  // 模型可见入参 schema(强校验过的)
};

// ---------------------------------------------------------------------------
// PluginManifest:plugin.json 解析 + 静态校验后的产物。解析失败/校验不过
// 一律整件拒绝(ManifestInvalid + 人话),绝不部分加载、绝不宽化。
// ---------------------------------------------------------------------------
struct PluginManifest {
    std::string id;        // 插件 id
    std::string version;   // 语义展示用,不参与分派
    std::string language;  // 只给 /plugins、doctor、错误提示;不进 prompt
    RuntimeKind kind = RuntimeKind::Process;

    // process 运行时的命令行(已过 ${plugin_dir} 结构化替换,canonical 后
    // 仍须位于插件目录内)。argv[0] 是可执行文件。
    std::vector<std::string> argv;
    int timeout_ms = 30000;  // 进程墙钟;0 = 不设(不推荐)

    // manifest 里声明要额外递给子进程的环境变量名(allowlist)。宿主只递
    // 最小继承集(PATH 与必要系统变量)+ 这张表点名的变量;密钥一概不递。
    std::vector<std::string> env_allowlist;

    std::filesystem::path plugin_dir;  // canonical 后的插件目录

    std::vector<PluginDefinition> tools;  // 至少一件;重名在解析期就拒

    // permissions 段(单子 Manifest v1)。v1 只记账不做执法:cwd 缺省项目根
    // 由运行时保证;network/env 的执法在后续批次接信任账时落。
    bool network_allowed = false;
};

// manifest_version 1。将来升版,这个值变了就得明报 legacy/native-v1,不
// 静默拿错结构体。
inline constexpr int kPluginManifestVersion = 1;

// plugin.json 的解析 + 强校验入口。
//   manifest_json:plugin.json 全文。
//   plugin_dir:该插件目录(canonical 化由这里做;${plugin_dir} 替换后须
//     仍位于此目录内,逃出去就拒)。
// 失败返回 ManifestInvalid + 人话(点名哪一项、为什么不悄悄宽化)。
// 注意:Schema 坏了在这里拒绝整个插件,与 DLL 路径"退化成宽 object"的
// 老行为刻意相反——宽化会让模型乱传参数,也绕过作者想守的边界。
std::expected<PluginManifest, std::string> ParsePluginManifest(const std::string& manifest_json,
                                                               const std::filesystem::path& plugin_dir);

// ${plugin_dir} 结构化路径替换:只认这一个占位符、只在 args 与 env value
// 里出现(manifest 校验里已限)。不是字符串拼接 shell——产物直接当 argv
// 元素过 CreateProcess/execve,无注入面。导出给单测。
std::expected<std::string, std::string> ExpandPluginDirPlaceholder(std::string_view text,
                                                                   const std::filesystem::path& plugin_dir);

// ---------------------------------------------------------------------------
// 进程协议 v1 的请求/响应帧(单子「本地进程协议 v1」)。
//
// 请求恰好一份 JSON 写进 stdin,写完即关(脚本可 json.load(sys.stdin) 读
// 到 EOF);响应恰好一份 JSON 从 stdout 收,前后混日志即协议错。call_id 由
// 宿主生成,与 turn/item 对上;响应对不上就 CallIdMismatch。
// ---------------------------------------------------------------------------
namespace plugin_protocol {

// 协议 v2(工具结果图片回喂单):v1 之上加一枚 content 块 type=image——
// 工具能把图直接回喂模型(截图、图表、渲染结果)。协商向后兼容:宿主请求
// 帧说 2;插件看得懂就回 protocol=2 并许带 image 块,看不懂的 v1 旧插件
// 照旧回 1(它们根本不读请求帧的 protocol 字段),宿主两侧都收。image 块
// 只在响应 protocol=2 时合法;protocol=1 的响应里冒出 image 仍是
// UnknownContent——v1 的铁律不动。
inline constexpr int kProtocolVersionV1 = 1;
inline constexpr int kProtocolVersion = 2;  // 宿主当前说的版本(请求帧用它)

// 请求帧序列化。context 首版只有 cwd(项目根)与非敏感 call_id;会话历史、
// system prompt、API key、全份环境变量一概不送。
struct ProcessRequest {
    int protocol = kProtocolVersion;
    std::string call_id;
    std::string plugin;  // manifest.id
    std::string tool;    // manifest.tools[].name
    std::string entry;   // manifest.tools[].entry(脚本分派键)
    nlohmann::json arguments;
    std::string context_cwd;  // 项目根(UTF-8)
};

nlohmann::json SerializeRequest(const ProcessRequest& request);

// v2 的 image 块解析产物:来源二选一——data(base64 正文,不带前缀)或
// path(插件自己落好的文件)。宿主侧统一过大小帽 + 魔数复核再落会话
// artifact(规矩与 MCP 富结果同源),这里只管形状,不碰文件不碰解码。
struct ResponseImage {
    std::string mime_type;    // 声明的 MIME(image/png 等;宿主魔数复核)
    std::string data_base64;  // data 来源:base64(与 path 二选一)
    std::string path;         // path 来源:插件写的文件路径(UTF-8)
};

// 响应帧:stdout 收来的原始字节先过 UTF-8 校验,再整体 parse,再对
// call_id;三关全过才轮到字段校验。任何一关失败都映射成稳定错误码,
// 由 ParseResponse 带回,调用方据此落唯一终态。
struct ProcessResponse {
    int protocol = kProtocolVersion;
    std::string call_id;
    bool ok = false;
    std::string text;                  // content[] 里 type=text 的合并文本(v1 只认 text)
    std::vector<ResponseImage> images; // v2 的 image 块(protocol=2 才收)
    nlohmann::json structured;         // 可选;送模型仍须有明确 text
    std::string error_code;            // ok=false 时插件自报的码(原样收,不当宿主码)
    std::string error_message;         // ok=false 时的人话
};

struct ParsedResponse {
    PluginErrorCode status = PluginErrorCode::Ok;  // 非 Ok = 协议错,响应该作废
    std::string detail;                            // 协议错的人话
    ProcessResponse response;                      // status==Ok 时有效
};

ParsedResponse ParseResponse(std::string_view stdout_bytes, std::string_view expected_call_id);

}  // namespace plugin_protocol

// ---------------------------------------------------------------------------
// 调用参数校验:required/type/enum/const/min/max/length/array/object/
// additionalProperties 子集(单子「Schema 的方向不能倒」)。实现层仍须
// 防御性验参——Schema 不是内存安全。
// 与 tools/schema_check.cpp 的差别:那份是钩子改参后的"拦明显不对"从严
// 子集(不拦声明怪);这份是插件工具的调用前统一验证,嵌套递归、子集全
// 落,声明怪也拦(插件作者是外人,manifest 是合同,合同写岔了要在加载
// 期和调用期都看得见)。
// 返回 nullopt = 通过;有值 = 人话错误(第一个撞上的问题)。
// ---------------------------------------------------------------------------
std::optional<std::string> ValidateArgumentsAgainstSchema(const nlohmann::json& input, const nlohmann::json& schema);

}  // namespace lubancode::runtime
