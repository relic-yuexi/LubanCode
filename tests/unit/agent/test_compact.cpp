// agent/compact.hpp:BuildCompactedHistory 纯逻辑(archive + 按 token 预算
// 保留的热区,tool_use/tool_result 永远配对)、Compact() 用 FakeBackend 按脚
// 本吐事件(不碰真网络),验证请求怎么拼、失败时怎么处理。0.31.x 分层压缩
// 第一期加钉:压缩模型窗口预算(装不下明确拒绝,不发请求、不截史)、摘要
// 末尾可解析 JSON manifest、待办守恒校验(漏一项拒收,旧历史不动)、热区
// 按 token 预算保留多轮。

#include <doctest/doctest.h>

#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "agent/compact.hpp"
#include "agent/context.hpp"
#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/registry.hpp"

using namespace lubancode;

namespace {

api::Message UserText(const std::string& text) {
    api::Message m;
    m.role = api::Role::User;
    m.content.push_back(api::TextBlock{text});
    return m;
}

api::Message AssistantText(const std::string& text) {
    api::Message m;
    m.role = api::Role::Assistant;
    m.content.push_back(api::TextBlock{text});
    return m;
}

api::Message AssistantToolUse(const std::string& id, const std::string& name) {
    api::Message m;
    m.role = api::Role::Assistant;
    m.content.push_back(api::ToolUseBlock{id, name, nlohmann::json::object()});
    return m;
}

api::Message UserToolResult(const std::string& tool_use_id, const std::string& content) {
    api::Message m;
    m.role = api::Role::User;
    m.content.push_back(api::ToolResultBlock{tool_use_id, content, false});
    return m;
}

// 按脚本吐事件的假后端:记下收到的 Request,方便断言 system 里带没带
// focus 文本、messages 是不是整份历史、窗口拒绝时是不是压根没发。
class FakeBackend : public api::Backend {
public:
    std::vector<api::StreamEvent> script;
    bool fail = false;
    std::string fail_message;
    std::vector<api::Request> captured_requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
        captured_requests.push_back(request);
        if (fail) {
            return std::unexpected(api::Error{api::ErrorKind::Network, fail_message, 0});
        }
        for (const auto& event : script) {
            on_event(event);
        }
        return {};
    }
};

std::vector<api::StreamEvent> SummaryScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

// 成功摘要 = 够长的六栏正文 + 末尾 ```json manifest。默认 manifest 覆盖
// 两枚"必须守恒"的待办;单独的守恒用例再换特制 manifest。
std::string LongSummaryWithManifest(const std::string& manifest_json,
                                    const std::string& open_items_json = R"(["补齐单元测试","写文档说明"])") {
    return "## 任务目标\n实现上下文管理与压缩,关键文件是 config.cpp\n"
           "## 已证实的事实\n六栏存档与 manifest 由模型一并产出\n"
           "## 关键决策\n按栏目输出存档\n## 涉及文件与符号\nconfig.cpp\n"
           "## 关键命令与结果\n(无)\n## 未完成事项\n补齐单元测试;写文档说明\n"
           "```json\n" +
           (manifest_json.empty()
                ? "{\"goal\": \"实现上下文管理与压缩\", \"constraints\": [\"不许猜补\"], \"open_items\": " +
                      open_items_json + ", \"next_action\": \"补测试\"}"
                : manifest_json) +
           "\n```";
}

const std::string kGoodSummary = LongSummaryWithManifest("");

agent::CompactOptions PlainOptions() {
    agent::CompactOptions options;  // 窗口未知:不拦截(老行为的兼容档)
    return options;
}

}  // namespace

// ---------------------------------------------------------------------------
// BuildCompactedHistory:热区按 token 预算保留
// ---------------------------------------------------------------------------

TEST_CASE("BuildCompactedHistory: 新历史 = archive 并入热区首条,热区整段保留") {
    std::vector<api::Message> history;
    history.push_back(UserText("第一句话,提到一个名字:张三"));
    history.push_back(AssistantText("好的,记住了"));
    history.push_back(UserText("最近一次提问"));
    history.push_back(AssistantToolUse("tool_1", "read_file"));
    history.push_back(UserToolResult("tool_1", "文件内容"));
    history.push_back(AssistantText("读完了"));

    api::Message archive = UserText("[对话存档,此前内容已压缩] 摘要正文");
    // 默认 12k 预算:两轮都小,全进热区。
    const auto new_history = agent::BuildCompactedHistory(history, archive);

    // 存档正文并入热区第一条 user 消息的开头——不单独成一条消息,不然
    // 存档 user 紧跟热区的 user 输入,相邻两条 user 违反角色交替。
    REQUIRE(new_history.size() == 6);
    CHECK(new_history[0].role == api::Role::User);
    REQUIRE(std::holds_alternative<api::TextBlock>(new_history[0].content[0]));
    const std::string& first_text = std::get<api::TextBlock>(new_history[0].content[0]).text;
    CHECK(first_text.find("对话存档") != std::string::npos);
    CHECK(first_text.find("摘要正文") != std::string::npos);
    CHECK(first_text.find("第一句话") != std::string::npos);
    CHECK(first_text.find("对话存档") < first_text.find("第一句话"));
    // 热区里的最后一轮原样在后面。
    CHECK(std::get<api::TextBlock>(new_history[2].content[0]).text == "最近一次提问");

    // 合并后不出现相邻两条 user 消息。
    for (std::size_t i = 0; i + 1 < new_history.size(); ++i) {
        const bool both_user =
            new_history[i].role == api::Role::User && new_history[i + 1].role == api::Role::User;
        CHECK_FALSE(both_user);
    }
}

TEST_CASE("BuildCompactedHistory: 热区按 token 预算往前多收几轮,老的不进热区") {
    std::vector<api::Message> history;
    history.push_back(UserText("很老的一轮,不该进热区:老话"));
    history.push_back(AssistantText("老回答"));
    history.push_back(UserText("中间一轮,预算够就该留:中话"));
    history.push_back(AssistantText("中回答"));
    history.push_back(UserText("最近一轮:新话"));
    history.push_back(AssistantText("新回答"));

    api::Message archive = UserText("[对话存档,此前内容已压缩] 摘要");
    // 预算给足:三轮全留(每轮就十几个 token,1000 绰绰有余)。
    auto wide = agent::BuildCompactedHistory(history, archive, /*hot_zone_tokens=*/1000);
    REQUIRE(wide.size() == 6);  // 三轮 6 条,第一条并入存档后仍是一条消息
    CHECK(std::get<api::TextBlock>(wide[0].content[0]).text.find("老话") != std::string::npos);
    CHECK(std::get<api::TextBlock>(wide[2].content[0]).text.find("中话") != std::string::npos);

    // 预算掐小:末轮超预算时不再整轮全保——轮头(最新用户输入)必保并入
    // 存档,后面的 assistant 消息装不下就不进热区(P1-1 反涨的修法)。
    auto narrow = agent::BuildCompactedHistory(history, archive, /*hot_zone_tokens=*/1);
    REQUIRE(narrow.size() == 1);  // 只剩轮头 user 消息一条,已并入存档
    const std::string& narrow_text = std::get<api::TextBlock>(narrow[0].content[0]).text;
    CHECK(narrow_text.find("新话") != std::string::npos);
    CHECK(narrow_text.find("新回答") == std::string::npos);
    CHECK(narrow_text.find("中话") == std::string::npos);
}

TEST_CASE("BuildCompactedHistory: tool_use 和 tool_result 永远配对,不会被切开") {
    std::vector<api::Message> history;
    history.push_back(UserText("问题一"));
    history.push_back(AssistantText("回答一"));
    history.push_back(UserText("问题二,需要用工具"));
    history.push_back(AssistantToolUse("tool_9", "search"));
    history.push_back(UserToolResult("tool_9", "搜索结果"));
    history.push_back(AssistantText("回答二"));

    api::Message archive = UserText("[对话存档,此前内容已压缩] 摘要");
    const auto new_history = agent::BuildCompactedHistory(history, archive);

    for (std::size_t i = 0; i < new_history.size(); ++i) {
        for (const auto& block : new_history[i].content) {
            if (std::holds_alternative<api::ToolUseBlock>(block)) {
                const auto& tool_use = std::get<api::ToolUseBlock>(block);
                REQUIRE(i + 1 < new_history.size());
                bool found_pair = false;
                for (const auto& next_block : new_history[i + 1].content) {
                    if (std::holds_alternative<api::ToolResultBlock>(next_block) &&
                        std::get<api::ToolResultBlock>(next_block).tool_use_id == tool_use.id) {
                        found_pair = true;
                    }
                }
                CHECK(found_pair);
            }
        }
    }
}

TEST_CASE("BuildCompactedHistory: 历史里找不到真正的用户文本输入时,只留 archive") {
    std::vector<api::Message> history;
    history.push_back(UserToolResult("tool_x", "孤立的工具结果"));  // 角色是 user,但不含 TextBlock

    api::Message archive = UserText("[对话存档,此前内容已压缩] 摘要");
    const auto new_history = agent::BuildCompactedHistory(history, archive);

    REQUIRE(new_history.size() == 1);
    CHECK(std::get<api::TextBlock>(new_history[0].content[0]).text.find("对话存档") != std::string::npos);
}

// P1-1 反涨的真机形状:mid-turn 长工具循环——从最新用户消息到尾全在一个
// "轮"里,旧实现整轮保留导致压缩后反而更长(实测 70.8k 压成 73.7k)。新
// 规矩:末轮超预算时轮头必保、其余按 assistant+tool_result 消息组从尾收,
// 压缩后的新历史必须显著小于原历史。
TEST_CASE("BuildCompactedHistory: mid-turn 巨轮超预算,按消息组从尾收,新历史显著收窄") {
    std::vector<api::Message> history;
    history.push_back(UserText("开始建图书系统,先读需求再写代码"));
    // 三组工具来回,每组的结果都巨大(4k ASCII 一枚,统一口径下约 1k token)。
    const std::string huge_result = std::string(4000, 'r');
    for (int i = 0; i < 3; ++i) {
        const std::string id = "tool_" + std::to_string(i);
        history.push_back(AssistantToolUse(id, "read_file"));
        history.push_back(UserToolResult(id, huge_result));
    }
    // 最后再补一组小来回:它该被优先保进热区。
    history.push_back(AssistantToolUse("tool_last", "search"));
    history.push_back(UserToolResult("tool_last", "短结果"));

    api::Message archive = UserText("[对话存档,此前内容已压缩] 摘要正文,不短,凑够四十个字符以上。");
    // 预算 1500 token:末轮总量约 3k+,超预算,走组收法。
    const auto new_history = agent::BuildCompactedHistory(history, archive, /*hot_zone_tokens=*/1500);

    // 1) 必须显著收窄:远小于原历史。
    CHECK(agent::EstimateHistoryTokens(new_history) < agent::EstimateHistoryTokens(history) / 2);

    // 2) 轮头(最新用户输入)必保,且存档并进它的开头。
    REQUIRE_FALSE(new_history.empty());
    REQUIRE(std::holds_alternative<api::TextBlock>(new_history[0].content[0]));
    const std::string& head_text = std::get<api::TextBlock>(new_history[0].content[0]).text;
    CHECK(head_text.find("对话存档") != std::string::npos);
    CHECK(head_text.find("开始建图书系统") != std::string::npos);

    // 3) 保留的消息组内 tool_use/tool_result 配对不破;裁掉的大结果不在。
    std::set<std::string> kept_use_ids;
    for (const auto& message : new_history) {
        for (const auto& block : message.content) {
            if (const auto* use = std::get_if<api::ToolUseBlock>(&block); use != nullptr) {
                kept_use_ids.insert(use->id);
            }
        }
    }
    for (const auto& id : kept_use_ids) {
        bool paired = false;
        for (const auto& message : new_history) {
            for (const auto& block : message.content) {
                if (const auto* result = std::get_if<api::ToolResultBlock>(&block);
                    result != nullptr && result->tool_use_id == id) {
                    paired = true;
                }
            }
        }
        CHECK(paired);
    }
    // 尾部的小组装得下,该进热区;靠头的巨结果组装不下,不进(从尾收:
    // 最近的工具来回最相关)。
    CHECK(kept_use_ids.count("tool_last") == 1);
    CHECK(kept_use_ids.count("tool_2") == 1);
    CHECK(kept_use_ids.count("tool_0") == 0);
    CHECK(kept_use_ids.count("tool_1") == 0);

    // 4) 没有相邻两条 user 消息(archive 并入轮头,不单独成条)。
    for (std::size_t i = 0; i + 1 < new_history.size(); ++i) {
        const bool both_user =
            new_history[i].role == api::Role::User && new_history[i + 1].role == api::Role::User;
        CHECK_FALSE(both_user);
    }
}

TEST_CASE("ShouldSkipCompactForHysteresis: 新增不足滞回带就跳过,攒足了才放行") {
    // 无进展(同一视图连压):跳过。
    CHECK(agent::ShouldSkipCompactForHysteresis(/*last_post=*/100000, /*before=*/100000));
    CHECK(agent::ShouldSkipCompactForHysteresis(100000, 100500));
    // 新增越过滞回带:放行。
    CHECK_FALSE(agent::ShouldSkipCompactForHysteresis(100000, 100000 + agent::kCompactHysteresisFloorTokens));
    // 历史反而更小(不该发生,防御):视为无进展,跳过。
    CHECK(agent::ShouldSkipCompactForHysteresis(100000, 90000));
    // 自定义带宽照算。
    CHECK(agent::ShouldSkipCompactForHysteresis(100, 104, /*floor_tokens=*/5));
    CHECK_FALSE(agent::ShouldSkipCompactForHysteresis(100, 106, /*floor_tokens=*/5));
}

