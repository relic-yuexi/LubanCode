// tool_search(工具延迟挂载)的单测:阈值判定、检索匹配/排序/大小写/limit/
// 无命中建议、命中即入 loaded、索引段生成与截断、DeferredTool 包装转发、
// AgentLoop 过滤谓词(启用时 tools=核心+loaded+tool_search;一次 Run() 中途
// 挂载下一轮就带上;未挂载调用得友好错误)、prompts 注入点。

#include <doctest/doctest.h>

#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "agent/prompts.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "tools/tool_search.hpp"

using namespace lubancode;

namespace {

// 可配置 deferred 的假工具。
class StubTool : public tools::Tool {
public:
    StubTool(std::string name, std::string description, bool deferred_flag,
             nlohmann::json schema = nlohmann::json::object())
        : name_(std::move(name)),
          description_(std::move(description)),
          deferred_flag_(deferred_flag),
          schema_(std::move(schema)) {}

    std::string name() const override { return name_; }
    std::string description() const override { return description_; }
    nlohmann::json input_schema() const override { return schema_; }
    bool deferred() const override { return deferred_flag_; }

    tools::Tool::Result execute(const nlohmann::json&) override {
        ++call_count;
        return {"stub 执行了", false};
    }

    int call_count = 0;

private:
    std::string name_;
    std::string description_;
    bool deferred_flag_;
    nlohmann::json schema_;
};

// 按脚本吐事件的假后端(跟 test_loop.cpp 同一个路数,精简版)。
class ScriptBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::vector<api::Request> captured_requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
        captured_requests.push_back(request);
        const std::size_t idx = captured_requests.size() - 1;
        if (idx >= scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "ScriptBackend: 脚本用完了", 0});
        }
        for (const auto& event : scripts[idx]) {
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

std::vector<api::StreamEvent> ToolCallScript(const std::string& id, const std::string& name,
                                              const std::string& input_json) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, id, name},
        api::ToolUseInputDelta{0, input_json},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

// 请求的 tools 数组里有没有某个名字。
bool HasToolDef(const api::Request& request, const std::string& name) {
    for (const auto& def : request.tools) {
        if (def.name == name) {
            return true;
        }
    }
    return false;
}

nlohmann::json AddSchema() {
    return nlohmann::json::parse(R"({
        "type": "object",
        "properties": {"a": {"type": "number"}, "b": {"type": "number"}},
        "required": ["a", "b"]
    })");
}

}  // namespace

// ---------------------------------------------------------------------------
// 阈值判定
// ---------------------------------------------------------------------------

TEST_CASE("DeferralEnabled: 总数不超阈值不启用,超了才启用,threshold=0 永不启用") {
    CHECK_FALSE(tools::DeferralEnabled(20, 20));  // 等于阈值:不启用
    CHECK_FALSE(tools::DeferralEnabled(5, 20));
    CHECK(tools::DeferralEnabled(21, 20));
    CHECK(tools::DeferralEnabled(6, 5));
    CHECK_FALSE(tools::DeferralEnabled(1000, 0));  // 0 = 永不延迟
    CHECK_FALSE(tools::DeferralEnabled(0, 0));
}

// ---------------------------------------------------------------------------
// tool_search 匹配
// ---------------------------------------------------------------------------

TEST_CASE("tool_search: 单词命中,加入 loaded,返回文本含已挂载与参数概要") {
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<StubTool>("read_file", "读文件", false));
    registry.Register(std::make_unique<StubTool>("mcp__test__add", "返回两个数字的和", true, AddSchema()));
    registry.Register(std::make_unique<StubTool>("mcp__test__echo", "原样返回传入的文本", true));

    auto loaded = std::make_shared<std::set<std::string>>();
    tools::ToolSearchTool search(registry, loaded);

    const auto result = search.execute(nlohmann::json{{"query", "数字"}});
    REQUIRE_FALSE(result.is_error);
    CHECK(result.content.find("mcp__test__add") != std::string::npos);
    CHECK(result.content.find("mcp__test__echo") == std::string::npos);
    CHECK(result.content.find("已挂载") != std::string::npos);
    CHECK(result.content.find("a(number, 必填)") != std::string::npos);
    CHECK(loaded->count("mcp__test__add") == 1);
    CHECK(loaded->count("mcp__test__echo") == 0);
}

