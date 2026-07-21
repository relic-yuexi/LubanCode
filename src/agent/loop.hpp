// agent 核心循环:user 消息入历史 -> 带工具发请求 -> 流式转发给上层(打字机
// 输出)同时喂给 assembler 攒消息 -> stop_reason 是 tool_use 就把模型要的
// 工具都执行一遍、结果攒成一条 user 消息喂回去 -> 再发请求 -> 如此往复,
// 直到 end_turn,或者达到轮数上限。

#pragma once

#include <atomic>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/context.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

namespace lubancode::agent {

struct Callbacks {
    // 流式文本增量,打字机效果打印用。
    std::function<void(const std::string& text)> on_text_delta;

    // 模型发起了一次工具调用,还没执行,给上层显示用(比如打印
    // `[工具] read_file {"path":...}`)。
    std::function<void(const std::string& name, const nlohmann::json& input)> on_tool_start;

    // 工具 needs_confirm() 为真时才会调用;返回 true 表示允许执行。
    // 没设这个回调、或者工具本来就不需要确认,都视为允许。
    std::function<bool(const std::string& name, const nlohmann::json& input)> on_tool_confirm;

    // 工具跑完了(不管成功、失败、被拒绝、还是压根没找到这个工具),都会调用一次。
    std::function<void(const std::string& name, const tools::Tool::Result& result)> on_tool_done;

    // 每一次到模型的独立请求结束时(MessageDone 到达那一刻)都会调用一次,
    // 把这一次的 usage 报出来。一次 Run() 内部可能因为工具调用来回好几趟,
    // 也就是好几次独立请求——这个回调按请求粒度触发,不是按 Run() 粒度,
    // 上层(main.cpp)自己决定要不要跨请求累计。可选;不设就跳过,不影响
    // 其余行为。
    std::function<void(const api::Usage& usage)> on_usage;

    // M9:hooks.pre_tool。工具已经找到、还没问确认、更没执行的时候调用一次;
    // 返回非空表示被拦截——值就是要塞进 tool_result 里的 is_error 说明文本,
    // 该工具这次不会真的执行(needs_confirm 的确认也不会问)。返回
    // std::nullopt(或者压根没设这个回调)表示放行,跟没有 hooks 系统时
    // 的行为完全一样——main.cpp 不配 hooks 时就不设这个回调。
    // agent/ 本身不知道、也不关心 hooks 具体怎么解析、怎么执行(config::/
    // tools::hooks 那一层的事),只提供这一个挂接点,好保持依赖单向
    // (agent/ 不反过来牵扯 config/)。
    std::function<std::optional<std::string>(const std::string& name, const nlohmann::json& input)> on_pre_tool_hook;

    // M9:hooks.post_tool。工具真的执行完了(拿到 Result)才调用一次;不会
    // 影响返回给模型的结果,单纯给上层一个"跑一下 post_tool 命令"的机会。
    // 不设就跳过。
    std::function<void(const std::string& name, const nlohmann::json& input, const tools::Tool::Result& result)>
        on_post_tool_hook;
};

// Run() 的收场情况。cancelled=true 表示这一轮是被 ESC 打断收场的——打断
// 不是错误(std::expected 的 value 分支,不是 error 分支),半截 assistant
// 文本已经照常带着打断标注入了历史,history() 状态完整、下一轮能正常接着
// 聊,调用方(main.cpp)只需要照这个标志决定要不要额外打提示。
struct RunOutcome {
    bool cancelled = false;
};

// 轮数将尽提醒:剩余轮数(max_turns - turn)不超过这个阈值时,当轮请求要在
// system 尾部附一句"收尾"提示,让模型别把话说到一半就被硬掐断。阈值定死
// 为 3,不做成可配置——任务要求实现侵入要小,3 轮的提前量对"体面收场"够用,
// 没必要再加一层配置项。
constexpr int kMaxTurnsNudgeThreshold = 3;

// 纯函数,可单测:第 turn 轮(0-based,对应 Run() 里 for 循环的循环变量)、
// 总共 max_turns 轮,判断这一轮该不该在请求里附加"轮数将尽"的提示。
bool ShouldNudgeMaxTurns(int turn, int max_turns);

class AgentLoop {
public:
    // max_turns:一次 Run() 里最多跟模型来回几趟(每趟一次工具调用算一趟),
    // 超过这个数还没到 end_turn 就报错退出,防止死循环。默认 100——25 的老
    // 默认值在真实多步骤开发任务里(装依赖、跑测试、修错这类正常节奏)太
    // 容易见顶,把模型正在修的错硬掐断;上限本身要留(管道模式没有 ESC 可
    // 打断,防真死循环),但不该矮到拦腰截断正常开发。main.cpp 里这个值改由
    // config.max_turns 传入(可经配置文件/环境变量调整),这里的默认参数只
    // 服务不经过 main.cpp 配置流程的调用方(单测、未来的其它入口)。
    // max_context_chars:发给模型前 history 裁剪的阈值(字符数),默认读
    // 环境变量 LUBANCODE_MAX_CONTEXT(没设置就是 kDefaultMaxContextChars)。
    AgentLoop(api::Backend& backend, tools::ToolRegistry& registry, std::string model,
              std::string system_prompt, int max_tokens = 4096, int max_turns = 100,
              std::size_t max_context_chars = MaxContextCharsFromEnv());

