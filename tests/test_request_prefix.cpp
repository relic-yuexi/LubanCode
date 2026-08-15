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

// ---------------------------------------------------------------------------
// 断裂源一(钉案):记忆 suffix 与任务名册每个外层回合重算,改的是 system
// 尾巴——分叉点落在全部旧历史之前,对话越长损失越大。
// 断因必须被点名 system_changed,不许无声。
// ---------------------------------------------------------------------------

TEST_CASE("前缀[钉案]: 轮间换 turn suffix 现行实现会改 system(第五期挪尾后翻绿)") {
    CaptureBackend backend;
    backend.scripts = {TextScript("答一"), TextScript("答二"), TextScript("答三")};
    tools::ToolRegistry registry;

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    agent::Callbacks callbacks;

    loop.SetTurnSystemSuffix("项目记忆召回(甲)");
    REQUIRE(loop.Run("第一句", callbacks).has_value());
    loop.SetTurnSystemSuffix("项目记忆召回(乙)");
    REQUIRE(loop.Run("第二句", callbacks).has_value());
    loop.SetTurnSystemSuffix("项目记忆召回(丙)");
    REQUIRE(loop.Run("第三句", callbacks).has_value());
    REQUIRE(backend.captured.size() == 3);

    // 同一 Run 内 suffix 固定,单轮内部不重算——P1->P2 只在轮间断。
    // 轮间断:system 尾巴换了,分叉点在旧历史之前。
    const agent::PrefixDiff diff_12 = agent::DiffRequests(backend.captured[0], backend.captured[1]);
    const agent::PrefixDiff diff_23 = agent::DiffRequests(backend.captured[1], backend.captured[2]);
    CHECK_FALSE(diff_12.append_only());
    CHECK(diff_12.system_changed);
    CHECK_FALSE(diff_23.append_only());
    CHECK(diff_23.system_changed);
}

// ---------------------------------------------------------------------------
// 断裂源二(钉案):步数将尽 nudge 现在改 system。max_steps=5 时 nudge 落在
// step2(剩余 3):P3 的 system 忽然多出一段,P2/P3 之间断。
// (第五期改成"固定提醒只随消息尾部追加一次"后翻绿。)
// ---------------------------------------------------------------------------

TEST_CASE("前缀[钉案]: 步数将尽 nudge 现行实现中途改 system(第五期挪尾后翻绿)") {
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

    // nudge 只在"剩余步数第一次降到 3"那一步(step2)注入,且只注一次——
    // 这两点现行实现已经守住。
    CHECK(backend.captured[0].system.find("步数预算将尽") == std::string::npos);
    CHECK(backend.captured[1].system.find("步数预算将尽") == std::string::npos);
    CHECK(backend.captured[2].system.find("步数预算将尽") != std::string::npos);

    // 但它落在 system 里:P2 -> P3 断前缀,断因 system_changed。
    CHECK(IsAppendOnlySuccessor(backend.captured[0], backend.captured[1]));
    const agent::PrefixDiff diff = agent::DiffRequests(backend.captured[1], backend.captured[2]);
    CHECK_FALSE(diff.append_only());
    CHECK(diff.system_changed);

    // 台账照样点名:第三步记 system_changed 的 epoch 断。
    REQUIRE(reports.size() == 3);
    CHECK(reports[2].cache_epoch == 2);
    CHECK(reports[2].epoch_break_reason == "system_changed");
    CHECK_FALSE(reports[2].prefix_append_only);
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
// 断裂源四(钉案):结构压缩的"由热转冷"追改旧 tool result——上一轮热区发
// 全文,下一条外层用户消息一到,同一枚结果忽然换成 artifact 预览。
// (第六期"首次定形,epoch 内不追改"落地后翻绿。)
// ---------------------------------------------------------------------------

TEST_CASE("前缀[钉案]: 长工具结果由热转冷后现行实现会追改旧消息(第六期首次定形后翻绿)") {
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

    // 轮内:工具结果刚落地,在热区,两份请求逐字节追加。
    CHECK(IsAppendOnlySuccessor(backend.captured[0], backend.captured[1]));

    // 跨轮:上一轮跌进冷区,同一枚 read_file 结果被换成 artifact 预览——
    // 旧消息被追改,断因 old_message_changed,位置指到那条消息。
    const agent::PrefixDiff diff = agent::DiffRequests(backend.captured[1], backend.captured[2]);
    CHECK_FALSE(diff.append_only());
    CHECK(diff.break_reason() == "old_message_changed");
    // 台账点名:第三份请求(轮 2 的第一份,step 0)记断。
    REQUIRE(reports.size() == 3);
    CHECK(reports[2].epoch_break_reason == "old_message_changed");
}

// ---------------------------------------------------------------------------
// 断裂源五(钉案):hard trim 的裁剪窗口随新回合往后滑,每份请求都可能换
// 一副裁剪形状。(第六期 sticky view 落地后翻绿。)
// ---------------------------------------------------------------------------

TEST_CASE("前缀[钉案]: hard trim 裁剪窗口现行实现逐请求滑动(第六期 sticky view 后翻绿)") {
    CaptureBackend backend;
    backend.scripts = {
        TextScript("答一"), TextScript("答二"), TextScript("答三"),
        TextScript("答四"), TextScript("答五"), TextScript("答六"),
        TextScript("答七"),
    };
    tools::ToolRegistry registry;
    // max_context_chars 压小:7 轮每轮 ~1000 字符,第 5 轮起 TrimHistory
    // 开始丢中间轮(留第一轮 + 最近 3 轮)。
    agent::AgentLoop loop(backend, registry, "test-model", "system prompt", /*max_tokens=*/4096,
                          /*max_steps_per_turn=*/0, /*max_context_chars=*/4200);
    agent::Callbacks callbacks;
    std::vector<api::UsageReport> reports;
    callbacks.on_usage = [&](const api::UsageReport& report) { reports.push_back(report); };

    for (int turn = 1; turn <= 7; ++turn) {
        const std::string prompt(1000, static_cast<char>('a' + turn));
        REQUIRE(loop.Run(prompt, callbacks).has_value());
    }
    REQUIRE(backend.captured.size() == 7);

    // 找到第一份真正动手裁的请求:从它起,再发一份(下一轮)看窗口滑不滑。
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
    // 钉案:trim 之后下一份请求的视图又换了形状(窗口滑动),旧消息又被追改。
    REQUIRE(first_trimmed + 1 < backend.captured.size());
    const agent::PrefixDiff diff =
        agent::DiffRequests(backend.captured[first_trimmed], backend.captured[first_trimmed + 1]);
    CHECK_FALSE(diff.append_only());
    CHECK(diff.old_message_changed);
}
