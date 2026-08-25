// 请求前缀守恒(前缀缓存守恒单第三期):IsAppendOnlySuccessor/DiffRequests
// 纯函数钉死,再用捕获 backend 复现规格点名的六桩断裂源。
//
// 稳定前缀是三家 wire 共用的请求纪律:已经发给模型的 system、tools 与旧
// 消息不得追改,新材料只往尾部添。断了不可耻——compact、tool_search 挂载
// 都是有意的 epoch break;无名无姓、每轮偷偷断一次才可耻。这里的用例分
// 两类:
//   1) 追加律成立的路径(默认工具往返、tool_search 断一次后恢复)直接钉绿;
//   2) 现行实现里仍在偷偷断的桩(记忆 suffix/名册、步数 nudge、结构压缩
//      冷热切换、hard trim 滑窗),先把"断"钉成案——断因必须被 DiffRequests
//      点名;后续各期(动态 suffix 挪尾/首次定形/sticky view)把断修掉时,
//      这些用例翻成"追加律成立"。

#include <doctest/doctest.h>

#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "agent/loop.hpp"
#include "agent/prefix.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;
using lubancode::agent::IsAppendOnlySuccessor;

namespace {

// 按脚本吐事件的假后端:记下每份 api::Request,断言前缀形状用。
class CaptureBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::vector<api::Request> captured;

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
        captured.push_back(request);
        const std::size_t idx = captured.size() - 1;
        if (idx >= scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "CaptureBackend: 脚本用完了", 0});
        }
        for (const auto& event : scripts[idx]) {
            on_event(event);
        }
        return {};
    }
};

class FixedTool : public tools::Tool {
public:
    FixedTool(std::string name, std::string result)
        : name_(std::move(name)), result_(std::move(result)) {}

    std::string name() const override { return name_; }
    std::string description() const override { return "fixed tool for prefix tests"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }

    tools::Tool::Result execute(const nlohmann::json&) override { return {result_, false}; }

private:
    std::string name_;
    std::string result_;
};

std::vector<api::StreamEvent> TextScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id, const std::string& tool_name) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, tool_id, tool_name},
        api::ToolUseInputDelta{0, "{}"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

api::Message UserText(std::string text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{std::move(text)});
    return message;
}

}  // namespace

// ---------------------------------------------------------------------------
// DiffRequests / IsAppendOnlySuccessor 纯函数
// ---------------------------------------------------------------------------

TEST_CASE("DiffRequests: 尾部追加消息 = 追加律成立") {
    api::Request prev;
    prev.model = "m";
    prev.system = "s";
    prev.tools.push_back({"t", "d", nlohmann::json{{"type", "object"}}});
    prev.messages.push_back(UserText("你好"));

    api::Request next = prev;
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::TextBlock{"你好!"});
    next.messages.push_back(assistant);

    const agent::PrefixDiff diff = agent::DiffRequests(prev, next);
    CHECK(diff.append_only());
    CHECK(IsAppendOnlySuccessor(prev, next));
    CHECK(diff.appended_messages == 1);
    CHECK(diff.break_reason().empty());
}

TEST_CASE("DiffRequests: model/system/tools 任一动都点名") {
    api::Request prev;
    prev.model = "m";
    prev.system = "s";
    prev.tools.push_back({"t", "d", nlohmann::json{{"type", "object"}}});

    api::Request model_bumped = prev;
    model_bumped.model = "m2";
    CHECK(agent::DiffRequests(prev, model_bumped).break_reason() == "model_changed");

    api::Request system_bumped = prev;
    system_bumped.system = "s2";
    CHECK(agent::DiffRequests(prev, system_bumped).break_reason() == "system_changed");

    api::Request tools_bumped = prev;
    tools_bumped.tools.push_back({"t2", "d", nlohmann::json{{"type", "object"}}});
    CHECK(agent::DiffRequests(prev, tools_bumped).break_reason() == "tools_changed");

    // schema 内容变了也算 tools 变(缓存认字节,不只认名字)。
    api::Request schema_bumped = prev;
    schema_bumped.tools[0].input_schema = nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}};
    CHECK(agent::DiffRequests(prev, schema_bumped).tools_changed);
}

