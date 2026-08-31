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

#include "agent/agent.hpp"
#include "turn_event_recorder.hpp"
#include "agent/loop.hpp"
#include "agent/prefix.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "tools/tool_search.hpp"

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

TEST_CASE("StablePrefixOf: 稳定前缀的条数与合成指纹(问题 9 诊断账)") {
    api::Request base;
    base.model = "m";
    base.system = "s";
    base.messages.push_back(UserText("一"));
    base.messages.push_back(UserText("二"));
    base.messages.push_back(UserText("三"));
    const auto fb = agent::FingerprintRequest(base);

    // 完全相同的下一份:稳定前缀 = 全部 3 条,指纹非空。
    const auto same = agent::StablePrefixOf(fb, agent::FingerprintRequest(base));
    CHECK(same.messages == 3);
    CHECK(same.hash.size() == 16);

    // 原样追加一条:稳定前缀还是 3 条,指纹与"完全相同"那份一致(同一段
    // 前缀折出同一枚 hash,诊断账才能跨请求对上号)。
    api::Request appended = base;
    appended.messages.push_back(UserText("四"));
    const auto appended_view = agent::StablePrefixOf(fb, agent::FingerprintRequest(appended));
    CHECK(appended_view.messages == 3);
    CHECK(appended_view.hash == same.hash);

    // 中途分岔:稳定前缀缩到分岔处(第 2 条起不同 -> 稳定 1 条)。
    api::Request diverged = base;
    diverged.messages[1] = UserText("改");
    const auto diverged_view = agent::StablePrefixOf(fb, agent::FingerprintRequest(diverged));
    CHECK(diverged_view.messages == 1);
    CHECK(diverged_view.hash != same.hash);

    // 一条都不共享:0 条,指纹空(无稳定前缀,显示层另按"首请求/无共享"
    // 措辞,不拿空 hash 编故事)。
    api::Request alien;
    alien.model = "m";
    alien.system = "s";
    alien.messages.push_back(UserText("全新"));
    const auto alien_view = agent::StablePrefixOf(fb, agent::FingerprintRequest(alien));
    CHECK(alien_view.messages == 0);
    CHECK(alien_view.hash.empty());
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

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    agent::TurnWiring callbacks;
    lubancode::test::RecordedTurn turn;
    callbacks.events = &turn.adapter;
    // usage 观察改吃事件流(批二余款):UsageReport 从 UsageUpdated 的
    // 载荷里还原,身份(cache_epoch/追加律)齐。
    const std::vector<api::UsageReport>& reports = turn.recorder.usage_reports;

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

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    agent::TurnWiring callbacks;
    lubancode::test::RecordedTurn turn;
    callbacks.events = &turn.adapter;
    // usage 观察改吃事件流(批二余款):UsageReport 从 UsageUpdated 的
    // 载荷里还原,身份(cache_epoch/追加律)齐。
    const std::vector<api::UsageReport>& reports = turn.recorder.usage_reports;

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

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    agent::TurnWiring callbacks;

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

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .runtime{.max_output_tokens = 4096, .max_steps_per_turn = 5}, .system_prompt = "system prompt"});
    agent::TurnWiring callbacks;
    lubancode::test::RecordedTurn turn;
    callbacks.events = &turn.adapter;
    // usage 观察改吃事件流(批二余款):UsageReport 从 UsageUpdated 的
    // 载荷里还原,身份(cache_epoch/追加律)齐。
    const std::vector<api::UsageReport>& reports = turn.recorder.usage_reports;

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

    // 初始:延迟三只外挂工具不放行(tool_search 未挂载);probe_tool 执行时
    // 成批放行三只——等价于 tool_search 一次命中三只、只占一个请求边界。
    std::set<std::string> loaded;
    agent::AgentProfile profile{.request{.model = "test-model"}, .system_prompt = "system prompt"};
    profile.tool_filter = [&loaded](const tools::Tool& tool) {
        const std::string prefix = "mcp__";
        return tool.name().rfind(prefix, 0) != 0 || loaded.count(tool.name()) != 0;
    };
    agent::Agent loop(backend, registry, std::move(profile));
    agent::TurnWiring callbacks;
    lubancode::test::RecordedTurn turn;
    callbacks.events = &turn.adapter;
    // usage 观察改吃事件流(批二余款):UsageReport 从 UsageUpdated 的
    // 载荷里还原,身份(cache_epoch/追加律)齐。
    const std::vector<api::UsageReport>& reports = turn.recorder.usage_reports;
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

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    agent::TurnWiring callbacks;
    lubancode::test::RecordedTurn turn;
    callbacks.events = &turn.adapter;
    // usage 观察改吃事件流(批二余款):UsageReport 从 UsageUpdated 的
    // 载荷里还原,身份(cache_epoch/追加律)齐。
    const std::vector<api::UsageReport>& reports = turn.recorder.usage_reports;

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
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .runtime{.max_output_tokens = 4096, .max_steps_per_turn = 0, .max_context_chars = 6000}, .system_prompt = "system prompt"});
    agent::TurnWiring callbacks;
    lubancode::test::RecordedTurn turn;
    callbacks.events = &turn.adapter;
    // usage 观察改吃事件流(批二余款):UsageReport 从 UsageUpdated 的
    // 载荷里还原,身份(cache_epoch/追加律)齐。
    const std::vector<api::UsageReport>& reports = turn.recorder.usage_reports;

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