TEST_CASE("BuildCompactedHistory: 空历史,新历史只有 archive") {
    std::vector<api::Message> history;
    api::Message archive = UserText("[对话存档,此前内容已压缩] 摘要");
    const auto new_history = agent::BuildCompactedHistory(history, archive);
    REQUIRE(new_history.size() == 1);
}

// ---------------------------------------------------------------------------
// Compact:请求形状与旧坑守护
// ---------------------------------------------------------------------------

TEST_CASE("Compact: 成功时返回 user 角色的存档消息与解析过的 manifest") {
    FakeBackend backend;
    backend.script = SummaryScript(kGoodSummary);

    std::vector<api::Message> history;
    history.push_back(UserText("帮我实现上下文管理 " + std::string(2400, 'x')));
    history.push_back(AssistantText("好的,开始"));

    const auto result = agent::Compact(backend, "test-model", history, PlainOptions());

    REQUIRE(result.has_value());
    CHECK(result->archive.role == api::Role::User);
    REQUIRE(std::holds_alternative<api::TextBlock>(result->archive.content[0]));
    const std::string& text = std::get<api::TextBlock>(result->archive.content[0]).text;
    CHECK(text.find("[对话存档,此前内容已压缩]") == 0);
    CHECK(text.find("关键文件是 config.cpp") != std::string::npos);
    CHECK(result->manifest.goal == "实现上下文管理与压缩");
    REQUIRE(result->manifest.open_items.size() == 2);
    CHECK(result->manifest.open_items[0] == "补齐单元测试");

    REQUIRE(backend.captured_requests.size() == 1);
    CHECK(backend.captured_requests[0].model == "test-model");
    // history 最后一条是 assistant,会被补一条 user 收尾消息(防 prefill
    // continuation 的老坑),所以是 history.size() + 1。
    REQUIRE(backend.captured_requests[0].messages.size() == history.size() + 1);
    CHECK(backend.captured_requests[0].messages.back().role == api::Role::User);
}

TEST_CASE("Compact: history 最后一条已经是 user 角色时,不额外补收尾消息") {
    FakeBackend backend;
    backend.script = SummaryScript(kGoodSummary);

    std::vector<api::Message> history;
    history.push_back(UserText("问题一 " + std::string(2400, 'x')));
    history.push_back(AssistantText("回答一"));
    history.push_back(UserToolResult("tool_1", "工具结果"));

    const auto result = agent::Compact(backend, "test-model", history, PlainOptions());

    REQUIRE(result.has_value());
    REQUIRE(backend.captured_requests.size() == 1);
    CHECK(backend.captured_requests[0].messages.size() == history.size());
}

TEST_CASE("Compact: focus 非空时,请求的 system 指令里带上 重点保留:xxx") {
    FakeBackend backend;
    backend.script = SummaryScript(kGoodSummary);
    agent::CompactOptions options = PlainOptions();
    options.focus = "数据库连接字符串";

    std::vector<api::Message> history;
    history.push_back(UserText("问题 " + std::string(2400, 'x')));

    const auto result = agent::Compact(backend, "test-model", history, options);

    REQUIRE(result.has_value());
    REQUIRE(backend.captured_requests.size() == 1);
    CHECK(backend.captured_requests[0].system.find("重点保留:数据库连接字符串") != std::string::npos);
    // manifest 模板也进了指令,模型才知道要产出机器骨架。
    CHECK(backend.captured_requests[0].system.find("\"open_items\"") != std::string::npos);
}

TEST_CASE("Compact: 后端失败时返回错误,不影响传入的原始 history") {
    FakeBackend backend;
    backend.fail = true;
    backend.fail_message = "连接超时";

    std::vector<api::Message> history;
    history.push_back(UserText("问题"));
    history.push_back(AssistantText("回答"));

    const auto result = agent::Compact(backend, "test-model", history, PlainOptions());

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message == "连接超时");
    REQUIRE(history.size() == 2);
}

TEST_CASE("Compact: 流内报 StreamError 也算失败,返回错误") {
    FakeBackend backend;
    backend.script = {
        api::MessageStart{"msg", "model"},
        api::StreamError{"服务器繁忙"},
    };

    std::vector<api::Message> history;
    history.push_back(UserText("问题"));

    const auto result = agent::Compact(backend, "test-model", history, PlainOptions());

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("服务器繁忙") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Compact:manifest 三道验收
// ---------------------------------------------------------------------------

TEST_CASE("Compact: 摘要剥空白后为空 → 拒收,返回错误不给存档") {
    FakeBackend backend;
    backend.script = SummaryScript("  \n\t  \r\n ");

    std::vector<api::Message> history;
    history.push_back(UserText("问题"));

    const auto result = agent::Compact(backend, "test-model", history, PlainOptions());

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("历史未动") != std::string::npos);
}

TEST_CASE("Compact: 摘要不足 40 字 → 拒收(prefill continuation 只回一个字的老坑)") {
    FakeBackend backend;
    backend.script = SummaryScript("2");

    std::vector<api::Message> history;
    history.push_back(UserText("问题"));
    history.push_back(AssistantText("2"));

    const auto result = agent::Compact(backend, "test-model", history, PlainOptions());

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("过短") != std::string::npos);
    CHECK(result.error().message.find("历史未动") != std::string::npos);
}

TEST_CASE("Compact: 40 字够但末尾没有 JSON manifest → 拒收") {
    FakeBackend backend;
    std::string forty_plus =
        "## 任务目标\n实现上下文管理与压缩,不短,凑够四十字免得跟最外层防呆混为一谈。\n";
    backend.script = SummaryScript(forty_plus);

    std::vector<api::Message> history;
    history.push_back(UserText("问题"));

    const auto result = agent::Compact(backend, "test-model", history, PlainOptions());

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("manifest") != std::string::npos);
    CHECK(result.error().message.find("历史未动") != std::string::npos);
}

TEST_CASE("Compact: manifest 是坏 JSON → 拒收") {
    FakeBackend backend;
    backend.script = SummaryScript(LongSummaryWithManifest(
        "{\"goal\": \"实现压缩\", \"open_items\": [没引起来], \"next_action\": \"x\"}"));

    std::vector<api::Message> history;
    history.push_back(UserText("问题"));

    const auto result = agent::Compact(backend, "test-model", history, PlainOptions());

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("manifest") != std::string::npos);
}

TEST_CASE("Compact: manifest 缺 goal 键 → 拒收") {
    FakeBackend backend;
    backend.script =
        SummaryScript(LongSummaryWithManifest("{\"open_items\": [\"补测试\"], \"next_action\": \"x\"}"));

    std::vector<api::Message> history;
    history.push_back(UserText("问题"));

    const auto result = agent::Compact(backend, "test-model", history, PlainOptions());

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("manifest") != std::string::npos);
}

TEST_CASE("Compact: 守恒校验——活动待办漏一项 → 拒收,历史不动") {
    FakeBackend backend;
    // open_items 只有一条,丢了"写文档说明"。
    backend.script = SummaryScript(LongSummaryWithManifest("", R"(["补齐单元测试"])"));

    std::vector<api::Message> history;
    history.push_back(UserText("问题 " + std::string(2400, 'x')));
    agent::CompactOptions options = PlainOptions();
    options.required_open_items = {"补齐单元测试", "写文档说明"};

    const auto result = agent::Compact(backend, "test-model", history, options);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("守恒校验") != std::string::npos);
    CHECK(result.error().message.find("写文档说明") != std::string::npos);
    CHECK(result.error().message.find("历史未动") != std::string::npos);
}

TEST_CASE("Compact: 守恒校验——待办逐字都在(空白归一后) → 收下") {
    FakeBackend backend;
    backend.script =
        SummaryScript(LongSummaryWithManifest("", "[\"补齐 单元测试\", \"写 文档 说明\"]"));

    std::vector<api::Message> history;
    history.push_back(UserText("问题 " + std::string(2400, 'x')));
    agent::CompactOptions options = PlainOptions();
    options.required_open_items = {"补齐单元测试", "写文档说明"};

    const auto result = agent::Compact(backend, "test-model", history, options);

    REQUIRE(result.has_value());
    REQUIRE(result->manifest.open_items.size() == 2);
}

TEST_CASE("Compact: 指令把必须守恒的待办逐条列给模型") {
    FakeBackend backend;
    backend.script = SummaryScript(LongSummaryWithManifest("", R"(["修分块预算","补测试"])"));

    std::vector<api::Message> history;
    history.push_back(UserText("问题 " + std::string(2400, 'x')));
    agent::CompactOptions options = PlainOptions();
    options.required_open_items = {"修分块预算", "补测试"};

    REQUIRE(agent::Compact(backend, "test-model", history, options).has_value());
    const std::string& system = backend.captured_requests[0].system;
    CHECK(system.find("一项都不许丢、不许改写") != std::string::npos);
    CHECK(system.find("1. 修分块预算") != std::string::npos);
    CHECK(system.find("2. 补测试") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Compact:压缩模型自己的窗口预算
// ---------------------------------------------------------------------------

TEST_CASE("Compact: 窗口装不下当前历史 → 明确拒绝,不发请求,不截史") {
    FakeBackend backend;
    backend.script = SummaryScript(kGoodSummary);  // 就算后端肯回,也轮不到发

    std::vector<api::Message> history;
    history.push_back(UserText(std::string(20000, 'x')));  // 20000 ASCII ≈ 5000 token

    agent::CompactOptions options;
    options.budget.window_tokens = 4096;  // 减去预留后预算极小,必装不下

    const auto result = agent::Compact(backend, "test-model", history, options);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("装不下") != std::string::npos);
    CHECK(result.error().message.find("历史一字未动") != std::string::npos);
    // 关键:压根没发请求——拒绝发生在发送前,不是截了史再发。
    CHECK(backend.captured_requests.empty());
    // 历史原样。
    REQUIRE(history.size() == 1);
    CHECK(std::get<api::TextBlock>(history[0].content[0]).text.size() == 20000);
}

TEST_CASE("Compact: 窗口装得下 → 照常发送") {
    FakeBackend backend;
    backend.script = SummaryScript(kGoodSummary);

    std::vector<api::Message> history;
    history.push_back(UserText("小问题 " + std::string(2400, 'x')));

    agent::CompactOptions options;
    options.budget.window_tokens = 32768;

    REQUIRE(agent::Compact(backend, "test-model", history, options).has_value());
    REQUIRE(backend.captured_requests.size() == 1);
}

TEST_CASE("CompactInputBudget: 预算 = 窗口 − 输出预留 − 协议余量") {
    agent::CompactBudget budget;
    budget.window_tokens = 100000;
    budget.output_reserve_tokens = 4096;
    budget.protocol_headroom_tokens = 2048;
    CHECK(agent::CompactInputBudget(budget).has_value());
    CHECK(*agent::CompactInputBudget(budget) == 100000 - 4096 - 2048);
    // 窗口未知 → 无预算可核。
    agent::CompactBudget unknown;
    CHECK_FALSE(agent::CompactInputBudget(unknown).has_value());
    // 窗口连预留都盖不住 → 预算 0,任何输入都装不下。
    agent::CompactBudget tiny;
    tiny.window_tokens = 100;
    CHECK(agent::CompactInputBudget(tiny).has_value());
    CHECK(*agent::CompactInputBudget(tiny) == 0);
}

TEST_CASE("ParseCompactManifest: 取末尾最后一个 json 围栏块,前面的不算") {
    const std::string text =
        "正文里先引用了一枚代码块:\n```json\n{\"goal\": \"旧的\", \"open_items\": [\"旧\"], "
        "\"next_action\": \"旧的下一步\"}\n```\n"
        "存档正文若干字若干字若干字若干字。\n"
        "```json\n{\"goal\": \"新的\", \"open_items\": [\"新\"], \"next_action\": \"新的下一步\"}\n```";
    const auto manifest = agent::ParseCompactManifest(text);
    REQUIRE(manifest.has_value());
    CHECK(manifest->goal == "新的");
    REQUIRE(manifest->open_items.size() == 1);
    CHECK(manifest->open_items[0] == "新");
    CHECK(manifest->next_action == "新的下一步");
}

// ---------------------------------------------------------------------------
// schema/type 收紧(Compact 四分区单·阶段 0):constraints/next_action 的
// 坏形状整枚 manifest 判坏,不再静默吞掉半截字段。
// ---------------------------------------------------------------------------

TEST_CASE("ParseCompactManifest: constraints 不是数组 → 整枚判坏") {
    const std::string text = "正文若干字若干字若干字若干字若干字若干字若干字。\n```json\n" +
        std::string(R"({"goal": "目标", "constraints": "只许修改 compact", "open_items": [], "next_action": "继续"})") +
        "\n```";
    CHECK_FALSE(agent::ParseCompactManifest(text).has_value());
}

TEST_CASE("ParseCompactManifest: constraints 混进非字符串或空白空串 → 整枚判坏") {
    const std::string with_number = "正文若干字若干字若干字若干字若干字若干字若干字。\n```json\n" +
        std::string(R"({"goal": "目标", "constraints": [3], "open_items": [], "next_action": "继续"})") + "\n```";
    CHECK_FALSE(agent::ParseCompactManifest(with_number).has_value());
    const std::string with_blank = "正文若干字若干字若干字若干字若干字若干字若干字。\n```json\n" +
        std::string(R"({"goal": "目标", "constraints": ["  "], "open_items": [], "next_action": "继续"})") + "\n```";
    CHECK_FALSE(agent::ParseCompactManifest(with_blank).has_value());
}

TEST_CASE("ParseCompactManifest: constraints 没写或空数组都合法") {
    const std::string absent = "正文若干字若干字若干字若干字若干字若干字若干字。\n```json\n" +
        std::string(R"({"goal": "目标", "open_items": ["待办"], "next_action": "继续"})") + "\n```";
    const auto manifest = agent::ParseCompactManifest(absent);
    REQUIRE(manifest.has_value());
    CHECK(manifest->constraints.empty());
    const std::string empty_array = "正文若干字若干字若干字若干字若干字若干字若干字。\n```json\n" +
        std::string(R"({"goal": "目标", "constraints": [], "open_items": [], "next_action": "继续"})") + "\n```";
    REQUIRE(agent::ParseCompactManifest(empty_array).has_value());
}

TEST_CASE("ParseCompactManifest: next_action 缺席/非字符串/空白空串 → 整枚判坏") {
    const auto make = [](const std::string& manifest_json) {
        return "正文若干字若干字若干字若干字若干字若干字若干字。\n```json\n" + manifest_json + "\n```";
    };
    CHECK_FALSE(agent::ParseCompactManifest(
        make(R"({"goal": "目标", "open_items": [], "constraints": []})")).has_value());
    CHECK_FALSE(agent::ParseCompactManifest(
        make(R"({"goal": "目标", "open_items": [], "constraints": [], "next_action": 7})")).has_value());
    CHECK_FALSE(agent::ParseCompactManifest(
        make(R"({"goal": "目标", "open_items": [], "constraints": [], "next_action": "   "})")).has_value());
    CHECK_FALSE(agent::ParseCompactManifest(
        make(R"({"goal": "   ", "open_items": [], "constraints": [], "next_action": "继续"})")).has_value());
}

TEST_CASE("ParseCompactManifest: open_items 混进非字符串 → 整枚判坏") {
    const std::string text = "正文若干字若干字若干字若干字若干字若干字若干字。\n```json\n" +
        std::string(R"({"goal": "目标", "open_items": ["好的", 12], "constraints": [], "next_action": "继续"})") +
        "\n```";
    CHECK_FALSE(agent::ParseCompactManifest(text).has_value());
}

TEST_CASE("Compact: manifest 缺 next_action → 拒收,历史不动(schema 收紧)") {
    FakeBackend backend;
    // 正文够长、有 json 围栏、goal/open_items 都在——只缺 next_action。
    backend.script = SummaryScript(LongSummaryWithManifest(
        R"({"goal": "实现压缩", "constraints": [], "open_items": ["补测试"]})"));

    std::vector<api::Message> history;
    history.push_back(UserText("问题 " + std::string(2400, 'x')));

    const auto result = agent::Compact(backend, "test-model", history, PlainOptions());

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("manifest") != std::string::npos);
    CHECK(result.error().message.find("历史未动") != std::string::npos);
}

