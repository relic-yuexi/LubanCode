// app-server 阶段 3 单:diff/进度。
//
// 钉的是:
//   1. 工具条目事件带中立 diff 行表(write_file/edit_file 的 item/started
//      带 diff 字段,runtime::DiffTable 直转;别的工具不带);
//   2. 命令条目:run_command 的 item type 是 command,输出整段落
//      item/completed(工具层无流式回调,如实整段);
//   3. 出站队列的 delta 合并:撞满时同 itemId 的 delta 并进队尾,
//      coalesced 计数真算(不再恒 0);合并不了才丢 + queue/overflow
//      通报(dropped/coalesced 落位);
//   4. 事件显式 seq:每条事件 params.seq 单调(进程级发号局);
//   5. 图片输入:turn/start 带 images,假 backend 收到的请求里真有图。
// stdout 逐行可解析的纪律照旧。
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "app_server/connection.hpp"
#include "app_server/outbox.hpp"
#include "app_server/protocol.hpp"
#include "app_server/schema.hpp"
#include "app_server/server.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// 共享脚本的假后端(审批测试同款);请求体留档,图片断言用。Server 的
// backend_factory 每回合新建一只,这里让"工厂出的那只"与"测试攥的那只"
// 共享同一份账(scripts/seen_requests 都在 shared state 里)。
class SharedScriptBackend : public api::Backend {
public:
    struct State {
        std::mutex mutex;
        std::vector<std::vector<api::StreamEvent>> scripts;
        std::vector<api::Request> seen_requests;
        std::size_t index = 0;
    };

    explicit SharedScriptBackend(std::shared_ptr<State> state) : state_(std::move(state)) {}

    std::expected<void, api::Error> send_stream(
        const api::Request& request, const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* = nullptr) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->seen_requests.push_back(request);
        if (state_->index >= state_->scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "脚本用完了", 0});
        }
        for (const api::StreamEvent& event : state_->scripts[state_->index]) {
            on_event(event);
        }
        ++state_->index;
        return {};
    }

private:
    std::shared_ptr<State> state_;
};

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id, const std::string& tool_name,
                                            const std::string& input_json) {
    return {
        api::MessageStart{"msg", "fake-model"},
        api::ToolUseStart{0, tool_id, tool_name},
        api::ToolUseInputDelta{0, input_json},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

std::vector<api::StreamEvent> TextOnlyScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "fake-model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{10, 5, 0, 0, 0}},
    };
}

// 假文件工具:名字可换(write_file/edit_file/run_command/read_file),
// 不真碰盘(diff 行表读真盘,这里用真临时文件)。
class FakeNamedTool : public tools::Tool {
public:
    FakeNamedTool(std::string name, std::shared_ptr<std::atomic<int>> counter)
        : name_(std::move(name)), counter_(std::move(counter)) {}

    std::string name() const override { return name_; }
    std::string description() const override { return "假工具:" + name_; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    tools::Tool::Result execute(const nlohmann::json&) override {
        counter_->fetch_add(1);
        return tools::Tool::Result{name_ + " 跑完了", false};
    }

private:
    std::string name_;
    std::shared_ptr<std::atomic<int>> counter_;
};

struct ScriptedIo {
    std::vector<std::string> written;
    app_server::StdioConnection::LineWriter Writer() {
        return [this](const std::string& line) { written.push_back(line); };
    }
};

struct ProgressHarness {
    std::shared_ptr<SharedScriptBackend::State> backend = std::make_shared<SharedScriptBackend::State>();
    ScriptedIo io;
    std::unique_ptr<app_server::Server> server;
    std::shared_ptr<std::atomic<int>> tool_calls = std::make_shared<std::atomic<int>>(0);
    std::vector<std::string> tool_names;