// ---------------------------------------------------------------------------
// 动态工具 PromptCache 守恒单 P0(把问题钉死,不改产品行为):
//
// 断裂源三的用例拿合成探针(MountingTool 直接改 loaded 集合、system 全程
// 一字不变)只证明了"tools 会断"。生产真实接线还多一根梁——
// AgentProfile.deferred_index_provider 随 loaded 集合现查现拼延迟索引段
// (interactive_session_assembly.cpp / one_shot.cpp 同款接线)。真实
// tool_search 命中会同时改两处旧前缀:tools 数组多一枚完整 schema,system
// 尾部的索引段少一行。这里用生产同款接线 + 真 ToolSearchTool /
// BuildDeferredToolsIndexSegment(不是合成探针)复现规格 §一/§2.1/§2.2 的
// 断语:legacy 路径的这一断,是 system_changed 与 tools_changed 同一拍
// 一起断,不是分两拍各断一次。
// ---------------------------------------------------------------------------

TEST_CASE("P0现状证据: legacy tool_search 真实机制命中后,system_changed 与 tools_changed 同拍一起断") {
    CaptureBackend backend;
    backend.scripts = {
        // P1 -> 模型先搜:tool_search("github issue search")。
        {
            api::MessageStart{"msg", "test-model"},
            api::ToolUseStart{0, "call_search", "tool_search"},
            api::ToolUseInputDelta{0, R"({"query":"github issue search"})"},
            api::ContentBlockDone{0},
            api::MessageDone{"tool_use", api::Usage{}},
        },
        // P2 -> 搜索命中已挂载,模型直接调用目标工具。
        {
            api::MessageStart{"msg", "test-model"},
            api::ToolUseStart{0, "call_target", "mcp__github__search_issues"},
            api::ToolUseInputDelta{0, R"({"query":"repo:lubancode cache"})"},
            api::ContentBlockDone{0},
            api::MessageDone{"tool_use", api::Usage{}},
        },
        // P3 -> 收尾。
        TextScript("收到"),
    };

    tools::ToolRegistry registry;
    auto loaded = std::make_shared<std::set<std::string>>();
    registry.Register(std::make_unique<tools::DeferredTool>(
        std::make_unique<FixedTool>("mcp__github__search_issues", "命中结果:issue #42")));
    registry.Register(std::make_unique<tools::ToolSearchTool>(registry, loaded));

    // 生产同款接线(interactive_session_assembly.cpp:979-999 同一套):
    // deferred_index_provider 与 tool_filter 共享同一份 loaded 集合。
    agent::AgentProfile profile{.request{.model = "test-model"}, .system_prompt = "system prompt"};
    profile.deferred_index_provider = [&registry, loaded]() {
        return tools::BuildDeferredToolsIndexSegment(registry, *loaded);
    };
    profile.tool_filter = [loaded](const tools::Tool& tool) {
        return !tool.deferred() || loaded->count(tool.name()) != 0;
    };

    agent::Agent loop(backend, registry, std::move(profile));
    agent::TurnWiring callbacks;

    REQUIRE(loop.Run("帮我搜一下 github issue", callbacks).has_value());
    REQUIRE(backend.captured.size() == 3);

    // P1(搜索前):索引段带着待检索工具那一行;tools 数组里没有它的完整
    // schema。
    CHECK(backend.captured[0].system.find("mcp__github__search_issues") != std::string::npos);
    auto has_tool = [](const api::Request& req, const std::string& name) {
        for (const auto& t : req.tools) {
            if (t.name == name) return true;
        }
        return false;
    };
    CHECK_FALSE(has_tool(backend.captured[0], "mcp__github__search_issues"));

    // P2(命中之后发出的那份请求):索引段那一行消失(system_changed),
    // tools 数组多了完整 schema(tools_changed)——两根梁同一拍一起断。
    CHECK(backend.captured[1].system.find("mcp__github__search_issues") == std::string::npos);
    CHECK(has_tool(backend.captured[1], "mcp__github__search_issues"));

    const agent::PrefixDiff diff_12 = agent::DiffRequests(backend.captured[0], backend.captured[1]);
    CHECK(diff_12.system_changed);
    CHECK(diff_12.tools_changed);
    CHECK_FALSE(diff_12.append_only());
    // 断因点名口径(agent/prefix.hpp 的 break_reason 排序 model > system >
    // tools):两根梁同断时只报第一根,tools_changed 那半被"吃掉"、不出现
    // 在单一字符串断因里——PrefixDiff 结构体本身两个布尔都真,但下游只取
    // break_reason() 字符串(如 UsageReport.epoch_break_reason)的账本会
    // 漏记 tools 那一半。P1 记账/展示补强时要留意这处口径缺口。
    CHECK(diff_12.break_reason() == "system_changed");

    // loaded 集合此后不再变,追加律恢复——生产合同里"只断一次"成立,但
    // 那一次断的是复合断(system + tools),不是账面上的孤立小抖动。
    CHECK(IsAppendOnlySuccessor(backend.captured[1], backend.captured[2]));
}