TEST_CASE("Compact: 历史不比摘要大 → 压了反而更长,拒收") {
    FakeBackend backend;
    backend.script = SummaryScript(kGoodSummary);  // 摘要正文数百字

    std::vector<api::Message> history;
    history.push_back(UserText("短"));  // 历史统共几个 token

    const auto result = agent::Compact(backend, "test-model", history, PlainOptions());

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("压了反而更长") != std::string::npos);
    CHECK(result.error().message.find("历史未动") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 守护测试:压缩只动 history,绝不动 system
// ---------------------------------------------------------------------------

TEST_CASE("守护:压缩前后 AgentLoop 发出的 system 逐字节不变") {
    const std::string system_prompt =
        "系统提示原文——含人格段、环境段、features 段,一个字节都不许因压缩而变。";

    FakeBackend loop_backend;
    loop_backend.script = SummaryScript("这一轮的普通回答,不短,凑够字数免得跟压缩门槛混为一谈。");
    tools::ToolRegistry registry;  // 空表:这轮不调工具
    agent::AgentProfile loop_profile;
    loop_profile.request.model = "test-model";
    loop_profile.system_prompt = system_prompt;
    agent::Agent loop(loop_backend, registry, std::move(loop_profile));

    REQUIRE(loop.Run("第一问 " + std::string(2400, 'x'), agent::TurnWiring{}).has_value());
    REQUIRE_FALSE(loop_backend.captured_requests.empty());
    const std::string system_before = loop_backend.captured_requests.front().system;
    CHECK(system_before == system_prompt);

    // 压缩走独立后端(跟会话层用裸 real_backend 一个路数),替换历史。
    FakeBackend compact_backend;
    compact_backend.script = SummaryScript(kGoodSummary);
    const auto summary = agent::Compact(compact_backend, "test-model", loop.History(), PlainOptions());
    REQUIRE(summary.has_value());
    loop.ReplaceHistory(agent::BuildCompactedHistory(loop.History(), summary->archive));

    REQUIRE(loop.Run("第二问", agent::TurnWiring{}).has_value());
    const std::string system_after = loop_backend.captured_requests.back().system;

    CHECK(system_after == system_before);
    CHECK(system_after == system_prompt);
    bool archived = false;
    for (const auto& message : loop_backend.captured_requests.back().messages) {
        for (const auto& block : message.content) {
            if (std::holds_alternative<api::TextBlock>(block) &&
                std::get<api::TextBlock>(block).text.find("对话存档") != std::string::npos) {
                archived = true;
            }
        }
    }
    CHECK(archived);
}

// ---------------------------------------------------------------------------
// 0.31.x:AgentLoop mid-turn 压力通报
// ---------------------------------------------------------------------------

TEST_CASE("AgentLoop: 窗口未知或没设回调时,请求前不做任何通报(行为不变)") {
    FakeBackend backend;
    backend.script = SummaryScript("普通回答,凑够字数,不触发任何压缩门槛。");
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "sys"});
    int calls = 0;
    agent::AgentWiring wiring;
    wiring.on_context_pressure = [&calls](const agent::ContextPressure&) { ++calls; };
    loop.SetWiring(std::move(wiring));
    REQUIRE(loop.Run("第一问 " + std::string(2400, 'x'), agent::TurnWiring{}).has_value());
    CHECK(calls == 0);  // 窗口 0 = 未知,不评估
}

TEST_CASE("AgentLoop: projected overflow 在请求前通报,回调里压缩后请求换短史") {
    // 一轮脚本:先答一句长的(带工具调用会复杂化,这里纯文本即可)。
    FakeBackend backend;
    backend.script = SummaryScript("回答正文,凑够字数,免得跟压缩门槛混淆。");
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"},
                                          .runtime{.max_output_tokens = 8192,
                                                   .max_context_chars = 200000},
                                          .system_prompt = "sys"});
    api::Message old_user;
    old_user.role = api::Role::User;
    std::string short_terms;
    short_terms.reserve(100000);
    // 压缩触发失衡单后是双闸:托底尺虚算(projected)与真实水位(日常尺,
    // "a " 串按 ASCII/4 只有虚算一半)须同时过线。50000 词让真实水位也过
    // 60% 线——短词漏算的形状照样唤醒压缩,只是不再单看虚算。
    for (int i = 0; i < 50000; ++i) short_terms += "a ";
    old_user.content.push_back(api::TextBlock{std::move(short_terms)});
    api::Message old_assistant;
    old_assistant.role = api::Role::Assistant;
    old_assistant.content.push_back(api::TextBlock{"旧回答"});
    loop.ReplaceHistory({std::move(old_user), std::move(old_assistant)});
    loop.SetTurnContext("project memory context");
    loop.SetContextWindowTokens(32768);  // 旧 ASCII 尺会漏算，保守预检须先唤醒压缩

    std::vector<agent::ContextPressure> seen;
    bool compacted = false;
    agent::AgentWiring wiring;
    wiring.on_context_pressure = [&](const agent::ContextPressure& pressure) {
        seen.push_back(pressure);
        if (pressure.phase == agent::ContextPressure::Phase::PreRequest && pressure.projected_overflow &&
            !compacted) {
            compacted = true;
            // 安全点换短史(模拟会话层做了一次语义压缩)。
            api::Message archive;
            archive.role = api::Role::User;
            archive.content.push_back(api::TextBlock{"[对话存档,此前内容已压缩] 摘要正文,不短。"});
            loop.ReplaceHistory({archive});
        }
    };
    loop.SetWiring(std::move(wiring));

    REQUIRE(loop.Run("新问题", agent::TurnWiring{}).has_value());

    REQUIRE_FALSE(seen.empty());
    CHECK(seen.front().phase == agent::ContextPressure::Phase::PreRequest);
    CHECK(seen.front().projected_overflow);
    CHECK(seen.front().window_tokens == 32768);
    CHECK(compacted);
    // 请求确实是用换短后的历史发的:只有一条 archive 消息。
    REQUIRE(backend.captured_requests.size() == 1);
    REQUIRE(backend.captured_requests[0].messages.size() == 1);
    CHECK(std::get<api::TextBlock>(backend.captured_requests[0].messages[0].content[0]).text.find("对话存档") !=
          std::string::npos);
    REQUIRE(backend.captured_requests[0].messages[0].content.size() == 2);
    const auto* context = std::get_if<api::TextBlock>(&backend.captured_requests[0].messages[0].content[1]);
    REQUIRE(context != nullptr);
    CHECK(context->text == "project memory context");

    // compact 换的是持久历史；动态上下文只补回请求视图。
    for (const auto& message : loop.History()) {
        for (const auto& block : message.content) {
            const auto* text = std::get_if<api::TextBlock>(&block);
            if (text != nullptr) {
                CHECK(text->text.find("project memory context") == std::string::npos);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 压缩触发失衡单:projected 虚算 × 80% 参考线 = 真实 ~25% 就误触发
// midturn 压缩。修法双闸——projected(保守托底尺 + 输出预留,工作视图
// 口径)过 kProjectedOverflowPercent 之外,真实水位(同一副工作视图,日常
// 尺)也须过 kRealOverflowPercent;虚算单独不触发,该压的仍压。
// ---------------------------------------------------------------------------

TEST_CASE("AgentLoop: 预估虚算单独不触发 midturn 压缩——真实水位约三成不再被喊溢出") {
    // §三复现场景:256k 窗口,历史是短词密集的重复工具结果(run_command
    // 是副作用工具,判重不给开门,结构压缩后重复结果仍全文)——保守托底
    // 尺逐词计数把 projected 虚算到 80% 参考线之上,日常尺的真实水位却只有
    // ~29%。修前单闸即真:midturn 压缩被误触发,压完"没有冷区榨不出收益"
    // 空跑收场(用户真机 24% 即中的那副形状);修后双闸不同开,不触发。
    FakeBackend backend;
    backend.script = SummaryScript("回答正文,凑够字数,免得跟压缩门槛混淆。");
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"},
                                          .runtime{.max_output_tokens = 65536, .max_context_chars = 600000},
                                          .system_prompt = "sys"});
    // 20 组 run_command 工具来回:每组 7500 个短词(15000 字节),托底尺
    // 记 7500、日常尺只记 3750——虚算恰是真实水位的两倍。
    std::vector<api::Message> history{UserText("跑构建")};
    std::string short_terms;
    short_terms.reserve(15000);
    for (int i = 0; i < 7500; ++i) short_terms += "a ";
    for (int i = 0; i < 20; ++i) {
        const std::string id = "cmd_" + std::to_string(i);
        history.push_back(AssistantToolUse(id, "run_command"));
        history.push_back(UserToolResult(id, short_terms));
    }
    loop.ReplaceHistory(std::move(history));
    loop.SetContextWindowTokens(262144);  // 256k
    std::vector<agent::ContextPressure> seen;
    agent::AgentWiring wiring;
    wiring.on_context_pressure = [&](const agent::ContextPressure& pressure) { seen.push_back(pressure); };
    loop.SetWiring(std::move(wiring));

    REQUIRE(loop.Run("新问题", agent::TurnWiring{}).has_value());

    REQUIRE_FALSE(seen.empty());
    REQUIRE(seen.front().phase == agent::ContextPressure::Phase::PreRequest);
    // 事件账如实反映双闸口径:projected(托底尺 + 输出预留)过了 80% 参考
    // 线,working_view_tokens(真实水位,日常尺)远没过 60% 线。
    CHECK(seen.front().window_tokens == 262144);
    CHECK(seen.front().projected_tokens >= 262144 * 80 / 100);
    CHECK(seen.front().working_view_tokens < 262144 * 60 / 100);
    CHECK_FALSE(seen.front().working_view_overflow);
    CHECK_FALSE(seen.front().projected_overflow);
}

TEST_CASE("AgentLoop: 真实水位过 60% 但 projected 未到 80% 参考线,不触发") {
    // A 闸自己的钉:日常文本(floor ≈ 日常尺,不虚算)真实水位 64%,但
    // projected 加上输出预留仍不到 80% 参考线——不压,留给 turn 间的
    // ShouldAutoCompact 按真实 usage 收口。
    FakeBackend backend;
    backend.script = SummaryScript("回答正文,凑够字数,免得跟压缩门槛混淆。");
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"},
                                          .runtime{.max_output_tokens = 2048},
                                          .system_prompt = "sys"});
    api::Message old_user;
    old_user.role = api::Role::User;
    std::string plain_words;
    plain_words.reserve(84000);
    for (int i = 0; i < 16800; ++i) plain_words += "word ";  // 84000 字节,两尺都记 21000
    old_user.content.push_back(api::TextBlock{std::move(plain_words)});
    loop.ReplaceHistory({std::move(old_user)});
    loop.SetContextWindowTokens(32768);
    std::vector<agent::ContextPressure> seen;
    agent::AgentWiring wiring;
    wiring.on_context_pressure = [&](const agent::ContextPressure& pressure) { seen.push_back(pressure); };
    loop.SetWiring(std::move(wiring));

    REQUIRE(loop.Run("新问题", agent::TurnWiring{}).has_value());

    REQUIRE_FALSE(seen.empty());
    REQUIRE(seen.front().phase == agent::ContextPressure::Phase::PreRequest);
    CHECK(seen.front().working_view_tokens >= 32768 * 60 / 100);  // 真实水位 ~64%
    CHECK(seen.front().working_view_overflow);
    CHECK(seen.front().projected_tokens < 32768 * 80 / 100);  // 未到参考线
    CHECK_FALSE(seen.front().projected_overflow);
}