    // 发一轮用户输入。内部可能会跑好几个来回(工具调用),直到模型给出
    // end_turn(或者别的非 tool_use 的 stop_reason)才返回。历史跨多次
    // Run() 调用保留,下一句问话会带着之前的上下文。
    // cancel 非空且流式/工具执行期间被外部(cli 层的 ESC 监听线程)置位:
    // 半截 assistant 文本(如果已经流出来了)照常攒进历史,末尾附一段打断
    // 标注;工具循环发现自己被打断,已经在执行的那个工具的结果照常入历史、
    // 还没轮到的补一条"未执行"的合成 tool_result(保住 tool_use/tool_result
    // 成对约束,不然下一轮重放历史会被 API 拒绝);两种情况都从 Run() 正常
    // 返回(RunOutcome::cancelled = true),不是 std::unexpected——打断不是错误。
    std::expected<RunOutcome, std::string> Run(const std::string& user_input, const Callbacks& callbacks,
                                                const std::atomic<bool>* cancel = nullptr);

    // 跟字符串入口同义，只是调用方已经把本地图片装进 user_message 了。图片
    // 也须原样入 history，下一轮、重发、会话恢复才能带得上。
    std::expected<RunOutcome, std::string> Run(api::Message user_message, const Callbacks& callbacks,
                                                const std::atomic<bool>* cancel = nullptr);

    const std::vector<api::Message>& history() const { return history_; }

    // tool_search(延迟挂载):工具过滤谓词。设了之后,每轮请求的 tools
    // 数组只拼"谓词放行"的工具;模型调用了"注册表里查得到、谓词却不放行"
    // 的工具(延迟且未挂载)时,不执行,回一条友好错误让模型先走
    // tool_search。谓词由 main.cpp 注入(按 loaded 集合过滤),AgentLoop
    // 自己不懂什么叫"延迟"——不设(默认)行为跟从前完全一样。每轮现查
    // 而不是构造时定死,是因为 tool_search 命中会在一次 Run() 中途改变
    // loaded 集合,下一轮请求就得看到新挂载的工具。
    void SetToolFilter(std::function<bool(const tools::Tool&)> filter) { tool_filter_ = std::move(filter); }

    // /worktree 切换目录后只换运行环境段，已有聊天史要照留。主循环在下一
    // 次请求前换掉系统提示，文件工具则由进程 CWD 即刻接管。
    void SetSystemPrompt(std::string system_prompt) { system_prompt_ = std::move(system_prompt); }

    // M6.6:/compact 用。跟 history() 是同一份数据,单独起个大写名字是为了
    // 跟任务规矩"只许新增两个方法,不许改现有的"对齐——不改名、不改签名、
    // 不复用 history(),原样再加一份。
    const std::vector<api::Message>& History() const { return history_; }

    // M6.6:/compact 压缩完之后,把 AgentLoop 内部存的完整历史换成压缩后的
    // 那份(archive 消息 + 最近一轮完整对话)。是本次任务里唯一允许写
    // history_ 的新入口,agent/compact.cpp 里的 Compact() 本身不碰
    // AgentLoop,只管算出新历史,真正替换由调用方(main.cpp)拿到新历史后
    // 调这个方法完成。
    void ReplaceHistory(std::vector<api::Message> new_history) { history_ = std::move(new_history); }

private:
    api::Backend& backend_;
    tools::ToolRegistry& registry_;
    std::string model_;
    std::string system_prompt_;
    int max_tokens_;
    int max_turns_;
    std::size_t max_context_chars_;
    std::vector<api::Message> history_;
    std::function<bool(const tools::Tool&)> tool_filter_;  // tool_search:空 = 不过滤,全量直挂

    std::vector<api::ToolDefinition> BuildToolDefinitions() const;
};

}  // namespace lubancode::agent