    explicit ProgressHarness(std::vector<std::string> names, const std::string& sessions_dir = std::string(),
                             std::size_t outbox_capacity = 4096) {
        tool_names = std::move(names);
        app_server::ServerOptions options;
        options.sessions_dir = sessions_dir;
        options.cwd = "/test/cwd";
        options.outbox_capacity = outbox_capacity;
        std::shared_ptr<SharedScriptBackend::State> state = backend;
        server = std::make_unique<app_server::Server>(
            std::move(options),
            [state]() -> std::unique_ptr<api::Backend> {
                return std::make_unique<SharedScriptBackend>(state);
            },
            [this]() -> std::unique_ptr<tools::ToolRegistry> {
                auto registry = std::make_unique<tools::ToolRegistry>();
                for (const std::string& name : tool_names) {
                    registry->Register(std::make_unique<FakeNamedTool>(name, tool_calls));
                }
                return registry;
            });
        AttachIo(outbox_capacity);
    }

    void AttachIo(std::size_t outbox_capacity) {
        server->AttachForTest(std::make_unique<app_server::StdioConnection>(
            server->dispatcher_handle(), io.Writer(), []() { return std::string(); },
            outbox_capacity));
    }

    void PumpOutbox() {
        while (auto line = server->connection().outbox().Pop()) {
            io.written.push_back(*line);
        }
    }