TEST_CASE("AgentLoop: 真实水位 ≥80% 该触发仍触发(双闸同开)") {
    // 不许把该压的放过去:真实水位 82%,projected 过参考线——两闸同开,
    // midturn 压缩照常唤醒(回调里换短史后请求用新史重拼,既有册钉着)。
    FakeBackend backend;
    backend.script = SummaryScript("回答正文,凑够字数,免得跟压缩门槛混淆。");
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"},
                                          .runtime{.max_output_tokens = 8192},
                                          .system_prompt = "sys"});
    api::Message old_user;
    old_user.role = api::Role::User;
    std::string plain_words;
    plain_words.reserve(108000);
    for (int i = 0; i < 21600; ++i) plain_words += "word ";  // 108000 字节,两尺都记 27000
    old_user.content.push_back(api::TextBlock{std::move(plain_words)});
    loop.ReplaceHistory({std::move(old_user)});
    loop.SetContextWindowTokens(32768);
    std::vector<agent::ContextPressure> seen;
    agent::AgentWiring wiring;
    wiring.on_context_pressure = [&](const agent::ContextPressure& pressure) { seen.push_back(pressure); };
    loop.SetWiring(std::move(wiring));

    REQUIRE(loop.Run("新问题", agent::TurnWiring{}).has_value());

    REQUIRE_FALSE(seen.empty());
    REQUIRE(seen.front().phase == agent::ContextPressure::Phase::PreRequest);
    CHECK(seen.front().working_view_tokens >= 32768 * 80 / 100);  // 真实水位 ~82%
    CHECK(seen.front().working_view_overflow);
    CHECK(seen.front().projected_tokens >= 32768 * 80 / 100);
    CHECK(seen.front().projected_overflow);
}

TEST_CASE("AgentLoop: TrimHistory 兜底真丢东西时,AfterHardTrim 通报") {
    FakeBackend backend;
    backend.script = SummaryScript("回答正文,凑够字数,免得跟压缩门槛混淆。");
    tools::ToolRegistry registry;
    // max_context_chars 设得很小:第五轮起(轮数盖过 keep_recent_turns+1)
    // 必触发轮级裁剪。
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .runtime{.max_output_tokens = 4096, .max_steps_per_turn = 0, .max_context_chars = 2600}, .system_prompt = "sys"});
    loop.SetContextWindowTokens(0);  // 不做 projected 评估,单测硬裁线

    std::vector<agent::ContextPressure> seen;
    agent::AgentWiring wiring;
    wiring.on_context_pressure = [&seen](const agent::ContextPressure& pressure) { seen.push_back(pressure); };
    loop.SetWiring(std::move(wiring));

    // 前四轮:轮数不够裁,没有通报。
    for (int i = 0; i < 4; ++i) {
        REQUIRE(loop.Run("第" + std::to_string(i) + "问" + std::string(500, 'x'), agent::TurnWiring{}).has_value());
    }
    CHECK(seen.empty());

    // 第五轮:轮数盖过 keep_recent_turns + 1,中间轮被整轮丢掉,请求发出去
    // 之前必须通报——静默降级不许再有。
    REQUIRE(loop.Run("第5问" + std::string(500, 'x'), agent::TurnWiring{}).has_value());
    REQUIRE_FALSE(seen.empty());
    bool saw_hard_trim = false;
    for (const auto& pressure : seen) {
        if (pressure.phase == agent::ContextPressure::Phase::AfterHardTrim) {
            saw_hard_trim = true;
            CHECK(pressure.hard_trimmed_turns);
            CHECK(pressure.hard_dropped_messages > 0);
        }
    }
    CHECK(saw_hard_trim);
}

// ---------------------------------------------------------------------------
// 第三期:episode 切分与 map/reduce 分层压缩
// ---------------------------------------------------------------------------

namespace {

// 多脚本后端:每个请求吃队首一份脚本,吃完的出队;队空再要就失败。
// map/reduce 每一步的应答不同,得按次序喂。
class ScriptedBackend : public api::Backend {
public:
    std::deque<std::vector<api::StreamEvent>> scripts;
    bool fail_when_empty = true;
    std::vector<api::Request> captured_requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
        captured_requests.push_back(request);
        if (scripts.empty()) {
            if (fail_when_empty) {
                return std::unexpected(api::Error{api::ErrorKind::Api, "脚本用尽", 0});
            }
            return {};
        }
        const auto script = scripts.front();
        scripts.pop_front();
        for (const auto& event : script) {
            on_event(event);
        }
        return {};
    }
};

std::vector<api::StreamEvent> TextScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

// 一段局部小结(map 产物):五栏 + 末尾 json manifest。
std::string EpisodeText(const std::string& tag) {
    return "## 阶段目标\n" + tag + " 阶段的目标说明文字,足够长\n"
           "## 已证实的事实\n" + tag + " 的事实\n## 关键决策\n(无)\n"
           "## 涉及文件与符号\nsrc/" + tag + ".cpp\n## 未完成事项\n" + tag + " 收尾\n"
           "```json\n{\"goal\": \"" + tag + " 阶段目标\", \"constraints\": [], \"open_items\": [\"" + tag +
           " 收尾\"], \"next_action\": \"继续\"}\n```";
}

// 归并存档(reduce 产物):终稿六栏 + 守恒 manifest。
std::string ReduceText(const std::string& open_items_json = R"(["总收尾"])") {
    return "## 任务目标\n把分阶段压缩做完\n## 已证实的事实\n三段小结都齐了\n"
           "## 关键决策\nmap 再 reduce\n## 涉及文件与符号\ncompact.cpp\n"
           "## 关键命令与结果\n(无)\n## 未完成事项\n总收尾\n"
           "```json\n{\"goal\": \"把分阶段压缩做完\", \"constraints\": [\"不许猜补\"], \"open_items\": " +
           open_items_json + ", \"next_action\": \"收尾\"}\n```";
}

// 四轮大历史(每轮 ~1000 token),窗口预算只够装单轮。
std::vector<api::Message> BigHistory() {
    std::vector<api::Message> history;
    for (int i = 0; i < 4; ++i) {
        history.push_back(UserText("第" + std::to_string(i) + "问 " + std::string(4000, 'a')));
        history.push_back(AssistantText("答" + std::to_string(i) + " " + std::string(100, 'b')));
    }
    return history;
}

agent::CompactOptions SmallWindowOptions() {
    agent::CompactOptions options;
    options.budget.window_tokens = 4000;
    options.budget.output_reserve_tokens = 512;
    options.budget.protocol_headroom_tokens = 0;
    return options;
}

}  // namespace

TEST_CASE("SplitEpisodes: 每条外层用户输入与 todo_write 都开新段") {
    std::vector<api::Message> history;
    history.push_back(UserText("u1"));
    history.push_back(AssistantText("a1"));
    history.push_back(AssistantText("a1b"));  // 同段内第二条 assistant
    history.push_back(UserText("u2"));
    // todo_write 在轮中间:plan 变化,自成边界。
    history.push_back(AssistantToolUse("t9", "todo_write"));
    history.push_back(UserToolResult("t9", "ok"));
    history.push_back(AssistantText("a2"));
    history.push_back(UserText("u3"));
    history.push_back(AssistantText("a3"));

    const auto episodes = agent::SplitEpisodes(history);
    // 段界:u2 处、todo_write 处、u3 处。
    REQUIRE(episodes.size() == 4);
    CHECK(episodes[0] == std::make_pair(std::size_t{0}, std::size_t{3}));
    CHECK(episodes[1] == std::make_pair(std::size_t{3}, std::size_t{4}));  // u2 单独
    CHECK(episodes[2] == std::make_pair(std::size_t{4}, std::size_t{7}));  // todo_write 起
    CHECK(episodes[3] == std::make_pair(std::size_t{7}, std::size_t{9}));
}

TEST_CASE("CompactHierarchical: 装得下退化为单次,一条请求") {
    ScriptedBackend backend;
    backend.scripts.push_back(SummaryScript(kGoodSummary));

    std::vector<api::Message> history;
    history.push_back(UserText("小历史 " + std::string(2400, 'x')));
    history.push_back(AssistantText("小回答"));

    agent::CompactOptions options;  // 窗口未知 → 单次
    const auto result = agent::CompactHierarchical(backend, "test-model", history, options);

    REQUIRE(result.has_value());
    CHECK_FALSE(result->metrics.hierarchical);
    CHECK(result->metrics.chunks == 1);
    REQUIRE(backend.captured_requests.size() == 1);
}

TEST_CASE("CompactHierarchical: 装不下走 map/reduce,块界不劈 tool 对") {
    ScriptedBackend backend;
    // 冷区三轮 → 3 份 map 小结 + 1 份 reduce 归并。
    backend.scripts.push_back(TextScript(EpisodeText("壹")));
    backend.scripts.push_back(TextScript(EpisodeText("贰")));
    backend.scripts.push_back(TextScript(EpisodeText("叁")));
    backend.scripts.push_back(TextScript(ReduceText()));

    const std::vector<api::Message> history = BigHistory();
    const auto result = agent::CompactHierarchical(backend, "test-model", history, SmallWindowOptions());

    REQUIRE(result.has_value());
    CHECK(result->metrics.hierarchical);
    CHECK(result->metrics.chunks == 3);
    CHECK(result->metrics.reduce_passes == 0);
    CHECK(result->manifest.goal == "把分阶段压缩做完");
    CHECK(std::get<api::TextBlock>(result->archive.content[0]).text.find("把分阶段压缩做完") != std::string::npos);

    // 请求形状:前 3 份 map(指令带"局部小结",各吃一段消息),末份 reduce。
    REQUIRE(backend.captured_requests.size() == 4);
    for (std::size_t i = 0; i < 3; ++i) {
        const api::Request& map_request = backend.captured_requests[i];
        CHECK(map_request.system.find("局部小结") != std::string::npos);
        // 一轮 = u + a,末条 assistant 另补一条 user 收尾(防 prefill 老坑)。
        REQUIRE(map_request.messages.size() == 3);
        CHECK(map_request.messages.back().role == api::Role::User);
    }
    const api::Request& reduce_request = backend.captured_requests.back();
    CHECK(reduce_request.system.find("归并") != std::string::npos);
    // reduce 的输入带着三份小结与来源事件号。
    const std::string reduce_body = std::get<api::TextBlock>(reduce_request.messages[0].content[0]).text;
    CHECK(reduce_body.find("局部小结 1/3") != std::string::npos);
    CHECK(reduce_body.find("来源事件 e") != std::string::npos);
    CHECK(reduce_body.find("壹") != std::string::npos);
    CHECK(reduce_body.find("叁") != std::string::npos);
}

TEST_CASE("CompactHierarchical: 守恒校验在终稿上照样拦,历史不动") {
    ScriptedBackend backend;
    backend.scripts.push_back(TextScript(EpisodeText("壹")));
    backend.scripts.push_back(TextScript(EpisodeText("贰")));
    backend.scripts.push_back(TextScript(EpisodeText("叁")));
    // 终稿把"补分块预算"这条待办弄丢了。
    backend.scripts.push_back(TextScript(ReduceText(R"(["只留一条"])")));

    std::vector<api::Message> history = BigHistory();
    agent::CompactOptions options = SmallWindowOptions();
    options.required_open_items = {"补分块预算"};

    const auto result = agent::CompactHierarchical(backend, "test-model", history, options);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("守恒校验") != std::string::npos);
    CHECK(result.error().message.find("补分块预算") != std::string::npos);
    CHECK(history.size() == 8);  // 原史一字未动
}

TEST_CASE("CompactHierarchical: map 任一块失败,整趟失败,旧历史不动") {
    ScriptedBackend backend;
    backend.scripts.push_back(TextScript(EpisodeText("壹")));
    backend.scripts.push_back(TextScript(EpisodeText("贰")));
    backend.scripts.push_back({api::MessageStart{"msg", "model"}, api::StreamError{"中途断流"}});

    std::vector<api::Message> history = BigHistory();
    const auto result = agent::CompactHierarchical(backend, "test-model", history, SmallWindowOptions());

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("中途断流") != std::string::npos);
    CHECK(history.size() == 8);
}

TEST_CASE("CompactHierarchical: 上一轮存档只进 reduce 当参考,不进 map 块") {
    ScriptedBackend backend;
    backend.scripts.push_back(TextScript(EpisodeText("壹")));
    backend.scripts.push_back(TextScript(EpisodeText("贰")));
    backend.scripts.push_back(TextScript(EpisodeText("叁")));
    backend.scripts.push_back(TextScript(ReduceText()));

    // 首条消息带上一轮存档(BuildCompactedHistory 并入的形状)。
    std::vector<api::Message> history = BigHistory();
    api::Message& first = history[0];
    std::get<api::TextBlock>(first.content[0]).text =
        "[对话存档,此前内容已压缩] 上一轮的存档正文,不许复印。\n"
        "```json\n{\"goal\": \"旧目标\", \"open_items\": []}\n```\n\n" +
        std::get<api::TextBlock>(first.content[0]).text;

    const auto result = agent::CompactHierarchical(backend, "test-model", history, SmallWindowOptions());
    REQUIRE(result.has_value());
    REQUIRE(backend.captured_requests.size() == 4);
    // map 块不吃旧存档(阻断"摘要复印摘要")……
    for (std::size_t i = 0; i + 1 < backend.captured_requests.size(); ++i) {
        for (const auto& message : backend.captured_requests[i].messages) {
            for (const auto& block : message.content) {
                if (std::holds_alternative<api::TextBlock>(block)) {
                    CHECK(std::get<api::TextBlock>(block).text.find("上一轮的存档正文") == std::string::npos);
                }
            }
        }
    }
    // ……reduce 把它当参考输入带上,并声明以局部小结为准。
    const std::string reduce_body =
        std::get<api::TextBlock>(backend.captured_requests.back().messages[0].content[0]).text;
    CHECK(reduce_body.find("上一轮的存档正文") != std::string::npos);
    CHECK(backend.captured_requests.back().system.find("只当参考") != std::string::npos);
}

