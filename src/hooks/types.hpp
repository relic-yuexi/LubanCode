// hooks 框架的核心数据类型:来源、handler、匹配组、定义(带信任账)、
// 事件负载、归并后的决策、单次运行记录。dispatcher/protocol/trust/loader
// 都只认这里的形状。
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/config.hpp"
#include "hooks/events.hpp"

namespace lubancode::hooks {

// ---------------------------------------------------------------------------
// 来源。source_order 只管"日志与执行排序",不管覆盖——hooks 相加,项目
// 配置删不掉 user/managed 的任何一条(规格"信任与来源")。
// managed:管理员策略(LubanCode 暂无 managed 装载源,枚举先立着,信任上
// 按"策略信任、不可由普通用户在 UI 里关"对待)。
// user:    ~/.lubancode/config.json 里的 hooks。
// project: <cwd>/.lubancode/config.json 里的 hooks——未信任绝不起进程。
// local/plugin/skill/subagent:预留,本期没有装载源。
enum class HookSourceKind { Managed, User, Project, Local, Plugin, Skill, Subagent };

constexpr int SourceOrder(HookSourceKind kind) {
    switch (kind) {
        case HookSourceKind::Managed:
            return 0;
        case HookSourceKind::User:
            return 1;
        case HookSourceKind::Project:
            return 2;
        case HookSourceKind::Local:
            return 3;
        case HookSourceKind::Plugin:
            return 4;
        case HookSourceKind::Skill:
            return 5;
        case HookSourceKind::Subagent:
            return 6;
    }
    return 7;
}

constexpr std::string_view ToString(HookSourceKind kind) {
    switch (kind) {
        case HookSourceKind::Managed:
            return "managed";
        case HookSourceKind::User:
            return "user";
        case HookSourceKind::Project:
            return "project";
        case HookSourceKind::Local:
            return "local";
        case HookSourceKind::Plugin:
            return "plugin";
        case HookSourceKind::Skill:
            return "skill";
        case HookSourceKind::Subagent:
            return "subagent";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// 运行期定义 = 解析后的 handler + 来源账 + 信任状态。dispatcher 只吃这个。
// (HookHandlerConfig/HookMatcherGroupConfig 的配置形状住在 config/config.hpp,
// 由 config 层解析;hooks 层只加来源、hash 与信任账。)
struct HookDefinition {
    int id = 0;  // dispatcher 分配的稳定序号(/hooks 与运行记录引用它)
    HookEvent event = HookEvent::PreToolUse;
    HookSourceKind source_kind = HookSourceKind::User;
    std::string source_path;    // 配置文件绝对路径(信任键的一部分)
    std::string source_label;   // "user ~/.lubancode/config.json" 这类展示串
    int declaration_index = 0;  // 同一来源文件内的声明次序

    std::string matcher;
    bool regex = false;
    config::HookHandlerConfig handler;

    // legacy adapter 标记:旧四类(pre_tool 等)转来的定义。守旧语义——
    // 任意非零退出仍拦(pre_tool)、LUBAN_TOOL_* 环境变量照导、固定 30 秒、
    // shell 字符串照跑、不吃 stdin JSON。
    bool legacy = false;

    // definition hash:command/args/timeout/async/type 全算进去。命令一改,
    // hash 变,project 信任失效,须重审。user/managed hook 不走信任审查
    // (文件在用户自己目录里,改它的人本来就有全部权限),hash 照算(去重
    // 与 /hooks 展示用)。
    std::string definition_hash;       // 十六进制全串
    std::string definition_hash_short; // 前 12 位,/hooks 展示用

    // 运行期开关(trust store 持久化,内存里镜像):
    bool disabled = false;   // 用户在 /hooks 里禁用(managed 不可禁)
    bool trusted = false;    // project 来源是否已按当前 hash 信任
    bool deduped = false;    // 与同事件下另一只 hash 相同的定义去重(不执行)
};

// ---------------------------------------------------------------------------
// 事件负载。业务层只填事件特有字段,公共字段(session/cwd/……)由
// dispatcher 从 HookContext 补齐——公共字段只定义一遍(规格"统一 stdin
// JSON")。
struct HookPayload {
    HookEvent event = HookEvent::PreToolUse;
    // 工具事件:tool_name/tool_use_id/tool_input(/tool_response/tool_succeeded)。
    // UserPromptSubmit:prompt。SessionStart:source。SessionEnd:reason。
    // Pre/PostCompact:trigger。Subagent*:agent_id/agent_type/parent_agent_id/
    // agent_transcript_path/last_assistant_message/stop_hook_active。Stop:
    // stop_hook_active/last_assistant_message。
    nlohmann::json fields = nlohmann::json::object();
    // matcher 要匹配的值(工具名/source/reason/trigger;其余事件为空 = 只
    // 认 */缺省 matcher)。
    std::string match_value;
};

// 会话级上下文,dispatcher 每次发射都带出去。agent_id/agent_type 非空表示
// 当前在子代理里触发(主代理为 null)。
struct HookContext {
    std::string session_id;
    std::string turn_id;
    std::string cwd;
    std::string transcript_path;
    std::string permission_mode;  // confirm/auto/yolo
    std::optional<std::string> agent_id;
    std::optional<std::string> agent_type;
    std::optional<std::string> parent_agent_id;
    // 逐枚追踪单:本次 Emit 钉在哪枚工具执行上(可空)。dispatcher 把它
    // 抄进每条 HookRunRecord.tool_execution_id,工具 trace 侧只记引用,
    // 不把 /hooks runs 全文复制五遍。
    std::string tool_execution_id;
};

// ---------------------------------------------------------------------------
// 单只 handler 的一次运行记录。成功、失败、超时、spawn 失败、schema 不合、
// 未信任跳过、去重跳过——全都留痕(规格"失败策略":不许只往 cerr 丢一行)。
struct HookRunRecord {
    int definition_id = 0;
    std::string event_name;
    std::string definition_hash_short;
    std::string command_display;  // 可执行文件 + 参数(或 shell 串),给人看
    std::string source_label;
    // 结局分类:
    //   ok            正常跑完(exit 0,或 legacy 的"非零但只警告"档)
    //   blocked       exit 2 / legacy 非零拦截
    //   failure       其它非零退出码(hook 自己坏了)
    //   timeout       超时被杀
    //   spawn_failed  起不来
    //   schema_error  stdout JSON 字段用错/解析失败
    //   skipped_untrusted  project 未信任,跳过
    //   skipped_disabled   用户禁用,跳过
    //   skipped_dedupe     与同事件同 hash 定义去重,跳过
    //   skipped_async      async handler 本期不执行(见 dispatcher 注释)
    std::string outcome;
    unsigned long exit_code = 0;
    int duration_ms = 0;
    std::string detail;      // 阻断理由/stderr 摘要/错误说明
    std::string decision;    // 该 handler 单独表态:allow/deny/ask/none
    // stderr 的明示解码账(v2 路径;legacy 走合并 output,不填):首段、截断
    // 标志、解码口径。拿不准编码时 head 是原始字节摘要、encoding 为
    // "unknown"——台账如实,不无声替换。
    std::string stderr_head;         // stderr 首段(上限 kStderrHeadBytes,超出截断)
    bool stderr_truncated = false;   // stderr 超首段上限被截
    std::string stderr_encoding;     // "utf-8" / "cp936" / "unknown"
    std::int64_t timestamp_unix = 0;  // 落账时刻(秒)
    // 逐枚追踪单:这次运行钉在哪枚工具执行上(可空——非工具事件
    // UserPromptSubmit/Stop 类的运行没有 execution)。工具 trace 只记
    // 引用(pre/post_hook_run_ids),不复制全文五遍。
    std::string tool_execution_id;

    // stderr 首段的展示上限(字节)。够放一段 PowerShell 报错,又不至于刷屏。
    static constexpr std::size_t kStderrHeadBytes = 512;
};

// ---------------------------------------------------------------------------
// 一次 Emit 的归并结果。按事件读自己认得的字段;决策归并的法子固定
// (deny > ask > allow;PermissionRequest 无人表态 = 走原审批流程)。
struct HookEventResult {
    // 权限类决策(PreToolUse/PermissionRequest)。
    enum class Permission { None, Allow, Ask, Deny };
    Permission permission = Permission::None;
    std::string permission_reason;  // deny/ask 的理由,给用户与模型看

    // PreToolUse 改写后的入参(只与 allow 同返;重过 schema/deny 规则/权限)。
    std::optional<nlohmann::json> updated_input;
    bool updated_input_rejected = false;  // 改写被 schema/安全检查打回
    std::string updated_input_reject_reason;

    // continue=false 有谁拉了闸(UserPromptSubmit/PreCompact/Stop 类)。
    bool blocked = false;
    std::string block_reason;

    // 追加上下文与系统消息(拼接后由调用方送进 developer context/UI)。
    std::vector<std::string> additional_context;
    std::vector<std::string> system_messages;

    // 本次实际起了进程的 handler 数与全部记录(含跳过项)。
    int executed = 0;
    std::vector<HookRunRecord> records;

    bool HasAnyEffect() const {
        return permission != Permission::None || updated_input.has_value() || blocked ||
               !additional_context.empty() || !system_messages.empty();
    }
};

}  // namespace lubancode::hooks