TEST_CASE("tool_search: 多词按命中词数排序,命中多的在前") {
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<StubTool>("mcp__t__echo", "原样返回文本", true));
    registry.Register(std::make_unique<StubTool>("mcp__t__add", "数字求和,返回结果", true));

    auto loaded = std::make_shared<std::set<std::string>>();
    tools::ToolSearchTool search(registry, loaded);

    // "数字 返回" 两个词:add 命中 2 个,echo 命中 1 个,add 排前面。
    const auto result = search.execute(nlohmann::json{{"query", "数字 返回"}});
    REQUIRE_FALSE(result.is_error);
    const std::size_t pos_add = result.content.find("mcp__t__add");
    const std::size_t pos_echo = result.content.find("mcp__t__echo");
    REQUIRE(pos_add != std::string::npos);
    REQUIRE(pos_echo != std::string::npos);
    CHECK(pos_add < pos_echo);
    CHECK(loaded->size() == 2);
}

TEST_CASE("tool_search: 大小写不敏感") {
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<StubTool>("mcp__srv__AddNumbers", "Add two numbers", true));

    auto loaded = std::make_shared<std::set<std::string>>();
    tools::ToolSearchTool search(registry, loaded);

    const auto result = search.execute(nlohmann::json{{"query", "ADD"}});
    REQUIRE_FALSE(result.is_error);
    CHECK(result.content.find("mcp__srv__AddNumbers") != std::string::npos);
    CHECK(loaded->count("mcp__srv__AddNumbers") == 1);
}

TEST_CASE("tool_search: limit 限制返回数量,核心工具绝不进结果") {
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<StubTool>("read_file", "tool 读文件", false));
    for (int i = 0; i < 8; ++i) {
        registry.Register(std::make_unique<StubTool>("mcp__s__tool" + std::to_string(i), "tool 编号工具", true));
    }
    auto loaded = std::make_shared<std::set<std::string>>();
    tools::ToolSearchTool search(registry, loaded);

    const auto result = search.execute(nlohmann::json{{"query", "tool"}, {"limit", 3}});
    REQUIRE_FALSE(result.is_error);
    CHECK(loaded->size() == 3);
    CHECK(result.content.find("read_file") == std::string::npos);  // 核心工具不参与检索

    // 不给 limit 默认 5:换一批词再搜(loaded 里已有 3 个,重复命中无害)。
    auto loaded2 = std::make_shared<std::set<std::string>>();
    tools::ToolSearchTool search2(registry, loaded2);
    const auto result2 = search2.execute(nlohmann::json{{"query", "tool"}});
    REQUIRE_FALSE(result2.is_error);
    CHECK(loaded2->size() == 5);
}

TEST_CASE("tool_search: 无命中给前缀近似建议,不算错误,不入 loaded") {
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<StubTool>("mcp__test__add", "求和", true));

    auto loaded = std::make_shared<std::set<std::string>>();
    tools::ToolSearchTool search(registry, loaded);

    // "mcp__test" 是名字前缀,但描述/名字里没有这个完整子串?其实名字里有
    // ——换一个真的无命中的词:"zzz" 无子串命中;"mcp" 是前缀,拿它当建议词。
    const auto result = search.execute(nlohmann::json{{"query", "zzz"}});
    REQUIRE_FALSE(result.is_error);
    CHECK(result.content.find("没有命中") != std::string::npos);
    CHECK(loaded->empty());

    // 前缀兜底:token 是名字的前缀(名字子串匹配不到时不会发生——所以拿
    // 一个"是前缀但不是描述子串"的词不好造;直接验证建议格式即可)。
    tools::ToolRegistry registry2;
    registry2.Register(std::make_unique<StubTool>("weird_name", "完全不相干", true));
    auto loaded2 = std::make_shared<std::set<std::string>>();
    tools::ToolSearchTool search2(registry2, loaded2);
    const auto result2 = search2.execute(nlohmann::json{{"query", "weird_name_extra_long"}});
    REQUIRE_FALSE(result2.is_error);
    CHECK(result2.content.find("没有命中") != std::string::npos);
    CHECK(result2.content.find("weird_name") != std::string::npos);  // 名字是 token 的前缀,给建议
    CHECK(loaded2->empty());
}

TEST_CASE("tool_search: 参数校验——缺 query/空白 query/坏 limit 都报错") {
    tools::ToolRegistry registry;
    auto loaded = std::make_shared<std::set<std::string>>();
    tools::ToolSearchTool search(registry, loaded);

    CHECK(search.execute(nlohmann::json::object()).is_error);
    CHECK(search.execute(nlohmann::json{{"query", "   "}}).is_error);
    CHECK(search.execute(nlohmann::json{{"query", 42}}).is_error);
    CHECK(search.execute(nlohmann::json{{"query", "x"}, {"limit", 0}}).is_error);
    CHECK(search.execute(nlohmann::json{{"query", "x"}, {"limit", "five"}}).is_error);
}

// ---------------------------------------------------------------------------
// 索引段
// ---------------------------------------------------------------------------