TEST_CASE("CompactHierarchical: 整份历史都在热区(单轮巨型)→ 交给单次压缩明确拒绝") {
    ScriptedBackend backend;
    std::vector<api::Message> history;
    history.push_back(UserText(std::string(20000, 'x')));

    const auto result = agent::CompactHierarchical(backend, "test-model", history, SmallWindowOptions());

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("装不下") != std::string::npos);
    CHECK(backend.captured_requests.empty());
}

// ---------------------------------------------------------------------------
// 第四期观测钩子(只记不用):implementation 标注与 source_digest 指纹。
// source_digest 是将来"episode 关闭后台预计算局部摘要、正式触发按 digest
// 复用"的失效判据——同史必同值,改一字必变,这条钉死了才配当判据。
// 近重复(MinHash/SimHash)与 embedding 召回按单子不做实现,只在
// docs/sessions-and-context.md 观测账一节留了口。
// ---------------------------------------------------------------------------

TEST_CASE("观测钩子: source_digest 同史同值、改一字即变;implementation 分层与否各标各的") {
    // 单次路(local-single)。
    {
        ScriptedBackend backend;
        backend.scripts.push_back(SummaryScript(kGoodSummary));
        std::vector<api::Message> history;
        history.push_back(UserText("小历史 " + std::string(2400, 'x')));
        const auto result = agent::CompactHierarchical(backend, "test-model", history, agent::CompactOptions{});
        REQUIRE(result.has_value());
        CHECK(result->metrics.implementation == "local-single");
        CHECK_FALSE(result->metrics.source_digest.empty());
        // 同一份历史再压一遍,digest 一致。
        ScriptedBackend again;
        again.scripts.push_back(SummaryScript(kGoodSummary));
        const auto second = agent::CompactHierarchical(again, "test-model", history, agent::CompactOptions{});
        REQUIRE(second.has_value());
        CHECK(second->metrics.source_digest == result->metrics.source_digest);
    }
    // 分层路(local-hierarchical):digest 跟着历史内容走。
    {
        ScriptedBackend backend;
        backend.scripts.push_back(TextScript(EpisodeText("壹")));
        backend.scripts.push_back(TextScript(EpisodeText("贰")));
        backend.scripts.push_back(TextScript(EpisodeText("叁")));
        backend.scripts.push_back(TextScript(ReduceText()));
        const auto history = BigHistory();
        const auto result = agent::CompactHierarchical(backend, "test-model", history, SmallWindowOptions());
        REQUIRE(result.has_value());
        CHECK(result->metrics.implementation == "local-hierarchical");
        const std::string digest_before = result->metrics.source_digest;
        CHECK_FALSE(digest_before.empty());

        // 改一字再压:digest 必变(复用判据的失效信号)。
        std::vector<api::Message> changed = BigHistory();
        std::get<api::TextBlock>(changed[0].content[0]).text += "X";
        ScriptedBackend backend2;
        backend2.scripts.push_back(TextScript(EpisodeText("壹")));
        backend2.scripts.push_back(TextScript(EpisodeText("贰")));
        backend2.scripts.push_back(TextScript(EpisodeText("叁")));
        backend2.scripts.push_back(TextScript(ReduceText()));
        const auto changed_result = agent::CompactHierarchical(backend2, "test-model", changed, SmallWindowOptions());
        REQUIRE(changed_result.has_value());
        CHECK(changed_result->metrics.source_digest != digest_before);
    }
}

// ---------------------------------------------------------------------------
// 公共 turn 切分(Compact 四分区单·阶段 0,§二):IsUserTurnStart /
// SplitIntoTurns 收拢在 agent/context.hpp,compact/context/context_events
// 共用同一只。
// ---------------------------------------------------------------------------

TEST_CASE("IsUserTurnStart: ToolResult user 不开 turn,图片 user 开 turn") {
    CHECK(agent::IsUserTurnStart(UserText("真正的问题")));
    api::Message image_message;
    image_message.role = api::Role::User;
    image_message.content.push_back(api::ImageBlock{"png", "...bytes...", "", 640, 480});
    CHECK(agent::IsUserTurnStart(image_message));
    // 只带 ToolResultBlock 的 user 消息是工具回填,不是新 turn。
    CHECK_FALSE(agent::IsUserTurnStart(UserToolResult("tool_1", "结果")));
    // assistant 归当前 turn;空内容 user 不凭空开 turn(§二:没有 text/image
    // 就没有"用户说了话"的证据)。
    CHECK_FALSE(agent::IsUserTurnStart(AssistantText("回答")));
    api::Message empty_user;
    empty_user.role = api::Role::User;
    CHECK_FALSE(agent::IsUserTurnStart(empty_user));
}

TEST_CASE("SplitIntoTurns: 区间连续盖满,末段到尾;没有用户输入给空表") {
    std::vector<api::Message> history;
    history.push_back(UserText("第一问"));
    history.push_back(AssistantToolUse("t1", "read_file"));
    history.push_back(UserToolResult("t1", "结果"));  // 不开新 turn
    history.push_back(AssistantText("答一"));
    history.push_back(UserText("第二问"));
    history.push_back(AssistantToolUse("t2", "search"));
    history.push_back(UserToolResult("t2", "搜索结果"));
    history.push_back(AssistantText("答二"));

    const auto turns = agent::SplitIntoTurns(history);
    REQUIRE(turns.size() == 2);
    CHECK(turns[0] == std::make_pair(std::size_t{0}, std::size_t{4}));
    CHECK(turns[1] == std::make_pair(std::size_t{4}, std::size_t{8}));

    // 没有任何真正用户输入:空表。
    std::vector<api::Message> results_only;
    results_only.push_back(UserToolResult("t9", "孤立结果"));
    CHECK(agent::SplitIntoTurns(results_only).empty());
}

// ---------------------------------------------------------------------------
// BuildTurnPartitionPlan(Compact 四分区单·阶段 1):纯计算,不调模型。
// ---------------------------------------------------------------------------

namespace {

// n 枚等重 turn:每轮 user 文本 + assistant 文本,token 大致相同。
std::vector<api::Message> UniformTurns(std::size_t turn_count) {
    std::vector<api::Message> history;
    for (std::size_t i = 0; i < turn_count; ++i) {
        history.push_back(UserText("第 " + std::to_string(i) + " 问 " + std::string(400, 'u')));
        history.push_back(AssistantText("第 " + std::to_string(i) + " 答 " + std::string(400, 'a')));
    }
    return history;
}

// 分区结构不变量:连续盖满全部 turn、每份至少一枚、末份热区、冷区份数与
// map_calls 对账、分区 token 加总与全量对账。
void CheckPartitionInvariants(const agent::TurnPartitionPlan& plan) {
    REQUIRE_FALSE(plan.partitions.empty());
    CHECK(plan.partitions.front().first_turn == 0);
    CHECK(plan.partitions.back().last_turn == plan.turns.size());
    for (std::size_t p = 0; p < plan.partitions.size(); ++p) {
        CHECK(plan.partitions[p].first_turn < plan.partitions[p].last_turn);  // 每份至少一枚 turn
        if (p > 0) {
            CHECK(plan.partitions[p].first_turn == plan.partitions[p - 1].last_turn);  // 首尾相接
        }
    }
    CHECK(plan.partitions.back().is_hot);
    for (std::size_t p = 0; p + 1 < plan.partitions.size(); ++p) {
        CHECK_FALSE(plan.partitions[p].is_hot);
    }
    CHECK(plan.map_calls == plan.partitions.size() - 1);
    std::size_t sum = 0;
    for (const auto& partition : plan.partitions) {
        sum += partition.working_tokens;
    }
    CHECK(sum == plan.total_working_tokens);
}

}  // namespace

TEST_CASE("BuildTurnPartitionPlan: 17 枚等重 turn 默认四分,前三份 map,末份热区") {
    const auto plan = agent::BuildTurnPartitionPlan(UniformTurns(17), 4, agent::TurnPartitionBudgets{});
    REQUIRE(plan.turns.size() == 17);
    CHECK(plan.turns[0].id == "t1");
    CHECK(plan.turns[16].id == "t17");
    CheckPartitionInvariants(plan);
    CHECK(plan.partitions.size() == 4);
    CHECK(plan.map_calls == 3);
    // 等重轮按 token 大致四等分:每份 4~5 枚 turn(17 = 4+4+4+5 一类切法)。
    for (const auto& partition : plan.partitions) {
        const std::size_t count = partition.last_turn - partition.first_turn;
        CHECK(count >= 4);
        CHECK(count <= 5);
    }
    CHECK_FALSE(plan.has_prior_archive);
    CHECK(plan.WorthCompacting());
}

TEST_CASE("BuildTurnPartitionPlan: 0/1/2/3 枚 turn 的边界") {
    // 空 history:没有 turn,没有分区。
    {
        const auto plan = agent::BuildTurnPartitionPlan({}, 4, agent::TurnPartitionBudgets{});
        CHECK(plan.turns.empty());
        CHECK(plan.partitions.empty());
        CHECK(plan.map_calls == 0);
        CHECK_FALSE(plan.WorthCompacting());
    }
    // 1 枚:只有热区,没有冷区,无收益(§9.2/§9.3)。
    {
        const auto plan = agent::BuildTurnPartitionPlan(UniformTurns(1), 4, agent::TurnPartitionBudgets{});
        CheckPartitionInvariants(plan);
        CHECK(plan.partitions.size() == 1);
        CHECK(plan.partitions.front().is_hot);
        CHECK(plan.map_calls == 0);
        CHECK_FALSE(plan.WorthCompacting());
    }
    // 2、3 枚:min(turn, 4) 份,末份热区,其余各 map 一次。
    for (std::size_t turns : {std::size_t{2}, std::size_t{3}}) {
        const auto plan = agent::BuildTurnPartitionPlan(UniformTurns(turns), 4, agent::TurnPartitionBudgets{});
        CheckPartitionInvariants(plan);
        CHECK(plan.partitions.size() == turns);
        CHECK(plan.map_calls == turns - 1);
    }
}

TEST_CASE("BuildTurnPartitionPlan: 自定义 partition_count 2/3/5/8;turn 不够取 min") {
    const std::size_t turn_count = 12;
    for (std::size_t wanted : {std::size_t{2}, std::size_t{3}, std::size_t{5}, std::size_t{8}}) {
        const auto plan = agent::BuildTurnPartitionPlan(UniformTurns(turn_count), wanted, agent::TurnPartitionBudgets{});
        CheckPartitionInvariants(plan);
        CHECK(plan.partitions.size() == wanted);
        CHECK(plan.map_calls == wanted - 1);
    }
    // turn 数 6、7 少于 8 份:实际分区数取 min(6|7, 8)。
    for (std::size_t turns : {std::size_t{6}, std::size_t{7}}) {
        const auto plan = agent::BuildTurnPartitionPlan(UniformTurns(turns), 8, agent::TurnPartitionBudgets{});
        CheckPartitionInvariants(plan);
        CHECK(plan.partitions.size() == turns);
    }
}

TEST_CASE("BuildTurnPartitionPlan: tool-heavy turn 按工作视图 token 挪边界,不按 turn 数硬平分") {
    // 16 枚轻 turn,T4 里塞一组巨大的工具来回(60k ASCII ≈ 15k token,约
    // 占全史四分之三):按枚数平分必失衡,token 平衡器得提前收口 P1。
    std::vector<api::Message> history = UniformTurns(16);
    const std::string huge_result(60000, 'r');
    history.insert(history.begin() + 8, AssistantToolUse("big_1", "run_command"));  // 落进 T4
    history.insert(history.begin() + 9, UserToolResult("big_1", huge_result));

    const auto plan = agent::BuildTurnPartitionPlan(history, 4, agent::TurnPartitionBudgets{});
    CheckPartitionInvariants(plan);
    CHECK(plan.partitions.size() == 4);
    // T4 自己超过四分之一重量:P1 收不满 4 枚便收口。
    const std::size_t p0_count = plan.partitions[0].last_turn - plan.partitions[0].first_turn;
    CHECK(p0_count < 4);
    // 巨轮整枚落在一个分区里(轮界即组界,不劈开)。
    const std::size_t heavy_turn = 3;  // T4,0 起
    std::size_t owner = plan.partitions.size();
    for (std::size_t p = 0; p < plan.partitions.size(); ++p) {
        if (plan.partitions[p].first_turn <= heavy_turn && heavy_turn < plan.partitions[p].last_turn) {
            owner = p;
        }
    }
    REQUIRE(owner < plan.partitions.size());
}

