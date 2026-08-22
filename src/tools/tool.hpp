// tools 层的工具基类。每个工具只管把一件事做好(读文件、跑命令……),
// 不关心是谁在调它——agent 层拿着 ToolRegistry 按名字找、按 schema 拼进请求。

#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace lubancode::tools {

// 副作用等级(逐枚追踪单"Effect class 与恢复策略")。声明是保守承诺:
// 只影响崩溃后的恢复建议,不越过权限确认;未声明的注册按最危险档
// InProcessUnknown。定义在 Tool 头而不是 agent/tool_trace.hpp,好让
// tools 层注册元数据时不牵 agent 依赖(agent 侧另有一份同名枚举做
// 持久化字符串映射,两侧语义一一对应,见 agent/tool_trace.hpp)。
enum class EffectClass {
    ReadOnlyLocal,       // read_file/search:可建议重试
    ReadOnlyRemote,      // web_fetch/只读 MCP:不自动重试(费用/限流)
    LocalReversible,     // write_file/edit_file:查 undo token 再询问
    LocalProcessUnknown, // run_command:unknown,先核验
    RemoteIdempotent,    // 带 idempotency key:按 key 查,不直接重发
    RemoteCompensatable, // 支持 delete/cancel:可提补偿(另一枚可见调用)
    RemoteIrreversible,  // 发信/付款/发布:只告警与人工核验
    InProcessUnknown,    // 未声明的 native/Lua:按未知副作用处理
};

// 幂等性声明(重试规矩用):工具自报"同参数重跑是否等价"。只服务恢复
// 建议,不构成自动重试的许可——unknown_after_start 默认禁止一键重试。
enum class Idempotency { Unknown, Idempotent, NonIdempotent };

// 恢复能力声明:工具/插件 manifest 能承诺什么,宿主才做什么。没承诺的
// 一概不做(不替 MCP server 猜幂等,不替 Plugin 猜 compensator)。
enum class RecoveryCapability {
    None,                  // 无补偿协议:只能告警、核验、人工处置
    Retryable,             // 可安全重试(只读本地)
    ConditionallyUndoable, // 本地文件:undo token + 条件式撤销
    Compensatable,         // 反向业务动作(另作一枚可见工具调用)
};

class Tool {
public:
    virtual ~Tool() = default;

    // 工具名,模型靠这个发起调用(tool_use 里的 name)。
    virtual std::string name() const = 0;
    virtual std::string description() const = 0;

    // 入参的 JSON Schema,手写、拼进请求里给模型看。
    virtual nlohmann::json input_schema() const = 0;

    // 执行前要不要先问用户一句。默认不用(比如 read_file 这种只读的)。
    virtual bool needs_confirm() const { return false; }

    // tool_search:这个工具是不是"延迟挂载"的——注册表总工具数超过阈值时,
    // 延迟工具不直接进请求的 tools 数组,只在系统提示的索引段里露个名字,
    // 模型用 tool_search 检索命中后才真正挂载。默认 false(内置九件套和
    // web_fetch/web_search 都是核心,恒在);McpTool(经 DeferredTool 包装)、
    // PluginTool、LuaTool 这些外挂工具才是 true——它们是工具表膨胀的主力。
    virtual bool deferred() const { return false; }

    // ---- 逐枚追踪单:注册元数据(默认全保守) ----------------------------
    // 诊断层不许用 dynamic_cast 猜来源——这些 getter 由各 runtime 在注册
    // 时明写。默认值:未声明副作用按 InProcessUnknown(最危险档)、幂等
    // Unknown、恢复能力 None、version 摘要空。DeferredTool 透传内层的
    // 回答,不把来源洗成 deferred(见 DeferredTool 的 override)。
    virtual EffectClass effect_class() const { return EffectClass::InProcessUnknown; }
    virtual Idempotency idempotency() const { return Idempotency::Unknown; }
    virtual RecoveryCapability recovery_capability() const { return RecoveryCapability::None; }
    // 版本/摘要(MCP server 版本、插件 digest 等;可空,诊断展示用)。
    virtual std::string version_or_digest() const { return std::string(); }

    struct Result {
        std::string content;    // 回传给模型的文本(成功的结果,或者人能看懂的错误说明)
        bool is_error = false;  // 执行失败/被拒绝时置位

        // ---- 逐枚追踪单:诊断扩展(缺省全空 = 与旧结构语义一致) ----------
        // is_error 留作 Provider/UI 的粗投影;诊断用稳定 outcome/error_code。
        // 便利构造 {content, is_error} 照旧编译(聚合初始化前两字段),旧
        // builtin 一行不用改。原生 C ABI v1/v2 只回 is_error,宿主 wrapper
        // 把它投影成 tool_error/plugin_error,不添 struct 字段、不伪造
        // exception 细节(将来 ABI 再升才让插件明报细码)。
        std::string outcome;     // ToolOutcome 的稳定字符串(agent/tool_trace.hpp);空 = 未细报
        std::string error_code;  // 分层错误码(kErr* 常量);空 = 未细报
        nlohmann::json details = nlohmann::json::object();  // 结构化补充(exit_code 等)
        std::string effect_summary;  // 副作用一句话(改了哪个文件/起了哪个进程);可空
        std::string result_artifact; // 大结果落仓后的 artifact id;可空

        // ---- 本地文件条件式撤销(逐枚追踪单第四期) ----
        // write_file/edit_file 产:撤销前重读目标,当前 sha 仍等于
        // postimage 才可恢复 preimage(新建文件只在内容未再变时可移走)。
        // preimage 超上限不内联(available() 如实报 false),不拿半截原文
        // 冒充可恢复。undo 是宿主侧的另一枚工具调用,须确认、留 compensates
        // 关系、失败留账——这些在装配层,不在这里。
        std::string undo_path;
        std::string undo_preimage_sha256;
        std::string undo_postimage_sha256;
        std::string undo_preimage;
        bool undo_created_new_file = false;
        bool undo_available() const {
            return !undo_path.empty() && !undo_postimage_sha256.empty() &&
                   (!undo_created_new_file || !undo_preimage.empty());
        }
    };

    // undo preimage 的内联字节上限(逐枚追踪单:不默认存第二份正文,超大
    // 改动的撤销交给 Git/worktree 检查点,不塞进结果结构)。
    static constexpr std::uint64_t kToolUndoPreimageCap = 256 * 1024;

    // 真正执行。input 是模型给的入参(已经是解析好的 JSON 对象)。
    // 失败(参数错、文件不存在、命令跑挂了……)不抛异常,用 Result::is_error 传回去。
    virtual Result execute(const nlohmann::json& input) = 0;
};

}  // namespace lubancode::tools