TEST_CASE("索引段: 只列延迟未加载的工具,描述截断到 80 字,loaded 的不再出现") {
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<StubTool>("read_file", "核心工具不该出现", false));
    std::string long_desc;
    for (int i = 0; i < 100; ++i) {
        long_desc += "很";  // 100 个中文字符,超 80 该截
    }
    registry.Register(std::make_unique<StubTool>("mcp__s__long", long_desc, true));
    registry.Register(std::make_unique<StubTool>("mcp__s__done", "已加载的", true));

    std::set<std::string> loaded{"mcp__s__done"};
    const std::string segment = tools::BuildDeferredToolsIndexSegment(registry, loaded);

    CHECK(segment.find("1 个延迟") != std::string::npos);
    CHECK(segment.find("mcp__s__long") != std::string::npos);
    CHECK(segment.find("mcp__s__done") == std::string::npos);
    CHECK(segment.find("read_file") == std::string::npos);
    CHECK(segment.find("tool_search") != std::string::npos);  // 指路
    // 截断:80 个"很" + 省略号,原文 100 个不该全在。
    std::string eighty;
    for (int i = 0; i < 80; ++i) {
        eighty += "很";
    }
    CHECK(segment.find(eighty + "…") != std::string::npos);
    CHECK(segment.find(long_desc) == std::string::npos);
}

TEST_CASE("索引段: 没有延迟未加载的工具时是空串(核心工具再多也不注段)") {
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<StubTool>("read_file", "核心", false));
    std::set<std::string> loaded;
    CHECK(tools::BuildDeferredToolsIndexSegment(registry, loaded).empty());

    registry.Register(std::make_unique<StubTool>("mcp__s__t", "延迟", true));
    CHECK_FALSE(tools::BuildDeferredToolsIndexSegment(registry, loaded).empty());
    loaded.insert("mcp__s__t");
    CHECK(tools::BuildDeferredToolsIndexSegment(registry, loaded).empty());  // 全加载了也不注段
}

TEST_CASE("prompts: WithDeferredToolsIndex 空段原样返回,非空段以空行分隔追加") {
    CHECK(agent::WithDeferredToolsIndex("base", "") == "base");
    CHECK(agent::WithDeferredToolsIndex("base", "索引段") == "base\n\n索引段");
}

// ---------------------------------------------------------------------------
// DeferredTool 包装
// ---------------------------------------------------------------------------

TEST_CASE("DeferredTool: 原样转发 name/description/schema/needs_confirm/execute,deferred 恒真") {
    auto inner = std::make_unique<StubTool>("mcp__s__x", "描述", false, AddSchema());
    auto* inner_ptr = inner.get();
    tools::DeferredTool wrapped(std::move(inner));

    CHECK(wrapped.name() == "mcp__s__x");
    CHECK(wrapped.description() == "描述");
    CHECK(wrapped.input_schema() == AddSchema());
    CHECK_FALSE(wrapped.needs_confirm());
    CHECK(wrapped.deferred());
    const auto result = wrapped.execute(nlohmann::json::object());
    CHECK_FALSE(result.is_error);
    CHECK(inner_ptr->call_count == 1);
}

// ---------------------------------------------------------------------------
// AgentLoop 过滤谓词(请求拼装 + 未挂载友好错误 + 中途挂载生效)
// ---------------------------------------------------------------------------

namespace {

// 造一张"启用延迟"的注册表:核心 read_file + 两个延迟 MCP 工具 + tool_search,
// 返回配套的 loaded 集合和过滤谓词。
struct DeferredSetup {
    tools::ToolRegistry registry;
    std::shared_ptr<std::set<std::string>> loaded = std::make_shared<std::set<std::string>>();

    DeferredSetup() {
        registry.Register(std::make_unique<StubTool>("read_file", "读文件", false));
        registry.Register(std::make_unique<StubTool>("mcp__test__add", "返回两个数字的和", true, AddSchema()));
        registry.Register(std::make_unique<StubTool>("mcp__test__echo", "原样返回文本", true));
        registry.Register(std::make_unique<tools::ToolSearchTool>(registry, loaded));
    }

    std::function<bool(const tools::Tool&)> Filter() const {
        auto loaded_copy = loaded;
        return [loaded_copy](const tools::Tool& tool) {
            return !tool.deferred() || loaded_copy->count(tool.name()) != 0;
        };
    }
};

}  // namespace