TEST_CASE("DiffRequests: 旧消息被追改/被裁都点名,位置报得出来") {
    api::Request prev;
    prev.messages.push_back(UserText("一"));
    prev.messages.push_back(UserText("二"));

    api::Request rewritten = prev;
    rewritten.messages[0].content[0] = api::TextBlock{"一(改)"};
    const agent::PrefixDiff diff = agent::DiffRequests(prev, rewritten);
    CHECK_FALSE(diff.append_only());
    CHECK(diff.break_reason() == "old_message_changed");
    CHECK(diff.old_message_changed_at == 0);

    api::Request shrunk = prev;
    shrunk.messages.pop_back();  // 旧消息被裁掉
    const agent::PrefixDiff shrink_diff = agent::DiffRequests(prev, shrunk);
    CHECK_FALSE(shrink_diff.append_only());
    CHECK(shrink_diff.break_reason() == "old_message_changed");
}

TEST_CASE("FingerprintRequest/DiffFingerprints: 与逐字节 diff 同一套判定") {
    api::Request prev;
    prev.model = "m";
    prev.system = "s";
    prev.messages.push_back(UserText("你好"));

    api::Request appended = prev;
    appended.messages.push_back(UserText("再问"));
    CHECK(agent::DiffFingerprints(agent::FingerprintRequest(prev), agent::FingerprintRequest(appended))
              .append_only());

    api::Request rewritten = prev;
    rewritten.system = "s2";
    CHECK(agent::DiffFingerprints(agent::FingerprintRequest(prev), agent::FingerprintRequest(rewritten))
              .break_reason() == "system_changed");

    // 同一份请求指纹稳定(两次算逐位相等)。
    const auto f1 = agent::FingerprintRequest(prev);
    const auto f2 = agent::FingerprintRequest(prev);
    CHECK(f1.model == f2.model);
    CHECK(f1.system_hash == f2.system_hash);
    CHECK(f1.tools_hash == f2.tools_hash);
    CHECK(f1.message_hashes == f2.message_hashes);
}

// ---------------------------------------------------------------------------
// 钉追加律:默认普通工具往返(绿)
// ---------------------------------------------------------------------------

TEST_CASE("前缀: 默认工具往返,后一份请求是前一份的原样追加版") {
    CaptureBackend backend;
    backend.scripts = {
        ToolUseScript("call_1", "read_file"),
        TextScript("好了"),
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FixedTool>("read_file", "正文"));

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    agent::Callbacks callbacks;
    std::vector<api::UsageReport> reports;
    callbacks.on_usage = [&](const api::UsageReport& report) { reports.push_back(report); };

    const auto result = loop.Run("帮我读一下", callbacks);
    REQUIRE(result.has_value());
    REQUIRE(backend.captured.size() == 2);

    CHECK(IsAppendOnlySuccessor(backend.captured[0], backend.captured[1]));
    const agent::PrefixDiff diff = agent::DiffRequests(backend.captured[0], backend.captured[1]);
    CHECK(diff.appended_messages == 2);  // assistant(tool_use) + tool result
    CHECK(diff.break_reason().empty());

    // 台账:两步都在 epoch 1,没断。
    REQUIRE(reports.size() == 2);
    CHECK(reports[0].cache_epoch == 1);
    CHECK(reports[0].prefix_append_only);
    CHECK(reports[1].cache_epoch == 1);
    CHECK(reports[1].prefix_append_only);
    CHECK(reports[1].epoch_break_reason.empty());
}

TEST_CASE("前缀: 按需 artifact 摘要只追加 tool result,不追改旧消息") {
    CaptureBackend backend;
    backend.scripts = {
        ToolUseScript("call_summary", "context_read"),
        TextScript("我看完摘要了"),
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FixedTool>(
        "context_read", "artifact a0001 按需摘要:构建通过。原文未改。"));

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    agent::Callbacks callbacks;
    std::vector<api::UsageReport> reports;
    callbacks.on_usage = [&](const api::UsageReport& report) { reports.push_back(report); };

    REQUIRE(loop.Run("摘要这枚 artifact", callbacks).has_value());
    REQUIRE(backend.captured.size() == 2);
    CHECK(IsAppendOnlySuccessor(backend.captured[0], backend.captured[1]));
    CHECK(agent::DiffRequests(backend.captured[0], backend.captured[1]).break_reason().empty());
    REQUIRE(backend.captured[1].messages.size() == backend.captured[0].messages.size() + 2);
    const auto* result = std::get_if<api::ToolResultBlock>(
        &backend.captured[1].messages.back().content.front());
    REQUIRE(result != nullptr);
    CHECK(result->content.find("按需摘要") != std::string::npos);
    REQUIRE(reports.size() == 2);
    CHECK(reports[1].cache_epoch == 1);
    CHECK(reports[1].prefix_append_only);
}