TEST_CASE("BuildTurnPartitionPlan: 并行工具按 tool_use_id 收成一组,组不跨分区") {
    // 末轮一条 assistant 消息并行发两枚 tool_use,results 分两条 user 消息
    // 回来——§6.1:按 id 收齐整组,不按"下一条 user 消息"猜配对。
    std::vector<api::Message> history = UniformTurns(6);
    api::Message parallel;
    parallel.role = api::Role::Assistant;
    parallel.content.push_back(api::ToolUseBlock{"p1", "read_file", nlohmann::json::object()});
    parallel.content.push_back(api::ToolUseBlock{"p2", "search", nlohmann::json::object()});
    history.push_back(parallel);
    history.push_back(UserToolResult("p1", "结果一"));
    history.push_back(UserToolResult("p2", "结果二"));

    const auto plan = agent::BuildTurnPartitionPlan(history, 3, agent::TurnPartitionBudgets{});
    CheckPartitionInvariants(plan);
    const agent::ToolExchangeGroupInfo* group = nullptr;
    for (const auto& candidate : plan.tool_groups) {
        if (candidate.tool_use_ids.size() == 2) {
            group = &candidate;
        }
    }
    REQUIRE(group != nullptr);
    CHECK(group->complete);
    CHECK(group->to_message == history.size());  // 区间盖到最后一条 result(+1)
    CHECK_FALSE(plan.has_incomplete_tool_exchange);
    // 组不跨分区:组的消息区间整枚落在所属 turn 里,turn 整枚落在一个分区。
    const agent::TurnInfo& turn = plan.turns[group->turn];
    CHECK(group->from_message >= turn.from_message);
    CHECK(group->to_message <= turn.to_message);
    bool owned_by_one_partition = false;
    for (const auto& partition : plan.partitions) {
        if (partition.first_turn <= group->turn && group->turn < partition.last_turn) {
            owned_by_one_partition = true;
        }
    }
    CHECK(owned_by_one_partition);
}

TEST_CASE("BuildTurnPartitionPlan: orphan tool_use 与悬空 result 都点名,不静默") {
    std::vector<api::Message> history = UniformTurns(3);
    history.push_back(AssistantToolUse("orphan_1", "read_file"));  // 没有回 result
    history.push_back(UserToolResult("dangling_1", "没人认领"));    // 没有对应 use

    const auto plan = agent::BuildTurnPartitionPlan(history, 2, agent::TurnPartitionBudgets{});
    CheckPartitionInvariants(plan);
    CHECK(plan.has_incomplete_tool_exchange);
    CHECK(plan.dangling_results == 1);
    bool saw_incomplete = false;
    for (const auto& candidate : plan.tool_groups) {
        if (!candidate.complete) {
            saw_incomplete = true;
        }
    }
    CHECK(saw_incomplete);
}

TEST_CASE("BuildTurnPartitionPlan: 旧存档剥出不算 turn,不占分区账") {
    std::vector<api::Message> history = UniformTurns(8);
    // 首条 user 文本并上旧存档(BuildCompactedHistory 的产出形状)。
    std::get<api::TextBlock>(history[0].content[0]).text =
        "[对话存档,此前内容已压缩] 旧存档正文,不算 turn。\n"
        "```json\n{\"goal\": \"旧目标\", \"constraints\": [], \"open_items\": [], \"next_action\": \"旧的下一步\"}\n```\n\n" +
        std::get<api::TextBlock>(history[0].content[0]).text;

    const auto plan = agent::BuildTurnPartitionPlan(history, 4, agent::TurnPartitionBudgets{});
    CHECK(plan.has_prior_archive);
    CHECK(plan.prior_archive_tokens > 0);
    CHECK_FALSE(plan.prior_archive_text.empty());
    // turn 还是 8 枚:旧存档没有多出一枚 turn。
    REQUIRE(plan.turns.size() == 8);
    CheckPartitionInvariants(plan);
    CHECK(plan.partitions.size() == 4);
    CHECK(plan.map_calls == 3);
    // 首 turn 的账精确扣掉了旧存档:turn0 + 存档 = 原两条消息的全量口径。
    CHECK(plan.turns[0].raw_tokens + plan.prior_archive_tokens ==
          agent::EstimateMessageTokens(history[0]) + agent::EstimateMessageTokens(history[1]));
}

TEST_CASE("BuildTurnPartitionPlan: 长 ToolResult 外置后计量,外置账点名;原文不动") {
    std::vector<api::Message> history = UniformTurns(4);
    // 末轮一枚超长 read_file 结果(默认 long_result_bytes=8192):工作视图
    // 换 artifact 预览,分区按外置后的重量算,原 history 全文一字不动。
    const std::string huge = std::string(20000, 'h');
    history.push_back(AssistantToolUse("big_read", "read_file"));
    history.push_back(UserToolResult("big_read", huge));

    const auto plan = agent::BuildTurnPartitionPlan(history, 2, agent::TurnPartitionBudgets{});
    CheckPartitionInvariants(plan);
    CHECK(plan.externalized_results == 1);
    CHECK(plan.turns[3].externalized_results == 1);  // 末 turn(0 起 turn 3)里那枚
    CHECK(plan.total_working_tokens < plan.total_raw_tokens);
    // 纯函数不改入参。
    bool original_intact = false;
    for (const auto& message : history) {
        for (const auto& block : message.content) {
            if (const auto* result = std::get_if<api::ToolResultBlock>(&block);
                result != nullptr && result->tool_use_id == "big_read") {
                original_intact = result->content.size() == huge.size();
            }
        }
    }
    CHECK(original_intact);
}

TEST_CASE("BuildTurnPartitionPlan: 预算诊断——分区/单 turn 超 compact 模型预算点名") {
    const std::vector<api::Message> history = UniformTurns(8);
    // 窗口未知:不校验,不假装核过。
    {
        const auto plan = agent::BuildTurnPartitionPlan(history, 4, agent::TurnPartitionBudgets{});
        CHECK_FALSE(plan.compact_input_budget.has_value());
        CHECK_FALSE(plan.any_partition_over_map_budget);
        CHECK_FALSE(plan.any_turn_over_map_budget);
    }
    // 窗口给得极小(连输出预留都盖不住 → 预算 0):单 turn 也超限必须点名
    // (§3.4:该次 compact 须明确拒绝,不截半条用户输入)。
    {
        agent::TurnPartitionBudgets budgets;
        budgets.compact_model.window_tokens = 64;
        const auto plan = agent::BuildTurnPartitionPlan(history, 4, budgets);
        REQUIRE(plan.compact_input_budget.has_value());
        CHECK(plan.any_turn_over_map_budget);
        CHECK(plan.any_partition_over_map_budget);
    }
    // 窗口装得下单份:无超限。
    {
        agent::TurnPartitionBudgets budgets;
        budgets.compact_model.window_tokens = 1000000;
        const auto plan = agent::BuildTurnPartitionPlan(history, 4, budgets);
        REQUIRE(plan.compact_input_budget.has_value());
        CHECK_FALSE(plan.any_turn_over_map_budget);
        CHECK_FALSE(plan.any_partition_over_map_budget);
    }
}

// ---------------------------------------------------------------------------
// 四分区·阶段 2-4:TurnGroupSummary map、双账 final reduce、新 history。
// 全部走假后端(按 system 内容分流 map/reduce),不碰真网络。
// ---------------------------------------------------------------------------

namespace {

// 双账后端:system 含"两份总账" = reduce,吃 reduce_response;含"局部小结"
// = map,吃 map_responses 队(用尽或 fail_map_at 命中即失败)。
class DualLedgerBackend : public api::Backend {
public:
    std::deque<std::string> map_responses;
    std::string reduce_response;
    int fail_map_at = -1;  // 第几次 map(1 起)直接断流;<0 不失败
    std::vector<api::Request> captured_requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
        captured_requests.push_back(request);
        if (request.system.find("两份总账") != std::string::npos) {
            if (reduce_response.empty()) {
                return std::unexpected(api::Error{api::ErrorKind::Api, "reduce 脚本未备", 0});
            }
            for (const auto& event : TextScript(reduce_response)) {
                on_event(event);
            }
            return {};
        }
        if (fail_map_at == static_cast<int>(captured_requests.size())) {
            on_event(api::MessageStart{"msg", "model"});
            on_event(api::StreamError{"map 中途断流"});
            return {};
        }
        if (map_responses.empty()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "map 脚本用尽", 0});
        }
        const std::string text = map_responses.front();
        map_responses.pop_front();
        for (const auto& event : TextScript(text)) {
            on_event(event);
        }
        return {};
    }
};

// 一份合法的严格 JSON TurnGroupSummary(map 产物)。
std::string MapJsonText(const std::string& tag) {
    return "{\"user_requirement_changes\": [\"" + tag + " 的要求\"], \"confirmed_facts\": [\"" + tag +
           " 证实的事\"], \"tool_results\": [{\"tool\": \"read_file\", \"result\": \"" + tag +
           " 读到关键内容\", \"evidence\": \"t1:e1\"}], \"files\": [\"src/" + tag + ".cpp\"], \"changes_made\": [\"改了 " +
           tag + "\"], \"failed_attempts\": [], \"open_items\": [\"" + tag + " 收尾\"], \"next_step_candidates\": [\"继续\"]}";
}

// 双账 JSON(reduce 产物)的可定制脚本:正常形与坏形共用一份拼装。默认
// 形引用到 t10——用默认脚本的夹具至少要 10 turn。
struct DualLedgerScript {
    std::string goal_turn = "t1";
    std::string constraint_turn = "t3";
    std::string acceptance_turn = "t7";
    std::string addition_turn = "t8";
    std::string open_question_turn = "t9";
    std::string superseded_source_turn = "t2";
    std::string superseded_at_turn = "t8";
    std::string fact_evidence = "t4:e1";
    std::string tool_evidence = "t6:e2";
    std::string change_evidence = "t10";
    std::string failed_evidence = "t1:e4";
    std::string open_items_json = R"(["收尾一","收尾二"])";
    std::string goal_text = "实现上下文管理与压缩";
    std::string constraint_text = "只修改 compact,不改 project memory";
    bool no_superseded = false;  // 单 turn 档:没有旧 turn 可废旧,置空该栏
    // 坏形注入口:非空则替换对应片段。
    std::string goal_source_override;
    std::string superseded_at_override;
    std::string superseded_by_override;
    std::string fact_evidence_override;
    std::string extra_superseded;
    std::string extra_constraint;
};

std::string DualJsonText(const DualLedgerScript& script) {
    const std::string goal_turns =
        !script.goal_source_override.empty() ? script.goal_source_override : "[\"" + script.goal_turn + "\"]";
    const std::string superseded_at =
        !script.superseded_at_override.empty() ? script.superseded_at_override : script.superseded_at_turn;
    const std::string superseded_by =
        !script.superseded_by_override.empty() ? script.superseded_by_override : "\"r3\"";
    const std::string fact_refs = !script.fact_evidence_override.empty() ? script.fact_evidence_override
                                                                        : "[\"" + script.fact_evidence + "\"]";
    std::string constraints = "[{\"id\": \"r1\", \"text\": \"" + script.constraint_text + "\", \"source_turns\": [\"" +
                              script.constraint_turn + "\"]}]";
    if (!script.extra_constraint.empty()) {
        constraints.pop_back();  // 去掉尾 ]
        constraints += ", " + script.extra_constraint + "]";
    }
    std::string superseded = "[{\"id\": \"r0\", \"text\": \"旧要求原文\", \"source_turns\": [\"" +
                             script.superseded_source_turn + "\"], \"superseded_by\": " + superseded_by +
                             ", \"superseded_at_turn\": \"" + superseded_at + "\"}]";
    if (script.no_superseded) {
        superseded = "[]";
    } else if (!script.extra_superseded.empty()) {
        superseded.pop_back();
        superseded += ", " + script.extra_superseded + "]";
    }
    return "{\"user_contract\": {\"goal\": {\"text\": \"" + script.goal_text + "\", \"source_turns\": " + goal_turns +
           "}, \"active_constraints\": " + constraints +
           ", \"acceptance_criteria\": [{\"id\": \"r2\", \"text\": \"第四分区保留最近 turns 的原始消息形状\", "
           "\"source_turns\": [\"" +
           script.acceptance_turn + "\"]}], \"additions\": [{\"id\": \"r3\", \"text\": \"全部 turns 按 token 重量四分\", "
           "\"source_turns\": [\"" +
           script.addition_turn + "\"]}], \"superseded_requirements\": " + superseded +
           ", \"open_questions\": [{\"text\": \"配置是否允许项目级覆盖\", \"source_turns\": [\"" +
           script.open_question_turn + "\"]}]}, "
           "\"work_state\": {\"confirmed_facts\": [{\"text\": \"自动 compact 在 80% 触发\", \"evidence_refs\": " +
           fact_refs +
           "}], \"tool_results\": [{\"tool\": \"ctest\", \"result\": \"6 项相关测试通过\", \"evidence_refs\": [\"" +
           script.tool_evidence + "\"]}], \"files\": [\"src/agent/compact.cpp\"], \"changes_made\": [{\"text\": "
           "\"已接入双账压缩\", \"evidence_refs\": [\"" +
           script.change_evidence + "\"]}], \"failed_attempts\": [{\"text\": \"某次摘要反涨已拒收\", "
           "\"evidence_refs\": [\"" +
           script.failed_evidence + "\"]}], \"open_items\": " + script.open_items_json +
           ", \"next_action\": \"先统一 turn 切分函数\"}}";
}

// 备好 N 份 map 脚本 + 一份 reduce 脚本的后端。
DualLedgerBackend ReadyBackend(std::size_t map_count, const DualLedgerScript& script = DualLedgerScript{}) {
    DualLedgerBackend backend;
    for (std::size_t i = 0; i < map_count; ++i) {
        backend.map_responses.push_back(MapJsonText("段" + std::to_string(i + 1)));
    }
    backend.reduce_response = DualJsonText(script);
    return backend;
}

}  // namespace