// ---------------------------------------------------------------------------
// 动态工具 PromptCache 守恒单 P0:现状证据二——discovery 前后的 cached
// tokens 账。真机没有钥匙,这里用确定性假后端复现"长历史攒着高命中率,
// tool_search 命中那一拍 cache_read 归零、cache_creation 整段重付"的
// 力学关系;Usage 数字是本用例手写喂给 CaptureBackend 的确定性脚本
// (不是真机实测),但账目结构(哪一步归零、哪一步重新爬升)如实反映
// §一断语"最值钱的长历史落在差异之后"。真机数字见 P0 报告另一栏。
// ---------------------------------------------------------------------------

TEST_CASE("P0现状证据: 假后端确定性账——discovery 前长历史高命中,discovery 那拍 cache 归零重付") {
    CaptureBackend backend;
    // input_tokens 恒 0:这批确定性脚本里,每步的完整输入全由"读缓存"与
    // "新写缓存"两笔账付清(api::TotalInputTokens 的口径:input_tokens 是
    // 二者之外的第三笔"既不命中也不新写"的量,这里不构造那种情形,免得
    // 跟 cache_read/cache_creation 重复计成两份)。
    auto usage_step = [](std::int64_t cache_read, std::int64_t cache_creation) {
        api::Usage u;
        u.input_tokens = 0;
        u.output_tokens = 40;
        u.cache_read_tokens = cache_read;
        u.cache_creation_tokens = cache_creation;
        return u;
    };
    auto text_with_usage = [](const std::string& text, api::Usage usage) {
        return std::vector<api::StreamEvent>{
            api::MessageStart{"msg", "test-model"},
            api::TextDelta{text},
            api::ContentBlockDone{0},
            api::MessageDone{"end_turn", usage},
        };
    };
    auto tool_use_with_usage = [](const std::string& id, const std::string& name, const std::string& input_json,
                                  api::Usage usage) {
        return std::vector<api::StreamEvent>{
            api::MessageStart{"msg", "test-model"},
            api::ToolUseStart{0, id, name},
            api::ToolUseInputDelta{0, input_json},
            api::ContentBlockDone{0},
            api::MessageDone{"tool_use", usage},
        };
    };

    backend.scripts = {
        // 轮 1、2:长历史攒起来,前缀命中率越垒越高(典型长会话力学)。
        text_with_usage("答一", usage_step(0, 5000)),      // epoch 1 首份请求:全款重付
        text_with_usage("答二", usage_step(5000, 1200)),   // 上一份系统+历史整段命中,只新付本轮
        // 轮 3:模型先搜,这一步仍在旧 epoch 上,继续吃历史命中。
        tool_use_with_usage("call_search", "tool_search", R"({"query":"github issue search"})",
                            usage_step(6200, 1500)),
        // 命中后紧跟这一步:system 少一行、tools 多一枚 schema——旧前缀
        // 在服务端也对不上号,cache_read 归零,cache_creation 重付整段。
        tool_use_with_usage("call_target", "mcp__github__search_issues", R"({"query":"repo:lubancode cache"})",
                            usage_step(0, 9200)),
        // 新 epoch 站稳:后续步骤重新爬命中率。
        text_with_usage("收到", usage_step(9200, 600)),
        // 轮 4(下一个外层 user 轮):新 epoch 延续,不再归零。
        text_with_usage("答四", usage_step(9800, 700)),
    };

    tools::ToolRegistry registry;
    auto loaded = std::make_shared<std::set<std::string>>();
    registry.Register(std::make_unique<tools::DeferredTool>(
        std::make_unique<FixedTool>("mcp__github__search_issues", "命中结果:issue #42")));
    registry.Register(std::make_unique<tools::ToolSearchTool>(registry, loaded));

    agent::AgentProfile profile{.request{.model = "test-model"}, .system_prompt = "system prompt"};
    profile.deferred_index_provider = [&registry, loaded]() {
        return tools::BuildDeferredToolsIndexSegment(registry, *loaded);
    };
    profile.tool_filter = [loaded](const tools::Tool& tool) {
        return !tool.deferred() || loaded->count(tool.name()) != 0;
    };

    agent::Agent loop(backend, registry, std::move(profile));
    agent::TurnWiring callbacks;
    lubancode::test::RecordedTurn turn;
    callbacks.events = &turn.adapter;
    const std::vector<api::UsageReport>& reports = turn.recorder.usage_reports;

    REQUIRE(loop.Run("先聊两句", callbacks).has_value());   // 轮 1
    REQUIRE(loop.Run("再聊一句", callbacks).has_value());   // 轮 2
    REQUIRE(loop.Run("帮我搜一下 github issue", callbacks).has_value());  // 轮 3(内含 search+invoke+收尾三步)
    REQUIRE(loop.Run("还有别的吗", callbacks).has_value());  // 轮 4
    REQUIRE(backend.captured.size() == 6);
    REQUIRE(reports.size() == 6);

    // discovery 前(下标 0..2):同一 epoch,cache_read 逐步爬升,命中率
    // 越垒越高——长历史最值钱的那部分正在这里。
    CHECK(reports[0].cache_epoch == 1);
    CHECK(reports[1].cache_epoch == 1);
    CHECK(reports[2].cache_epoch == 1);
    CHECK(reports[0].usage.cache_read_tokens == 0);
    CHECK(reports[1].usage.cache_read_tokens == 5000);
    CHECK(reports[2].usage.cache_read_tokens == 6200);

    // discovery 命中后紧跟那一步(下标 3):epoch 断到 2,cache_read 归零,
    // 全段按 cache_creation 重付——正是规格断语"最值钱的长历史落在差异
    // 之后"的账面证据。
    CHECK(reports[3].cache_epoch == 2);
    CHECK_FALSE(reports[3].prefix_append_only);
    CHECK(reports[3].usage.cache_read_tokens == 0);
    CHECK(reports[3].usage.cache_creation_tokens == 9200);

    // 新 epoch 站稳后(下标 4、5):命中率重新爬升,但爬升是从零开始的
    // 第二次——旧 epoch 攒的那笔命中率账白付了一次重建成本。
    CHECK(reports[4].cache_epoch == 2);
    CHECK(reports[4].usage.cache_read_tokens == 9200);
    CHECK(reports[5].cache_epoch == 2);
    CHECK(reports[5].usage.cache_read_tokens == 9800);

    // 完整口径对照(§十一·11.3 不能只看 cached ratio):discovery 前最后
    // 一步的命中率,与 discovery 那一步的命中率,两者都要摆出来,不能只
    // 挑好看的那一个。
    const auto hit_percent = [](const api::UsageReport& r) -> double {
        const std::int64_t total = api::TotalInputTokens(r.usage);
        if (total <= 0) return 0.0;
        return 100.0 * static_cast<double>(r.usage.cache_read_tokens) / static_cast<double>(total);
    };
    CHECK(hit_percent(reports[2]) > 80.0);   // discovery 前:6200/7700 ≈ 80.5%
    CHECK(hit_percent(reports[3]) == 0.0);   // discovery 那一拍:硬归零
}

