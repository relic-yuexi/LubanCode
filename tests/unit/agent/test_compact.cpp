// agent/compact.hpp:BuildCompactedHistory 纯逻辑(archive + 按 token 预算
// 保留的热区,tool_use/tool_result 永远配对)、Compact() 用 FakeBackend 按脚
// 本吐事件(不碰真网络),验证请求怎么拼、失败时怎么处理。0.31.x 分层压缩
// 第一期加钉:压缩模型窗口预算(装不下明确拒绝,不发请求、不截史)、摘要
// 末尾可解析 JSON manifest、待办守恒校验(漏一项拒收,旧历史不动)、热区
// 按 token 预算保留多轮。

#include <doctest/doctest.h>

#include <deque>
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
        "正文里先引用了一枚代码块:\n```json\n{\"goal\": \"旧的\", \"open_items\": [\"旧\"]}\n```\n"
        "存档正文若干字若干字若干字若干字。\n"
        "```json\n{\"goal\": \"新的\", \"open_items\": [\"新\"]}\n```";
    const auto manifest = agent::ParseCompactManifest(text);
    REQUIRE(manifest.has_value());
    CHECK(manifest->goal == "新的");
    REQUIRE(manifest->open_items.size() == 1);
    CHECK(manifest->open_items[0] == "新");
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
    short_terms.reserve(50000);
    for (int i = 0; i < 25000; ++i) short_terms += "a ";
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