TEST_CASE("ParseTurnGroupSummary: 只收严格 JSON,Markdown/缺键全拒") {
    const std::string good = MapJsonText("甲");
    REQUIRE(agent::ParseTurnGroupSummary(good).has_value());
    // 围栏包裹也认(模型手滑加围栏;内容仍须是合法 JSON)。
    REQUIRE(agent::ParseTurnGroupSummary("```json\n" + good + "\n```").has_value());
    // Markdown 散文(旧 episode 路的产出形状)不再收。
    CHECK_FALSE(agent::ParseTurnGroupSummary("## 阶段目标\n一些散文\n## 未完成事项\n收尾").has_value());
    // 缺一个键。
    std::string missing_key = good;
    const std::size_t pos = missing_key.find("\"failed_attempts\"");
    missing_key.replace(pos, 19, "\"failed_attempt_x\"");
    CHECK_FALSE(agent::ParseTurnGroupSummary(missing_key).has_value());
    // 元素类型不对。
    CHECK_FALSE(agent::ParseTurnGroupSummary(
        "{\"user_requirement_changes\": [1], \"confirmed_facts\": [], \"tool_results\": [], \"files\": [], "
        "\"changes_made\": [], \"failed_attempts\": [], \"open_items\": [], \"next_step_candidates\": []}")
                    .has_value());
    // 空串元素。
    CHECK_FALSE(agent::ParseTurnGroupSummary(
        "{\"user_requirement_changes\": [\" \"], \"confirmed_facts\": [], \"tool_results\": [], \"files\": [], "
        "\"changes_made\": [], \"failed_attempts\": [], \"open_items\": [], \"next_step_candidates\": []}")
                    .has_value());
}

TEST_CASE("CompactTurnPartitioned: 17 枚等重 turn 固定 3 次 map + 1 次 reduce") {
    const std::vector<api::Message> history = UniformTurns(17);
    const auto plan = agent::BuildTurnPartitionPlan(history, 4, agent::TurnPartitionBudgets{});
    REQUIRE(plan.partitions.size() == 4);
    REQUIRE(plan.map_calls == 3);

    DualLedgerBackend backend = ReadyBackend(3);
    agent::CompactOptions options;  // 窗口未知:不拦,照跑
    const auto result = agent::CompactTurnPartitioned(backend, "test-model", history, options,
                                                      agent::StructuralCompressionOptions{});
    REQUIRE(result.has_value());

    // 请求形状:恰好 3 map + 1 reduce(预算内固定 N-1 次 map)。
    REQUIRE(backend.captured_requests.size() == 4);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(backend.captured_requests[i].system.find("局部小结") != std::string::npos);
    }
    CHECK(backend.captured_requests.back().system.find("两份总账") != std::string::npos);

    // 宿主钉 turn 范围:map 指令里的来源 turn 与 plan 分区逐一对上。
    REQUIRE(result->group_summaries.size() == 3);
    for (std::size_t p = 0; p + 1 < plan.partitions.size(); ++p) {
        const std::string& instruction = backend.captured_requests[p].system;
        const std::string first = plan.turns[plan.partitions[p].first_turn].id;
        const std::string last = plan.turns[plan.partitions[p].last_turn - 1].id;
        CHECK(instruction.find("来源 turn " + first + "-" + last) != std::string::npos);
        CHECK(result->group_summaries[p].turn_range == first + "-" + last);
    }

    // 指标与分区账。
    CHECK(result->metrics.chunks == 3);
    CHECK(result->metrics.schema == "dual-ledger-v1");
    CHECK(result->metrics.implementation == "turn-partition");
    CHECK(result->metrics.partition_count == 4);
    CHECK(result->metrics.total_turns == 17);
    CHECK(result->metrics.hot_turns ==
          plan.partitions.back().last_turn - plan.partitions.back().first_turn);

    // 兼容面:双账折算成旧 manifest。
    CHECK(result->manifest.goal == "实现上下文管理与压缩");
    REQUIRE(result->manifest.constraints.size() == 1);
    CHECK(result->manifest.constraints[0] == "只修改 compact,不改 project memory");
}

TEST_CASE("CompactTurnPartitioned: map 输入不吃热区 turn;reduce 吃热区原文与全部小结") {
    const std::vector<api::Message> history = UniformTurns(12);
    const auto plan = agent::BuildTurnPartitionPlan(history, 4, agent::TurnPartitionBudgets{});

    DualLedgerBackend backend = ReadyBackend(3);
    const auto result = agent::CompactTurnPartitioned(backend, "test-model", history, agent::CompactOptions{},
                                                      agent::StructuralCompressionOptions{});
    REQUIRE(result.has_value());
    REQUIRE(backend.captured_requests.size() == 4);

    // 热区 turn 的正文绝不出现在任何 map 请求里。
    const std::size_t hot_first_turn = plan.partitions.back().first_turn;
    for (std::size_t i = 0; i + 1 < backend.captured_requests.size(); ++i) {
        for (const auto& message : backend.captured_requests[i].messages) {
            for (const auto& block : message.content) {
                if (const auto* text = std::get_if<api::TextBlock>(&block); text != nullptr) {
                    for (std::size_t t = hot_first_turn; t < plan.turns.size(); ++t) {
                        CHECK(text->text.find("第 " + std::to_string(t) + " 问") == std::string::npos);
                    }
                }
            }
        }
    }
    // reduce 输入带热区原文与全部小结。
    const std::string reduce_body =
        std::get<api::TextBlock>(backend.captured_requests.back().messages[0].content[0]).text;
    CHECK(reduce_body.find("最近热区原文") != std::string::npos);
    CHECK(reduce_body.find("局部小结 1/3") != std::string::npos);
    CHECK(reduce_body.find("段1 的要求") != std::string::npos);
    CHECK(reduce_body.find("段3 的要求") != std::string::npos);
}

TEST_CASE("CompactTurnPartitioned: 工具重载夹具按 token 挪边界,原子组无一拆开") {
    // 16 枚轻 turn,T4 塞一组巨大工具来回(工具重载夹具)。
    std::vector<api::Message> history = UniformTurns(16);
    const std::string huge_result(60000, 'r');
    history.insert(history.begin() + 8, AssistantToolUse("big_1", "run_command"));  // 落进 T4
    history.insert(history.begin() + 9, UserToolResult("big_1", huge_result));

    const auto plan = agent::BuildTurnPartitionPlan(history, 4, agent::TurnPartitionBudgets{});
    DualLedgerBackend backend = ReadyBackend(3);
    const auto result = agent::CompactTurnPartitioned(backend, "test-model", history, agent::CompactOptions{},
                                                      agent::StructuralCompressionOptions{});
    REQUIRE(result.has_value());
    REQUIRE(backend.captured_requests.size() == 4);

    // 每个 map 块内 tool_use/tool_result 全配对(原子组没被任何一块劈开)。
    for (std::size_t i = 0; i + 1 < backend.captured_requests.size(); ++i) {
        std::map<std::string, bool> pairs;
        bool dangling_result = false;
        for (const auto& message : backend.captured_requests[i].messages) {
            for (const auto& block : message.content) {
                if (const auto* use = std::get_if<api::ToolUseBlock>(&block); use != nullptr) {
                    pairs[use->id] = false;
                } else if (const auto* tool_result = std::get_if<api::ToolResultBlock>(&block);
                           tool_result != nullptr) {
                    if (pairs.count(tool_result->tool_use_id) == 0) {
                        dangling_result = true;
                    } else {
                        pairs[tool_result->tool_use_id] = true;
                    }
                }
            }
        }
        CHECK_FALSE(dangling_result);
        for (const auto& [id, matched] : pairs) {
            (void)id;
            CHECK(matched);
        }
    }
    // 巨轮整枚落在一个分区里(工具组没跨区)。
    bool big_turn_single_partition = false;
    for (const auto& partition : plan.partitions) {
        if (partition.first_turn <= 3 && 3 < partition.last_turn) {
            big_turn_single_partition = true;
        }
    }
    CHECK(big_turn_single_partition);
}

TEST_CASE("CompactTurnPartitioned: 单分区超预算只递归拆该分区,map 次数可多于 3") {
    // UniformTurns 每轮约 200 token;窗口给 900(输出预留 100、协议 0):
    // 单枚 turn 装得下,两轮的分区超预算必须沿 turn 再切。
    const std::vector<api::Message> history = UniformTurns(8);
    agent::CompactOptions options;
    options.budget.window_tokens = 900;
    options.budget.output_reserve_tokens = 100;
    options.budget.protocol_headroom_tokens = 0;

    agent::TurnPartitionBudgets budgets;
    budgets.compact_model = options.budget;
    const auto plan = agent::BuildTurnPartitionPlan(history, 4, budgets);
    CHECK(plan.any_partition_over_map_budget);
    CHECK_FALSE(plan.any_turn_over_map_budget);

    DualLedgerScript script;  // 8 turn 档:引用全部落在 t1..t8
    script.acceptance_turn = "t6";
    script.addition_turn = "t7";
    script.open_question_turn = "t8";
    script.change_evidence = "t8";
    // 窗口 800 时 reduce 指令本身超预算:§9.4 会先两两归并 summaries
    // (6→3→2→1 的归并请求与 map 同吃 map 脚本队列)再终稿 reduce——脚本
    // 多备 7 份。
    DualLedgerBackend backend = ReadyBackend(13, script);
    const auto result = agent::CompactTurnPartitioned(backend, "test-model", history, options,
                                                      agent::StructuralCompressionOptions{});
    REQUIRE(result.has_value());
    // 冷区只有 3 份(末份是热区,不 map):每份 2 turn 再切成单 turn 一块
    // → map 块数 6 > 分区数-1 = 3。
    CHECK(result->metrics.chunks == 6);
    CHECK(result->metrics.reduce_passes == 3);  // 6→3→2→1
    // map 指令(system 带"局部小结"且钉"来源 turn")的请求恰好 8 次,再切
    // 出来的块都是单 turn 范围(tN,无 '-')。
    int single_turn_maps = 0;
    for (const auto& request : backend.captured_requests) {
        if (request.system.find("局部小结") != std::string::npos &&
            request.system.find("来源 turn t") != std::string::npos) {
            const std::size_t pin = request.system.find("来源 turn ");
            const std::size_t line_end = request.system.find('\n', pin);
            const std::string range = request.system.substr(
                pin + 9, (line_end == std::string::npos ? request.system.size() : line_end) - pin - 9);
            const std::size_t comma = range.find(',');
            const std::string clean = comma == std::string::npos ? range : range.substr(0, comma);
            if (clean.find('-') == std::string::npos) {
                ++single_turn_maps;
            }
        }
    }
    CHECK(single_turn_maps == 6);
}

TEST_CASE("CompactTurnPartitioned: 单 turn 超预算明确拒绝,一次请求都不发") {
    const std::vector<api::Message> history = UniformTurns(4);
    agent::CompactOptions options;
    options.budget.window_tokens = 64;  // 连输出预留都盖不住 → 预算 0

    DualLedgerBackend backend;  // 不备脚本:发了请求就是错
    const auto result = agent::CompactTurnPartitioned(backend, "test-model", history, options,
                                                      agent::StructuralCompressionOptions{});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("单枚 turn 超") != std::string::npos);
    CHECK(backend.captured_requests.empty());
}

TEST_CASE("CompactTurnPartitioned: map 任一块失败,整次失败,旧历史不动") {
    const std::vector<api::Message> history = UniformTurns(17);
    DualLedgerBackend backend = ReadyBackend(3);
    backend.fail_map_at = 2;  // 第 2 次 map 断流

    const auto result = agent::CompactTurnPartitioned(backend, "test-model", history, agent::CompactOptions{},
                                                      agent::StructuralCompressionOptions{});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("map 中途断流") != std::string::npos);
    CHECK(history.size() == 34);  // 原史一字未动
}

TEST_CASE("CompactTurnPartitioned: map 吐 Markdown 而非严格 JSON,整次失败") {
    const std::vector<api::Message> history = UniformTurns(8);
    DualLedgerBackend backend = ReadyBackend(3);
    backend.map_responses[1] = "## 阶段目标\n散文小结,没有 JSON";

    const auto result = agent::CompactTurnPartitioned(backend, "test-model", history, agent::CompactOptions{},
                                                      agent::StructuralCompressionOptions{});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("严格 JSON") != std::string::npos);
}

TEST_CASE("CompactTurnPartitioned: 没有冷区也没有旧档,拒绝空跑") {
    // 单 turn:只有热区,没有旧档 → §9.3 无收益。
    const std::vector<api::Message> history = UniformTurns(1);
    DualLedgerBackend backend = ReadyBackend(1);
    const auto result = agent::CompactTurnPartitioned(backend, "test-model", history, agent::CompactOptions{},
                                                      agent::StructuralCompressionOptions{});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("没有冷区") != std::string::npos);
    CHECK(backend.captured_requests.empty());
}

TEST_CASE("CompactTurnPartitioned: 没有冷区但有旧档,只跑 final reduce") {
    std::vector<api::Message> history = UniformTurns(1);
    // 单 turn 档的 reduce 脚本:全部来源只引 t1,且没有可废旧要求。
    DualLedgerScript single_turn_script;
    single_turn_script.goal_turn = "t1";
    single_turn_script.constraint_turn = "t1";
    single_turn_script.acceptance_turn = "t1";
    single_turn_script.addition_turn = "t1";
    single_turn_script.open_question_turn = "t1";
    single_turn_script.fact_evidence = "t1:e0";
    single_turn_script.tool_evidence = "t1:e1";
    single_turn_script.change_evidence = "t1";
    single_turn_script.failed_evidence = "t1:e1";  // 单 turn 史只有 e0/e1 两枚事件
    single_turn_script.no_superseded = true;
    // 首条并上旧双账(BuildCompactedHistory 的产出形状)。
    std::get<api::TextBlock>(history[0].content[0]).text =
        "[对话存档,此前内容已压缩] 旧双账存档。\n```json\n" + DualJsonText(single_turn_script) +
        "\n```\n\n" + std::get<api::TextBlock>(history[0].content[0]).text;

    DualLedgerBackend backend = ReadyBackend(0, single_turn_script);
    const auto result = agent::CompactTurnPartitioned(backend, "test-model", history, agent::CompactOptions{},
                                                      agent::StructuralCompressionOptions{});
    REQUIRE(result.has_value());
    CHECK(result->metrics.chunks == 0);  // 不跑 map
    REQUIRE(backend.captured_requests.size() == 1);
    CHECK(backend.captured_requests[0].system.find("两份总账") != std::string::npos);
}