// ---------------------------------------------------------------------------
// 动态工具 PromptCache 守恒单 P1:客户端三拍前缀合同(§11.1)。proxy_
// reference 必须满足 R1/R2/R3 的 model/system/tools 指纹全等、messages 只
// 追加、cache epoch 不因发现或调用而断——只要其中一条不成,P1 不算落地。
// ---------------------------------------------------------------------------

TEST_CASE("P1三拍前缀合同: proxy_reference 发现与调用前后 system/tools 指纹不变,messages 只追加") {
    CaptureBackend backend;
    // 计数延迟目标:发现与调用各拍是否真的动了目标工具,一眼可查。
    class CountingTarget : public tools::Tool {
    public:
        std::string name() const override { return "mcp__github__search_issues"; }
        std::string description() const override { return "Search issues in a GitHub repository."; }
        nlohmann::json input_schema() const override {
            return nlohmann::json{{"type", "object"},
                                  {"properties", {{"query", {{"type", "string"}}}}},
                                  {"required", nlohmann::json::array({"query"})}};
        }
        bool deferred() const override { return true; }
        tools::Tool::Result execute(const nlohmann::json&) override {
            ++calls;
            return {"命中结果:issue #42", false};
        }
        int calls = 0;
    };
    auto target = std::make_unique<CountingTarget>();
    CountingTarget* target_ptr = target.get();

    tools::ToolRegistry registry;
    registry.Register(std::move(target));
    auto resolver = std::make_shared<tools::DeferredToolResolver>("main");
    registry.Register(std::make_unique<tools::ToolSearchTool>(registry, resolver));
    registry.Register(std::make_unique<tools::ToolInvokeTool>());

    // 生产同款 proxy 接线(interactive_session_assembly 的 P1 分支同一套):
    // resolver + 执行资格 + exposure 过滤(延迟工具恒不进顶层);没有
    // deferred_index_provider——proxy 路的 system 恒定(§8.1)。
    agent::AgentProfile profile{.request{.model = "test-model"}, .system_prompt = "system prompt"};
    profile.tool_ref_resolver = resolver;
    profile.tool_filter = [](const tools::Tool& tool) { return !tool.deferred(); };
    profile.tool_filter_denial = "延迟工具不能按名直调:请先用 tool_search,再以 tool_invoke 调用。";
    profile.tool_execution_policy = [](const tools::Tool&) { return true; };

    agent::Agent loop(backend, registry, std::move(profile));
    agent::TurnWiring callbacks;

    // R1(发现前)-> R2(追加 tool_search call/result)。
    backend.scripts = {
        {
            api::MessageStart{"msg", "test-model"},
            api::ToolUseStart{0, "call_search", "tool_search"},
            api::ToolUseInputDelta{0, R"({"query":"github issue search"})"},
            api::ContentBlockDone{0},
            api::MessageDone{"tool_use", api::Usage{}},
        },
        TextScript("搜到了"),
    };
    REQUIRE(loop.Run("帮我搜一下 github issue", callbacks).has_value());
    REQUIRE(backend.captured.size() == 2);
    REQUIRE(target_ptr->calls == 0);

    // 取出 tool_search 结果里的 tool_ref(宿主运行时铸的号,测前不可知)。
    std::string tool_ref;
    for (const auto& message : loop.history()) {
        for (const auto& block : message.content) {
            if (const auto* result = std::get_if<api::ToolResultBlock>(&block); result != nullptr) {
                const nlohmann::json parsed =
                    nlohmann::json::parse(result->content, nullptr, /*allow_exceptions=*/false);
                if (parsed.is_object() && parsed.contains("matches") && !parsed["matches"].empty() &&
                    parsed["matches"][0].contains("tool_ref")) {
                    tool_ref = parsed["matches"][0]["tool_ref"].get<std::string>();
                }
            }
        }
    }
    REQUIRE_FALSE(tool_ref.empty());

    // R2(调用前)-> R3(追加 tool_invoke call/result)-> 收尾。CaptureBackend
    // 的脚本按下标全局累进,第二轮只能追加、不能整份换(换了就下标越界)。
    backend.scripts.push_back({
        api::MessageStart{"msg", "test-model"},
        api::ToolUseStart{0, "call_invoke", "tool_invoke"},
        api::ToolUseInputDelta{0,
                               R"({"tool_ref":")" + tool_ref + R"(","arguments":{"query":"repo:lubancode cache"}})"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    });
    backend.scripts.push_back(TextScript("完成"));
    REQUIRE(loop.Run("调用它", callbacks).has_value());
    REQUIRE(backend.captured.size() == 4);
    REQUIRE(target_ptr->calls == 1);  // 调用真实发生(发现那拍没有)

    // ---- 合同逐条验(§11.1)------------------------------------------------
    auto hash_of = [](const api::Request& req) {
        const agent::PrefixFingerprint fp = agent::FingerprintRequest(req);
        return std::pair<std::string, std::string>{fp.system_hash, fp.tools_hash};
    };
    const auto [system_1, tools_1] = hash_of(backend.captured[0]);
    for (std::size_t i = 1; i < backend.captured.size(); ++i) {
        const auto [system_i, tools_i] = hash_of(backend.captured[i]);
        CHECK(system_i == system_1);
        CHECK(tools_i == tools_1);
        CHECK(backend.captured[i - 1].model == backend.captured[i].model);
    }
    // messages 逐份严格前缀追加(R1⊂R2⊂R3⊂收尾)。
    for (std::size_t i = 1; i < backend.captured.size(); ++i) {
        CHECK(agent::IsAppendOnlySuccessor(backend.captured[i - 1], backend.captured[i]));
    }
    // 顶层 tools 恒为 core + tool_search + tool_invoke;目标工具从不进
    // 任何一份请求的 tools,system 里也没有延迟索引段。
    for (const auto& request : backend.captured) {
        bool has_search = false;
        bool has_invoke = false;
        for (const auto& def : request.tools) {
            has_search = has_search || def.name == "tool_search";
            has_invoke = has_invoke || def.name == "tool_invoke";
            CHECK_MESSAGE(def.name != "mcp__github__search_issues", "延迟目标进了顶层 tools");
        }
        CHECK(has_search);
        CHECK(has_invoke);
        CHECK_MESSAGE(request.system.find("mcp__github__search_issues") == std::string::npos,
                      "proxy 路的 system 里不该出现延迟索引段");
    }
}