// ---------------------------------------------------------------------------
// 断裂源一(已修):记忆 suffix 与任务名册不再改 system——第五期起随本轮
// user 消息尾部进请求视图,发过即钉住,新一轮再往尾部添新快照。旧前缀
// 逐字节不动。
// ---------------------------------------------------------------------------

TEST_CASE("前缀: 轮间换动态上下文(记忆/名册)不改 system,旧前缀原样追加") {
    CaptureBackend backend;
    backend.scripts = {TextScript("答一"), TextScript("答二"), TextScript("答三")};
    tools::ToolRegistry registry;

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    agent::Callbacks callbacks;

    loop.SetTurnContext("项目记忆召回(甲)");
    REQUIRE(loop.Run("第一句", callbacks).has_value());
    loop.SetTurnContext("项目记忆召回(乙)");
    REQUIRE(loop.Run("第二句", callbacks).has_value());
    loop.SetTurnContext("项目记忆召回(丙)");
    REQUIRE(loop.Run("第三句", callbacks).has_value());
    REQUIRE(backend.captured.size() == 3);

    // system 三份一字不差:动态材料不再进头段。
    CHECK(backend.captured[0].system == "system prompt");
    CHECK(backend.captured[1].system == "system prompt");
    CHECK(backend.captured[2].system == "system prompt");

    // 上下文进了当轮 user 消息的尾部块,旧前缀原样追加。
    const agent::PrefixDiff diff_12 = agent::DiffRequests(backend.captured[0], backend.captured[1]);
    const agent::PrefixDiff diff_23 = agent::DiffRequests(backend.captured[1], backend.captured[2]);
    CHECK(diff_12.append_only());
    CHECK(diff_23.append_only());
    CHECK(diff_12.appended_messages == 2);
    CHECK(diff_23.appended_messages == 2);
}

// ---------------------------------------------------------------------------
// 断裂源二(已修):步数将尽提醒不再改 system——第五期起改成固定文案,
// 在"剩余步数第一次降到阈值"那一步追加进尚未发出的尾部消息,随 history
// 留住;后头不改数字、不撤旧提醒,system 全程一字不动。
// ---------------------------------------------------------------------------

TEST_CASE("前缀: 步数将尽提醒只随消息尾部追加一次,system 全程不动") {
    CaptureBackend backend;
    backend.scripts = {
        ToolUseScript("call_1", "read_file"),
        ToolUseScript("call_2", "read_file"),
        TextScript("收尾"),
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FixedTool>("read_file", "正文"));

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt",
                          /*max_tokens=*/4096, /*max_steps_per_turn=*/5);
    agent::Callbacks callbacks;
    std::vector<api::UsageReport> reports;
    callbacks.on_usage = [&](const api::UsageReport& report) { reports.push_back(report); };

    REQUIRE(loop.Run("干活", callbacks).has_value());
    REQUIRE(backend.captured.size() == 3);

    // system 三份一字不差,提醒一个字都不进头段。
    CHECK(backend.captured[0].system == "system prompt");
    CHECK(backend.captured[1].system == "system prompt");
    CHECK(backend.captured[2].system == "system prompt");

    // 提醒在"剩余 3 步"那一步(step2)随尾部消息(刚攒完的 tool result)
    // 追加,只此一次;此前此后各步都没有。
    CHECK(backend.captured[0].messages.back().content.size() == 1);
    CHECK(backend.captured[1].messages.back().content.size() == 1);
    REQUIRE(backend.captured[2].messages.back().content.size() == 2);
    const auto* nudge = std::get_if<api::TextBlock>(&backend.captured[2].messages.back().content[1]);
    REQUIRE(nudge != nullptr);
    CHECK(nudge->text.find("收尾区") != std::string::npos);
    CHECK(nudge->text.find("检查点") != std::string::npos);

    // 三份请求两两原样追加——提醒落在"尚未发出的新消息"里,不追改旧前缀。
    CHECK(IsAppendOnlySuccessor(backend.captured[0], backend.captured[1]));
    CHECK(IsAppendOnlySuccessor(backend.captured[1], backend.captured[2]));

    // 台账:全程一个 epoch,没断。
    REQUIRE(reports.size() == 3);
    CHECK(reports[0].cache_epoch == 1);
    CHECK(reports[1].cache_epoch == 1);
    CHECK(reports[2].cache_epoch == 1);
    CHECK(reports[2].epoch_break_reason.empty());
    CHECK(reports[2].prefix_append_only);
}