TEST_CASE("CompactTurnPartitioned: 热区最后一轮纠正旧约束,新约 active、旧约 superseded") {
    // 8 turn:T2 立旧约束,末 turn(热区)改成新约束。
    std::vector<api::Message> history = UniformTurns(8);
    std::get<api::TextBlock>(history[2].content[0]).text = "约束:注释必须用中文";  // T2 的用户消息
    std::get<api::TextBlock>(history[14].content[0]).text = "改成:注释改用英文";   // T8 的用户消息

    DualLedgerScript script;
    script.constraint_text = "注释改用英文";
    script.constraint_turn = "t8";
    script.superseded_source_turn = "t2";
    script.superseded_at_turn = "t8";
    script.open_question_turn = "t8";   // 8 turn 档:t9 不存在
    script.change_evidence = "t8";
    DualLedgerBackend backend = ReadyBackend(3, script);
    const auto result = agent::CompactTurnPartitioned(backend, "test-model", history, agent::CompactOptions{},
                                                      agent::StructuralCompressionOptions{});
    REQUIRE(result.has_value());

    // 总 UserContract:只有新约束列 active。
    REQUIRE(result->contract.active_constraints.size() == 1);
    CHECK(result->contract.active_constraints[0].text == "注释改用英文");
    REQUIRE(result->contract.active_constraints[0].source_turns.size() == 1);
    CHECK(result->contract.active_constraints[0].source_turns[0] == "t8");
    // 旧约束有来源地列在 superseded。
    REQUIRE(result->contract.superseded_requirements.size() == 1);
    const auto& old = result->contract.superseded_requirements[0];
    CHECK(old.text == "旧要求原文");
    REQUIRE(old.source_turns.size() == 1);
    CHECK(old.source_turns[0] == "t2");
    CHECK(old.superseded_by == "r3");
    CHECK(old.superseded_at_turn == "t8");

    // 热区原文参加了 final reduce:纠正语句在 reduce 输入里。
    const std::string reduce_body =
        std::get<api::TextBlock>(backend.captured_requests.back().messages[0].content[0]).text;
    CHECK(reduce_body.find("改成:注释改用英文") != std::string::npos);
}

TEST_CASE("CompactTurnPartitioned: 新 history = [双账并入热区首条][热区原文],工具对不丢") {
    std::vector<api::Message> history = UniformTurns(10);
    // 末轮带工具来回(热区里的 tool pair 必须原样在)。
    history.push_back(AssistantToolUse("hot_tool", "read_file"));
    history.push_back(UserToolResult("hot_tool", "热区读到的内容"));

    const auto plan = agent::BuildTurnPartitionPlan(history, 4, agent::TurnPartitionBudgets{});
    DualLedgerBackend backend = ReadyBackend(3);
    const auto result = agent::CompactTurnPartitioned(backend, "test-model", history, agent::CompactOptions{},
                                                      agent::StructuralCompressionOptions{});
    REQUIRE(result.has_value());

    const auto& new_history = result->new_history;
    REQUIRE_FALSE(new_history.empty());
    // 首条 = 双账存档并入热区第一条 user 消息:json 围栏与热区首问都在。
    REQUIRE(std::holds_alternative<api::TextBlock>(new_history[0].content[0]));
    const std::string& first_text = std::get<api::TextBlock>(new_history[0].content[0]).text;
    CHECK(first_text.find("对话存档") != std::string::npos);
    CHECK(first_text.find("\"user_contract\"") != std::string::npos);
    CHECK(first_text.find("\"work_state\"") != std::string::npos);
    CHECK(first_text.find("```json") != std::string::npos);
    // 热区原文在后面:末轮工具对原样保留。
    bool hot_tool_use = false;
    bool hot_tool_result = false;
    for (const auto& message : new_history) {
        for (const auto& block : message.content) {
            if (const auto* use = std::get_if<api::ToolUseBlock>(&block);
                use != nullptr && use->id == "hot_tool") {
                hot_tool_use = true;
            }
            if (const auto* tool_result = std::get_if<api::ToolResultBlock>(&block);
                tool_result != nullptr && tool_result->tool_use_id == "hot_tool") {
                hot_tool_result = true;
            }
        }
    }
    CHECK(hot_tool_use);
    CHECK(hot_tool_result);
    // 角色交替:没有相邻两条 user。
    for (std::size_t i = 0; i + 1 < new_history.size(); ++i) {
        const bool both_user = new_history[i].role == api::Role::User && new_history[i + 1].role == api::Role::User;
        CHECK_FALSE(both_user);
    }
    // kept_indices 与新史对账:热区消息下标升序,条数对得上。
    const std::size_t hot_from = plan.turns[plan.partitions.back().first_turn].from_message;
    REQUIRE(result->kept_indices.size() == history.size() - hot_from);
    CHECK(result->kept_indices.front() == hot_from);
    CHECK(result->kept_indices.back() == history.size() - 1);
}

TEST_CASE("CompactTurnPartitioned: 双账 JSON 可从新史首条认回(第二次压缩的基线)") {
    const std::vector<api::Message> history = UniformTurns(12);
    DualLedgerBackend backend = ReadyBackend(3);
    const auto result = agent::CompactTurnPartitioned(backend, "test-model", history, agent::CompactOptions{},
                                                      agent::StructuralCompressionOptions{});
    REQUIRE(result.has_value());

    // 新史首条能被 BuildTurnPartitionPlan 剥出旧档(prior archive 不算 turn)。
    const auto plan2 = agent::BuildTurnPartitionPlan(result->new_history, 4, agent::TurnPartitionBudgets{});
    CHECK(plan2.has_prior_archive);

    // ParsePriorLedgers:新双账认成双账,字段往返无损。
    const auto ledgers = agent::ParsePriorLedgers(
        "[对话存档,此前内容已压缩] 头\n```json\n" +
        nlohmann::json{{"user_contract", agent::ToJson(result->contract)},
                       {"work_state", agent::ToJson(result->state)}}
            .dump() +
        "\n```");
    REQUIRE(ledgers.has_value());
    CHECK(ledgers->dual_ledger);
    CHECK(ledgers->contract.goal.text == result->contract.goal.text);
    CHECK(ledgers->state.next_action == result->state.next_action);

    // 旧 flat manifest 也认:折成 goal-only 基线(legacy 兼容)。
    const auto legacy = agent::ParsePriorLedgers(
        "[对话存档,此前内容已压缩] 旧档\n```json\n{\"goal\": \"旧目标\", \"constraints\": [], \"open_items\": "
        "[\"旧待办\"], \"next_action\": \"旧下一步\"}\n```");
    REQUIRE(legacy.has_value());
    CHECK_FALSE(legacy->dual_ledger);
    CHECK(legacy->contract.goal.text == "旧目标");
    REQUIRE(legacy->state.open_items.size() == 1);
    CHECK(legacy->state.open_items[0] == "旧待办");
}

TEST_CASE("CompactTurnPartitioned: 第二次压缩从旧双账接力,map 不吃旧档,reduce 当基线") {
    std::vector<api::Message> history = UniformTurns(12);
    DualLedgerBackend backend = ReadyBackend(3);
    const auto first = agent::CompactTurnPartitioned(backend, "test-model", history, agent::CompactOptions{},
                                                     agent::StructuralCompressionOptions{});
    REQUIRE(first.has_value());
    // 接着攒 8 个新 turn(旧热区+新 turn 一起重新四分)。
    std::vector<api::Message> grown = first->new_history;
    for (std::size_t i = 0; i < 8; ++i) {
        grown.push_back(UserText("新问 " + std::to_string(i) + " " + std::string(400, 'n')));
        grown.push_back(AssistantText("新答 " + std::to_string(i) + " " + std::string(400, 'm')));
    }

    DualLedgerBackend backend2 = ReadyBackend(3);
    const auto second = agent::CompactTurnPartitioned(backend2, "test-model", grown, agent::CompactOptions{},
                                                      agent::StructuralCompressionOptions{});
    REQUIRE(second.has_value());

    // 旧档只进 reduce 当参考:map 请求里没有旧双账正文,reduce 里有。
    REQUIRE(backend2.captured_requests.size() == 4);
    for (std::size_t i = 0; i + 1 < backend2.captured_requests.size(); ++i) {
        for (const auto& message : backend2.captured_requests[i].messages) {
            for (const auto& block : message.content) {
                if (const auto* text = std::get_if<api::TextBlock>(&block); text != nullptr) {
                    CHECK(text->text.find("user_contract") == std::string::npos);
                }
            }
        }
    }
    const std::string reduce_body =
        std::get<api::TextBlock>(backend2.captured_requests.back().messages[0].content[0]).text;
    CHECK(reduce_body.find("上一轮压缩的总账") != std::string::npos);
    CHECK(reduce_body.find("user_contract") != std::string::npos);
    // 第二次的 plan 认得旧档:不算 turn。
    CHECK(second->plan.has_prior_archive);
    // 旧档没冒出新 turn:turn 数 = 旧热区 turn + 新 8 turn。
    const std::size_t old_hot_turns = first->metrics.hot_turns;
    CHECK(second->metrics.total_turns == old_hot_turns + 8);
}

// ---------------------------------------------------------------------------
// 双账校验:来源、覆盖方向、环、证据、待办守恒。
// ---------------------------------------------------------------------------

namespace {

// 带坏形 reduce 脚本跑一次,断言拒收原因里含 needle(旧历史不动)。
void RequireRejectedWith(const DualLedgerScript& script, const std::string& needle) {
    const std::vector<api::Message> history = UniformTurns(17);
    DualLedgerBackend backend = ReadyBackend(3, script);
    const auto result = agent::CompactTurnPartitioned(backend, "test-model", history, agent::CompactOptions{},
                                                      agent::StructuralCompressionOptions{});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find(needle) != std::string::npos);
    CHECK(history.size() == 34);
}

}  // namespace

TEST_CASE("双账校验: source turn 悬空拒收") {
    DualLedgerScript script;
    script.goal_source_override = R"(["t99"])";
    RequireRejectedWith(script, "t99");
}

TEST_CASE("双账校验: 事件号冒充用户来源,拒收") {
    DualLedgerScript script;
    script.goal_source_override = R"(["e5"])";  // 事件号不是 turn
    RequireRejectedWith(script, "来源");
}

TEST_CASE("双账校验: 倒序覆盖拒收(superseded_at_turn 早于旧要求来源)") {
    DualLedgerScript script;
    script.superseded_at_override = "t1";  // 旧要求来自 t2,t1 比它还早
    RequireRejectedWith(script, "倒序");
}

TEST_CASE("双账校验: superseded_by 指向不存在的 requirement,拒收") {
    DualLedgerScript script;
    script.superseded_by_override = "\"r404\"";
    RequireRejectedWith(script, "不存在的 requirement");
}

TEST_CASE("双账校验: 覆盖关系成环,拒收") {
    DualLedgerScript script;
    // r0 → r5,r5 → r0:方向各自不倒序,但成环。
    script.superseded_by_override = "\"r5\"";
    script.extra_superseded =
        "{\"id\": \"r5\", \"text\": \"另一条旧要求\", \"source_turns\": [\"t3\"], \"superseded_by\": \"r0\", "
        "\"superseded_at_turn\": \"t4\"}";
    RequireRejectedWith(script, "成环");
}

TEST_CASE("双账校验: 已证实事实没带证据,拒收") {
    DualLedgerScript script;
    script.fact_evidence_override = "[]";
    RequireRejectedWith(script, "evidence_refs");
}

TEST_CASE("双账校验: 证据事件号不存在,拒收") {
    DualLedgerScript script;
    script.fact_evidence_override = R"(["t4:e9999"])";
    RequireRejectedWith(script, "证据事件不存在");
}

TEST_CASE("双账校验: 活动待办丢一条,拒收(逐字守恒)") {
    const std::vector<api::Message> history = UniformTurns(17);
    DualLedgerScript script;
    script.open_items_json = R"(["只留一条"])";  // 丢掉"补分块预算"
    DualLedgerBackend backend = ReadyBackend(3, script);
    agent::CompactOptions options;
    options.required_open_items = {"补分块预算", "另一件待办"};
    const auto result = agent::CompactTurnPartitioned(backend, "test-model", history, options,
                                                      agent::StructuralCompressionOptions{});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("补分块预算") != std::string::npos);
    CHECK(history.size() == 34);  // 校验失败旧史不动
}

TEST_CASE("双账校验: 活动约束无来源,拒收") {
    DualLedgerScript script;
    // source_turns 空数组:解析层就判坏(schema 层拒),不给它混进校验层。
    script.extra_constraint = "{\"id\": \"r9\", \"text\": \"没有来源的约束\", \"source_turns\": []}";
    RequireRejectedWith(script, "schema");
}

TEST_CASE("BuildCompactedHistory(plan): 热区整段保留,存档并入首条") {
    const std::vector<api::Message> history = UniformTurns(9);
    const auto plan = agent::BuildTurnPartitionPlan(history, 4, agent::TurnPartitionBudgets{});
    api::Message archive = UserText("[对话存档,此前内容已压缩] 双账正文");
    std::vector<std::size_t> kept;
    const auto new_history = agent::BuildCompactedHistory(history, archive, plan, &kept);
    const std::size_t hot_from = plan.turns[plan.partitions.back().first_turn].from_message;
    REQUIRE(new_history.size() == history.size() - hot_from);
    REQUIRE(kept.size() == new_history.size());
    CHECK(kept.front() == hot_from);
    const std::string& first = std::get<api::TextBlock>(new_history[0].content[0]).text;
    CHECK(first.find("双账正文") != std::string::npos);
}