    std::optional<nlohmann::json> FindEvent(const std::string& method) {
        PumpOutbox();
        for (const std::string& line : io.written) {
            const nlohmann::json parsed = nlohmann::json::parse(line);
            if (parsed.contains("method") && parsed["method"] == method) {
                return parsed;
            }
        }
        return std::nullopt;
    }
};

std::string MakeTempDir(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    const std::u8string u8 = dir.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

void WriteFileUtf8(const std::string& path, const std::string& content) {
    std::ofstream out(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary);
    out << content;
}

}  // namespace

// ---------------------------------------------------------------------------
// diff 行表穿透:item/started 带 runtime::DiffTable 直转的行表
// ---------------------------------------------------------------------------

TEST_CASE("diff 行表:write_file 的 item/started 带中立行表") {
    const std::string dir = MakeTempDir("lubancode_test_app_server_diff");
    const std::string target = dir + "/new-file.txt";
    ProgressHarness harness({"write_file"});
    // 新文件:全 Add,oldExists=false。第二步给收尾文本(工具跑完模型
    // 还得再说一句)。
    harness.backend->scripts = {ToolUseScript("t1", "write_file",
                                              nlohmann::json{{"path", target}, {"content", "a\nb\n"}}.dump()),
                                TextOnlyScript("写好了。")};
    // 写盘前目标不存在 -> oldExists=false;写盘发生在工具执行(diff
    // 在 on_tool_start 算,先于执行)。
    std::string error_code;
    const nlohmann::json start = harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    REQUIRE(error_code.empty());
    harness.server->HandleTurnStart(start["threadId"], "写文件", {}, error_code);
    REQUIRE(error_code.empty());

    const auto item_started = harness.FindEvent("item/started");
    REQUIRE(item_started.has_value());
    const nlohmann::json& item = (*item_started)["params"]["item"];
    CHECK(item["type"] == "tool");
    CHECK(item["tool"] == "write_file");
    REQUIRE(item.contains("diff"));
    const nlohmann::json& diff = item["diff"];
    CHECK(diff["path"] == target);
    CHECK(diff["oldExists"] == false);
    CHECK(diff["addedLines"] == 2);
    CHECK(diff["removedLines"] == 0);
    REQUIRE(diff["rows"].size() == 2);
    CHECK(diff["rows"][0]["kind"] == "add");
    CHECK(diff["rows"][0]["text"] == "a");
    CHECK(diff["rows"][0]["newNo"] == 1);
    CHECK(diff["rows"][0]["oldNo"] == 0);

    // 收口:条目有终态,回合有唯一终态。
    const auto completed = harness.FindEvent("turn/completed");
    REQUIRE(completed.has_value());
    CHECK((*completed)["params"]["status"] == "success");

    std::string stop_error;
    harness.server->HandleThreadStop(start["threadId"], stop_error);
    std::error_code cleanup_ec;
    std::filesystem::remove_all(
        std::filesystem::path(reinterpret_cast<const char8_t*>(dir.c_str())), cleanup_ec);
}

TEST_CASE("diff 行表:edit_file 定位成功,located/replacedCount 落位") {
    const std::string dir = MakeTempDir("lubancode_test_app_server_diff_edit");
    const std::string target = dir + "/edit.txt";
    WriteFileUtf8(target, "one\ntwo\nthree\n");
    ProgressHarness harness({"edit_file"});
    harness.backend->scripts = {ToolUseScript("t1", "edit_file",
                                              nlohmann::json{{"path", target},
                                                             {"old_string", "two"},
                                                             {"new_string", "TWO"}}
                                                .dump()),
                                TextOnlyScript("改好了。")};
    std::string error_code;
    const nlohmann::json start = harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    REQUIRE(error_code.empty());
    harness.server->HandleTurnStart(start["threadId"], "改文件", {}, error_code);
    REQUIRE(error_code.empty());

    const auto item_started = harness.FindEvent("item/started");
    REQUIRE(item_started.has_value());
    const nlohmann::json& diff = (*item_started)["params"]["item"]["diff"];
    CHECK(diff["located"] == true);
    CHECK(diff["replacedCount"] == 1);
    // 上下文行 + 删 + 增:one 上下文,two 删,TWO 增,three 上下文。
    bool saw_del = false;
    bool saw_add = false;
    int context_count = 0;
    for (const nlohmann::json& row : diff["rows"]) {
        if (row["kind"] == "del") saw_del = true;
        if (row["kind"] == "add") saw_add = true;
        if (row["kind"] == "context") ++context_count;
    }
    CHECK(saw_del);
    CHECK(saw_add);
    CHECK(context_count == 2);
    CHECK(diff["addedLines"] == 1);
    CHECK(diff["removedLines"] == 1);

    std::string stop_error;
    harness.server->HandleThreadStop(start["threadId"], stop_error);
    std::error_code cleanup_ec;
    std::filesystem::remove_all(
        std::filesystem::path(reinterpret_cast<const char8_t*>(dir.c_str())), cleanup_ec);
}

TEST_CASE("diff 行表:别的工具不带 diff;run_command 的条目类型是 command") {
    ProgressHarness harness({"run_command", "read_file"});
    harness.backend->scripts = {ToolUseScript(
                                    "t1", "run_command", nlohmann::json{{"command", "echo hi"}}.dump()),
                                TextOnlyScript("跑完了。")};
    std::string error_code;
    const nlohmann::json start = harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    REQUIRE(error_code.empty());
    harness.server->HandleTurnStart(start["threadId"], "跑命令", {}, error_code);
    REQUIRE(error_code.empty());

    const auto item_started = harness.FindEvent("item/started");
    REQUIRE(item_started.has_value());
    const nlohmann::json& item = (*item_started)["params"]["item"];
    CHECK(item["type"] == "command");
    CHECK(item["tool"] == "run_command");
    CHECK_FALSE(item.contains("diff"));

    // 命令输出:工具层没有流式回调,整段落 item/completed(结果文本在
    // payload.result)。
    bool saw_output = false;
    harness.PumpOutbox();
    for (const std::string& line : harness.io.written) {
        const nlohmann::json parsed = nlohmann::json::parse(line);
        if (parsed.value("method", std::string()) == "item/completed" &&
            parsed["params"]["item"].value("id", std::string()) ==
                item["id"].get<std::string>()) {
            CHECK(parsed["params"]["item"]["result"].get<std::string>().find("跑完了") !=
                  std::string::npos);
            saw_output = true;
        }
    }
    CHECK(saw_output);

    std::string stop_error;
    harness.server->HandleThreadStop(start["threadId"], stop_error);
}

// ---------------------------------------------------------------------------
// 增量合并:撞满时同 itemId 的 delta 并进队尾,coalesced 真算
// ---------------------------------------------------------------------------

TEST_CASE("BoundedOutbox:撞满的 delta 并进队尾同 item 的 delta,coalesced 计数") {
    app_server::BoundedOutbox outbox(2);
    const std::string started =
        R"({"method":"turn/started","params":{"threadId":"t","turnId":"turn-1","seq":1}})";
    const std::string delta_1 =
        R"({"method":"item/delta","params":{"threadId":"t","turnId":"turn-1","itemId":"item-1","delta":"你好","seq":3}})";
    const std::string delta_2 =
        R"({"method":"item/delta","params":{"threadId":"t","turnId":"turn-1","itemId":"item-1","delta":",远方","seq":4}})";
    CHECK(outbox.Push(started));
    CHECK(outbox.Push(delta_1));
    // 撞满:delta_2 与队尾 delta_1 同 item,并进去(不丢内容、不长队)。
    CHECK(outbox.Push(delta_2));
    CHECK(outbox.dropped() == 0);
    CHECK(outbox.coalesced() == 1);
    CHECK(outbox.size() == 2);

    const std::string head = *outbox.Pop();
    const std::string merged = *outbox.Pop();
    // 队头的 turn/started 没动。
    CHECK(nlohmann::json::parse(head)["method"] == "turn/started");
    const nlohmann::json merged_json = nlohmann::json::parse(merged);
    CHECK(merged_json["method"] == "item/delta");
    CHECK(merged_json["params"]["delta"] == "你好,远方");
    CHECK(merged_json["params"]["seq"] == 4); // seq 取后到的那枚
}

TEST_CASE("BoundedOutbox:不同 itemId 的 delta 合不了,丢弃记账") {
    app_server::BoundedOutbox outbox(1);
    CHECK(outbox.Push(
        R"({"method":"item/delta","params":{"itemId":"item-1","delta":"a"}})"));
    // 队尾是 item-1 的 delta,来的是 item-2 的:合不了,丢。
    CHECK_FALSE(outbox.Push(
        R"({"method":"item/delta","params":{"itemId":"item-2","delta":"b"}})"));
    CHECK(outbox.dropped() == 1);
    CHECK(outbox.coalesced() == 0);

    // 队尾不是 delta(比如 item/started)也合不了。
    app_server::BoundedOutbox outbox2(1);
    CHECK(outbox2.Push(R"({"method":"item/started","params":{"item":{"id":"item-1"}}})"));
    CHECK_FALSE(outbox2.Push(
        R"({"method":"item/delta","params":{"itemId":"item-1","delta":"x"}})"));
    CHECK(outbox2.dropped() == 1);
    CHECK(outbox2.coalesced() == 0);
}

TEST_CASE("EmitEvent:丢事件时补 queue/overflow 通报,dropped/coalesced 落位") {
    ProgressHarness harness({}, std::string(), /*outbox_capacity=*/1);
    // 容量 1:第一条占满,第二条不同 item 的 delta 合不了被丢,随后
    // 通报入队(它自己是 must_keep,挤掉可丢的)。
    const std::string thread_id = [&] {
        std::string error_code;
        const nlohmann::json start =
            harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
        return start["threadId"].get<std::string>();
    }();
    app_server::StdioConnection& connection = harness.server->connection();
    connection.EmitEvent("thread/started", nlohmann::json{{"threadId", thread_id}});
    connection.EmitEvent("item/delta",
                         nlohmann::json{{"threadId", thread_id}, {"itemId", "item-x"}, {"delta", "x"}});
    harness.PumpOutbox();

    bool saw_overflow = false;
    for (const std::string& line : harness.io.written) {
        const nlohmann::json parsed = nlohmann::json::parse(line);
        if (parsed.value("method", std::string()) == "queue/overflow") {
            saw_overflow = true;
            CHECK(parsed["params"]["dropped"].get<std::uint64_t>() >= 1);
            CHECK(parsed["params"].contains("coalesced"));
            CHECK(parsed["params"].contains("seq"));
        }
    }
    CHECK(saw_overflow);
}

// ---------------------------------------------------------------------------
// 事件显式 seq:每条事件带 seq,进程内单调
// ---------------------------------------------------------------------------

TEST_CASE("事件显式 seq:整回合事件全带 seq 且单调") {
    ProgressHarness harness({});
    harness.backend->scripts = {TextOnlyScript("一句话。")};
    std::string error_code;
    const nlohmann::json start = harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    REQUIRE(error_code.empty());
    harness.server->HandleTurnStart(start["threadId"], "问", {}, error_code);
    REQUIRE(error_code.empty());
    harness.PumpOutbox();

    std::uint64_t last_seq = 0;
    int event_count = 0;
    for (const std::string& line : harness.io.written) {
        const nlohmann::json parsed = nlohmann::json::parse(line);
        REQUIRE(parsed.contains("method"));
        REQUIRE(parsed["params"].contains("seq"));
        const std::uint64_t seq = parsed["params"]["seq"].get<std::uint64_t>();
        CHECK(seq > last_seq);
        last_seq = seq;
        ++event_count;
    }
    CHECK(event_count >= 4); // turn/started + item/* + turn/usage + turn/completed

    std::string stop_error;
    harness.server->HandleThreadStop(start["threadId"], stop_error);
}

// ---------------------------------------------------------------------------
// 图片输入:turn/start 带 images,请求里真有图
// ---------------------------------------------------------------------------

TEST_CASE("图片输入:turn/start 的 images 折进请求(阶段 3 冻结的字段名)") {
    ProgressHarness harness({});
    std::string error_code;
    const nlohmann::json start = harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    REQUIRE(error_code.empty());
    // 假 backend 收到请求前先跑一回合(脚本空 = 报错回合也行,请求已发)。
    harness.backend->scripts = {TextOnlyScript("看到图了。")};
    const std::vector<nlohmann::json> images = {
        nlohmann::json{{"mediaType", "image/png"}, {"data", "aGk="}, {"filename", "a.png"}}};
    harness.server->HandleTurnStart(start["threadId"], "看图", images, error_code);
    REQUIRE(error_code.empty());

    // 请求账里首条 user 消息带 ImageBlock。
    REQUIRE(harness.backend->seen_requests.size() >= 1);
    const api::Request& request = harness.backend->seen_requests[0];
    REQUIRE(!request.messages.empty());
    bool has_image = false;
    for (const api::ContentBlock& block : request.messages[0].content) {
        if (const auto* image = std::get_if<api::ImageBlock>(&block)) {
            has_image = true;
            CHECK(image->media_type == "image/png");
            CHECK(image->data == "aGk=");
            CHECK(image->filename == "a.png");
        }
    }
    CHECK(has_image);

    std::string stop_error;
    harness.server->HandleThreadStop(start["threadId"], stop_error);
}

// ---------------------------------------------------------------------------
// methods 冻结矩阵:initialize 能力表与 protocol.hpp 一一对应
// ---------------------------------------------------------------------------

TEST_CASE("methods 冻结矩阵:能力表如实报接线面") {
    const nlohmann::json result = app_server::MakeInitializeResult("0.0.0-test", "linux");
    const nlohmann::json& caps = result["capabilities"];
    // 接线的:握手三件 + thread 六件 + turn 两件 + workflow/query。
    const std::vector<std::string> expected_methods = {
        "initialize", "initialized",      "shutdown",     "thread/start", "thread/list",
        "thread/stop", "thread/archive",  "thread/unarchive", "thread/delete",
        "turn/start", "turn/interrupt",   "workflow/query"};
    std::vector<std::string> actual = caps["methods"];
    std::sort(actual.begin(), actual.end());
    std::vector<std::string> expected = expected_methods;
    std::sort(expected.begin(), expected.end());
    CHECK(actual == expected);

    // 留位的:与 methods 不相交。
    for (const auto& pending : caps["pending"]) {
        bool leak = false;
        for (const auto& wired : caps["methods"]) {
            if (pending == wired) {
                leak = true;
            }
        }
        CHECK_FALSE(leak);
    }

    // 反向请求位:审批与 ask。
    CHECK(std::find(caps["serverRequests"].begin(), caps["serverRequests"].end(), "permission/request") !=
          caps["serverRequests"].end());
    CHECK(std::find(caps["serverRequests"].begin(), caps["serverRequests"].end(), "user/ask") !=
          caps["serverRequests"].end());
}