// ---------------------------------------------------------------------------
// 断裂源三:tool_search 成批挂载——一次命中三只只占一个请求边界、只断一次
// (绿,现行行为即如此):断因 tools_changed,断后恢复追加律。
// 用谓词中途放行三只新工具等价复现挂载。
// ---------------------------------------------------------------------------

TEST_CASE("前缀: 工具表中途成批挂载只断一次,断因 tools_changed,随后恢复追加律") {
    CaptureBackend backend;
    backend.scripts = {
        ToolUseScript("call_1", "probe_tool"),
        ToolUseScript("call_2", "probe_tool"),
        TextScript("收尾"),
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FixedTool>("mcp__srv__one", "一"));
    registry.Register(std::make_unique<FixedTool>("mcp__srv__two", "二"));
    registry.Register(std::make_unique<FixedTool>("mcp__srv__three", "三"));

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    agent::Callbacks callbacks;
    std::vector<api::UsageReport> reports;
    callbacks.on_usage = [&](const api::UsageReport& report) { reports.push_back(report); };

    // 初始:延迟三只外挂工具不放行(tool_search 未挂载);probe_tool 执行时
    // 成批放行三只——等价于 tool_search 一次命中三只、只占一个请求边界。
    std::set<std::string> loaded;
    class MountingTool : public tools::Tool {
    public:
        MountingTool(std::set<std::string>& loaded) : loaded_(loaded) {}
        std::string name() const override { return "probe_tool"; }
        std::string description() const override { return "mounts deferred tools"; }
        nlohmann::json input_schema() const override { return nlohmann::json::object(); }
        tools::Tool::Result execute(const nlohmann::json&) override {
            loaded_.insert("mcp__srv__one");
            loaded_.insert("mcp__srv__two");
            loaded_.insert("mcp__srv__three");
            return {"挂载完成", false};
        }

    private:
        std::set<std::string>& loaded_;
    };
    registry.Register(std::make_unique<MountingTool>(loaded));
    loop.SetToolFilter([&loaded](const tools::Tool& tool) {
        const std::string prefix = "mcp__";
        return tool.name().rfind(prefix, 0) != 0 || loaded.count(tool.name()) != 0;
    });

    REQUIRE(loop.Run("干活", callbacks).has_value());
    REQUIRE(backend.captured.size() == 3);

    // P1 -> P2:tools 添了三只完整 schema,断一次,断因 tools_changed。
    const agent::PrefixDiff diff_12 = agent::DiffRequests(backend.captured[0], backend.captured[1]);
    CHECK_FALSE(diff_12.append_only());
    CHECK(diff_12.tools_changed);
    // P2 -> P3:工具表不再变,追加律恢复。
    CHECK(IsAppendOnlySuccessor(backend.captured[1], backend.captured[2]));

    REQUIRE(reports.size() == 3);
    CHECK(reports[1].cache_epoch == 2);
    CHECK(reports[1].epoch_break_reason == "tools_changed");
    CHECK(reports[2].cache_epoch == 2);  // 没再断,epoch 不动
    CHECK(reports[2].prefix_append_only);
}

// ---------------------------------------------------------------------------
// 断裂源四(已修):结构压缩改成"首次定形,epoch 内不追改"——长结果首次进
// 视图就是 artifact 预览,此后(含由热转冷)一个字节不变;重复与新版只
// 自述,绝不回头改早先事件。
// ---------------------------------------------------------------------------

