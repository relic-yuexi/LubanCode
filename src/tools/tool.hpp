// tools 层的工具基类。每个工具只管把一件事做好(读文件、跑命令……),
// 不关心是谁在调它——agent 层拿着 ToolRegistry 按名字找、按 schema 拼进请求。

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "platform/text_encoding.hpp"  // SanitizeExternalText:结果正文出门前的编码关口
#include "tools/tool_content.hpp"      // ToolResultPayload:富结果的唯一真账

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

// 权限核只认工具自报的稳定类别，不按可变工具名猜语义。None 与
// needs_confirm=false 严格配对；需确认工具必须显式声明其真实类别。
enum class ApprovalClass { None, FileEdit, FileDestructive, Command, External };

// 工具执行上下文(子代理 x 停止失效单:取消令牌贯通工具进程)。渐进迁移
// 的口子:RunOneTool 把"这一次调用"的取消旗从这里递进来,工具 override
// execute(input, context) 便可在长操作里查旗、收子进程树;没 override 的
// 旧工具走默认实现(照旧只跑 execute(input)),语义与从前一字不差——
// cancel_capability 的完整分档见那单,这里先只立"取消旗"这一格。
struct ToolExecutionContext {
    // 本调用的取消旗:主回合 = ESC 那根,子代理 = CancelChain 并出来的那根
    //(面板 x / 父轮 ESC / 墙钟三信号合一)。null = 本调用没有取消源
    //(旧调用方/单测),工具行为与从前一致。指针活期盖过 execute 返回,
    // 调用方保证;工具不得跨调用存它(共享工具实例会被多只并跑的子代理
    // 同时调,存下来就是互相踩——要存请存自己 SetCancel 灌的那根做兜底)。
    const std::atomic<bool>* cancel = nullptr;

    // 本调用的二进制 artifact 落盘目录(MCP 富结果单 P0.5):MCP 工具返回
    // 的图片/音频/blob 字节先落这里的会话 artifact store(<会话目录>/
    // mcp-artifacts/),history 里只留引用。空 = 本次没有落盘地(单测/
    // 未开会话的调用路),富二进制块按稳定错误收口,不吞字节也不冒充
    // 落盘。文本结果不受影响。
    std::string artifact_dir;
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

    // 审批类别由工具自报。默认 None 只适用于无需确认工具；所有
    // needs_confirm=true 的生产工具必须显式 override 成非 None。
    virtual ApprovalClass approval_class() const { return ApprovalClass::None; }

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
        // ---- MCP 富结果单:正文真账只有一份 --------------------------------
        // payload 是唯一权威(text/image/audio/resource_link/resource/
        // structuredContent 全在这);content 是 payload 的 TextProjection
        // 派生缓存,给只读它的旧调用方(终端显示、四家 wire 的文本降级、
        // token 估算)继续用。写入一律走 SetText/SetPayload/AppendText/
        // SanitizeInPlace——它们保证 content 与 payload 投影同步;src 内
        // 不许再直接给 content 赋值(那会造出第二份真账,单子明令禁止),
        // 也不许拿 content 反过来还原 payload。
        ToolResultPayload payload;
        std::string content;    // = payload 的文本投影(派生缓存,只读)
        bool is_error = false;  // 执行失败/被拒绝时置位

        // 文本桥(MCP 富结果单 P0.2):旧聚合初始化 Tool::Result{"文本", true}
        // 照旧编译——构造器把文本包成一枚 TextContent,行为与从前一字不差。
        Result() = default;
        // 不加 explicit:Tool::Result{"文本", true} 这类列表初始化遍布全仓,
        // 构造器必须以非显式身份接住它们(单参数 string 意外的隐式转换
        // 由第二参默认值的存在性挡住——bool/string 重载歧义不存在)。
        Result(std::string text, bool error = false)
            : payload(MakeTextPayload(text)), content(std::move(text)), is_error(error) {}

        // 工厂:显式意图,不再依赖聚合初始化的字段次序。
        static Result Text(std::string text) { return Result(std::move(text), false); }
        static Result Error(std::string text, bool error = true) { return Result(std::move(text), error); }
        static Result FromPayload(ToolResultPayload rich, bool error = false) {
            Result result;
            result.is_error = error;
            result.SetPayload(std::move(rich));
            return result;
        }

        // 唯一的写入口(同步 content 投影)。
        void SetText(std::string text) {
            payload = MakeTextPayload(text);
            content = std::move(text);
        }
        void SetPayload(ToolResultPayload rich) {
            payload = std::move(rich);
            content = TextProjection(payload);
        }
        // 追加一段文本(hook 反馈、附注):富结果在末尾添一枚 TextContent,
        // 纯文本直接接上;投影随之同步。
        void AppendText(std::string text) {
            payload.content.push_back(TextContent{std::move(text)});
            content = TextProjection(payload);
        }
        // UTF-8 规范化:payload 全部文本字段(含 structured JSON)洗一遍,
        // content 重算。幂等,合法时零成本。
        void SanitizeInPlace() {
            SanitizePayloadTextInPlace(payload);
            content = TextProjection(payload);
        }

        // ---- 逐枚追踪单:诊断扩展(缺省全空 = 与旧结构语义一致) ----------
        // is_error 留作 Provider/UI 的粗投影;诊断用稳定 outcome/error_code。
        // 便利构造 {content, is_error} 照旧编译(文本构造器接住),旧
        // builtin 一行不用改。原生 C ABI v1/v2 只回 is_error,宿主 wrapper
        // 把它投影成 tool_error/plugin_error,不添 struct 字段、不伪造
        // exception 细节(将来 ABI 再升才让插件明报细码)。
        std::string outcome;     // ToolOutcome 的稳定字符串(agent/tool_trace.hpp);空 = 未细报
        std::string error_code;  // 分层错误码(kErr* 常量);空 = 未细报
        nlohmann::json details = nlohmann::json::object();  // 结构化补充(exit_code 等)
        // details 只放诊断附注;不许拿它偷运图片字节或 structuredContent
        // (富内容走 payload,单子 P0.2 定案)。
        std::string effect_summary;  // 副作用一句话(改了哪个文件/起了哪个进程);可空
        // 大结果卸载(context artifact store)的 artifact id;与 payload 里
        // 内容块自带的 ArtifactRef(二进制字节的内容寻址落盘)是两笔账:
        // 前者是"整条文本结果太长卸了货",后者是"这块图片/音频/资源本身
        // 的字节落在哪"。前者可空;后者在富块里必填(未落盘时 stored=false)。
        std::string result_artifact;

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

    // 带执行上下文的执行口(子代理 x 停止失效单):RunOneTool 走这只,把
    // 当次调用的取消旗递给肯合作取消的工具(run_command 收进程树、Lua 掐
    // 指令钩子、插件收子进程)。默认实现忽略 context、退回旧口——没迁的
    // 工具零改动,行为不变;这也是为什么它不是纯虚:一夜之间要求全工具表
    // 接取消,等于把断点从 loop 挪到每只工具身上,规格明令不许。
    virtual Result execute(const nlohmann::json& input, const ToolExecutionContext& context) {
        (void)context;
        return execute(input);
    }
};

}  // namespace lubancode::tools