TEST_CASE("启用延迟: 请求 tools = 核心 + tool_search,不带未加载的延迟工具;挂载后带上") {
    DeferredSetup setup;
    ScriptBackend backend;
    backend.scripts = {TextScript("好")};

    agent::AgentProfile profile{.request{.model = "m"}, .system_prompt = "sys"};
    profile.tool_filter = setup.Filter();
    agent::Agent loop(backend, setup.registry, std::move(profile));

    agent::TurnWiring callbacks;
    REQUIRE(loop.Run("你好", callbacks).has_value());
    REQUIRE(backend.captured_requests.size() == 1);
    const auto& req = backend.captured_requests[0];
    CHECK(HasToolDef(req, "read_file"));
    CHECK(HasToolDef(req, "tool_search"));
    CHECK_FALSE(HasToolDef(req, "mcp__test__add"));
    CHECK_FALSE(HasToolDef(req, "mcp__test__echo"));

    // 挂载一个,下一次 Run() 的请求就带上它(loaded 是外部共享状态)。
    setup.loaded->insert("mcp__test__add");
    backend.scripts.push_back(TextScript("好"));
    REQUIRE(loop.Run("再来", callbacks).has_value());
    const auto& req2 = backend.captured_requests[1];
    CHECK(HasToolDef(req2, "mcp__test__add"));
    CHECK_FALSE(HasToolDef(req2, "mcp__test__echo"));
}

TEST_CASE("一次 Run() 中途 tool_search 挂载:下一轮请求立即带上新工具,且能直接调用") {
    DeferredSetup setup;
    ScriptBackend backend;
    backend.scripts = {
        ToolCallScript("t1", "tool_search", R"({"query":"数字"})"),
        ToolCallScript("t2", "mcp__test__add", R"({"a":17,"b":25})"),
        TextScript("42"),
    };

    agent::AgentProfile profile{.request{.model = "m"}, .system_prompt = "sys"};
    profile.tool_filter = setup.Filter();
    agent::Agent loop(backend, setup.registry, std::move(profile));

    agent::TurnWiring callbacks;
    REQUIRE(loop.Run("算 17+25", callbacks).has_value());
    REQUIRE(backend.captured_requests.size() == 3);

    CHECK_FALSE(HasToolDef(backend.captured_requests[0], "mcp__test__add"));
    CHECK(HasToolDef(backend.captured_requests[1], "mcp__test__add"));  // 挂载后下一轮就在
    CHECK(setup.loaded->count("mcp__test__add") == 1);

    // tool_search 的 tool_result 里说了"已挂载";add 真的执行了(不是友好错误)。
    const auto& search_result =
        std::get<api::ToolResultBlock>(loop.history()[2].content[0]);
    CHECK(search_result.content.find("已挂载") != std::string::npos);
    const auto& add_result = std::get<api::ToolResultBlock>(loop.history()[4].content[0]);
    CHECK_FALSE(add_result.is_error);
}

TEST_CASE("未挂载的延迟工具被直接调用:不执行,友好错误指路 tool_search") {
    DeferredSetup setup;
    auto* add_tool = dynamic_cast<StubTool*>(setup.registry.Find("mcp__test__add"));
    REQUIRE(add_tool != nullptr);

    ScriptBackend backend;
    backend.scripts = {
        ToolCallScript("t1", "mcp__test__add", R"({"a":1,"b":2})"),
        TextScript("好吧,我先搜"),
    };

    agent::AgentProfile profile{.request{.model = "m"}, .system_prompt = "sys"};
    profile.tool_filter = setup.Filter();
    agent::Agent loop(backend, setup.registry, std::move(profile));

    agent::TurnWiring callbacks;
    REQUIRE(loop.Run("直接算", callbacks).has_value());
    CHECK(add_tool->call_count == 0);  // 真没执行

    const auto& tool_result = std::get<api::ToolResultBlock>(loop.history()[2].content[0]);
    CHECK(tool_result.is_error);
    CHECK(tool_result.content.find("尚未挂载") != std::string::npos);
    CHECK(tool_result.content.find("tool_search") != std::string::npos);
}

TEST_CASE("不设过滤谓词: 全量直挂,延迟标记不影响任何行为(现状回归)") {
    DeferredSetup setup;  // 注册表里有延迟工具和 tool_search,但不设谓词
    ScriptBackend backend;
    backend.scripts = {ToolCallScript("t1", "mcp__test__add", R"({"a":1,"b":2})"), TextScript("好")};

    agent::Agent loop(backend, setup.registry, agent::AgentProfile{.request{.model = "m"}, .system_prompt = "sys"});
    agent::TurnWiring callbacks;
    REQUIRE(loop.Run("算", callbacks).has_value());

    // 请求带全部工具,未挂载的延迟工具照样直接执行成功。
    CHECK(HasToolDef(backend.captured_requests[0], "mcp__test__add"));
    CHECK(HasToolDef(backend.captured_requests[0], "mcp__test__echo"));
    const auto& tool_result = std::get<api::ToolResultBlock>(loop.history()[2].content[0]);
    CHECK_FALSE(tool_result.is_error);
}