TEST_CASE("前缀: 长工具结果首次即预览,由热转冷不再追改旧消息") {
    CaptureBackend backend;
    const std::string big(9000, 'x');  // 超过 long_result_bytes(8192)
    backend.scripts = {
        ToolUseScript("call_1", "read_file"),
        TextScript("小结"),
        TextScript("追问的回答"),
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FixedTool>("read_file", big));

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    agent::Callbacks callbacks;
    std::vector<api::UsageReport> reports;
    callbacks.on_usage = [&](const api::UsageReport& report) { reports.push_back(report); };

    REQUIRE(loop.Run("读大文件", callbacks).has_value());
    REQUIRE(loop.Run("追问", callbacks).has_value());
    REQUIRE(backend.captured.size() == 3);

    // 首次进视图(P2,工具结果刚攒完的那份请求)就是 artifact 预览——
    // 要么首次就预览,要么这个 epoch 一直全文,不能半路变脸。
    const auto* first_result =
        std::get_if<api::ToolResultBlock>(&backend.captured[1].messages[2].content[0]);
    REQUIRE(first_result != nullptr);
    CHECK(first_result->content.find("[artifact") == 0);
    CHECK(first_result->content.size() < big.size());

    // 轮内、跨轮(由热转冷)都逐字节追加——第六期前这里会追改旧消息。
    CHECK(IsAppendOnlySuccessor(backend.captured[0], backend.captured[1]));
    CHECK(IsAppendOnlySuccessor(backend.captured[1], backend.captured[2]));

    REQUIRE(reports.size() == 3);
    CHECK(reports[2].epoch_break_reason.empty());
    CHECK(reports[2].prefix_append_only);
}

// ---------------------------------------------------------------------------
// 断裂源五(已修):hard trim 第一次真动手裁之后,裁过的视图钉住(sticky
// view),后续只往尾部追加——裁剪窗口不再随新回合一路滑。
// ---------------------------------------------------------------------------

TEST_CASE("前缀: hard trim 后 sticky view 钉住,后续请求不再滑窗") {
    CaptureBackend backend;
    backend.scripts = {
        TextScript("答一"), TextScript("答二"), TextScript("答三"),
        TextScript("答四"), TextScript("答五"), TextScript("答六"),
        TextScript("答七"),
    };
    tools::ToolRegistry registry;
    // 7 轮每轮 ~1000 字符;上限 6000:第 6 轮起 TrimHistory 开始丢中间轮
    // (留第一轮 + 最近 3 轮,约 4000 字符,给后续追加留了余量——余量内
    // sticky 不再动手,余量耗尽再裁一次是新的明确 epoch break)。
    agent::AgentLoop loop(backend, registry, "test-model", "system prompt", /*max_tokens=*/4096,
                          /*max_steps_per_turn=*/0, /*max_context_chars=*/6000);
    agent::Callbacks callbacks;
    std::vector<api::UsageReport> reports;
    callbacks.on_usage = [&](const api::UsageReport& report) { reports.push_back(report); };

    for (int turn = 1; turn <= 7; ++turn) {
        const std::string prompt(1000, static_cast<char>('a' + turn));
        REQUIRE(loop.Run(prompt, callbacks).has_value());
    }
    REQUIRE(backend.captured.size() == 7);

    // 找到第一份真正动手裁的请求(第 6 轮):hard_trim 记账。
    std::size_t first_trimmed = 0;
    bool saw_trim_reason = false;
    for (std::size_t i = 0; i < reports.size(); ++i) {
        if (reports[i].epoch_break_reason == "hard_trim") {
            first_trimmed = i;
            saw_trim_reason = true;
            break;
        }
    }
    REQUIRE(saw_trim_reason);
    CHECK(first_trimmed == 5);  // 第 6 轮(0-based 下标 5)
    CHECK(reports[first_trimmed].cache_epoch == 2);

    // trim 前的追加律照旧(trim 那次除外)。
    for (std::size_t i = 1; i < first_trimmed; ++i) {
        CHECK(IsAppendOnlySuccessor(backend.captured[i - 1], backend.captured[i]));
    }

    // trim 之后:视图钉住,下一轮只是尾部追加——旧消息不再被追改、窗口
    // 不再滑。全量 JSONL 仍照旧保留,sticky 只是模型眼下那本账。
    REQUIRE(first_trimmed + 1 < backend.captured.size());
    CHECK(IsAppendOnlySuccessor(backend.captured[first_trimmed], backend.captured[first_trimmed + 1]));
    const agent::PrefixDiff diff =
        agent::DiffRequests(backend.captured[first_trimmed], backend.captured[first_trimmed + 1]);
    CHECK(diff.appended_messages == 2);
    CHECK(reports[first_trimmed + 1].epoch_break_reason.empty());
    CHECK(reports[first_trimmed + 1].prefix_append_only);
}
