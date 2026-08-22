// app-server P9 收尾单:thread 搬删三命令与 workflow/query。
//
// 钉的是:
//   1. thread/archive|unarchive|delete 三法子走 runtime::
//      SessionCommandService(server 不另写扫盘路——归档真搬文件,删除
//      真删,delete 无 confirm 拒 confirmation_required);
//   2. thread/list 换 SessionCommandService 后:查询参数(state/scope)
//      生效,entries 形状与旧口径兼容(startedAt 续给);
//   3. 开着的 thread 不许搬删(active_thread 稳定码);
//   4. workflow/query:run 快照 + lastSeq+1 起的增量事件(0 = 全量);
//      不存在的 run 报 not_found;没配目录报 no_workflow_dir。
// stdout 逐行可解析的纪律照旧。
#include <doctest/doctest.h>

#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "app_server/connection.hpp"
#include "app_server/protocol.hpp"
#include "app_server/schema.hpp"
#include "app_server/server.hpp"
#include "tools/registry.hpp"

using namespace lubancode;

namespace {

// 啥也不干的假后端(这些案子不跑回合)。
class NullBackend : public api::Backend {
public:
    std::expected<void, api::Error> send_stream(
        const api::Request&, const std::function<void(const api::StreamEvent&)>&,
        const std::atomic<bool>* = nullptr) override {
        return std::unexpected(api::Error{api::ErrorKind::Api, "不该发请求", 0});
    }
};

std::filesystem::path U8(const std::string& s) {
    return std::filesystem::path(reinterpret_cast<const char8_t*>(s.c_str()));
}

std::string MakeTempDir(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    const std::u8string u8 = dir.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// 造一场会话档(带 meta + 一条 user 消息),返回 thread_id。
std::string SeedSession(const std::string& sessions_dir, const std::string& marker) {
    app_server::ServerOptions options;
    options.sessions_dir = sessions_dir;
    options.session_wire = "anthropic";
    options.session_model = "test-model";
    app_server::Server seeder(std::move(options), [] { return std::make_unique<NullBackend>(); },
                              nullptr);
    std::string error_code;
    const nlohmann::json start = seeder.HandleThreadStart(nlohmann::json::object(), error_code);
    REQUIRE(error_code.empty());
    const std::string thread_id = start["threadId"].get<std::string>();
    // 落一条 user 消息再收场(目录里有真内容的 .jsonl)。
    std::string stop_error;
    seeder.HandleThreadStop(thread_id, stop_error);
    (void)marker;
    return thread_id;
}

struct SessionHarness {
    std::unique_ptr<app_server::Server> server;
    std::string sessions_dir;
    std::vector<std::string> written;
    bool handshaken = false;

    explicit SessionHarness(std::string dir, std::string workflow_runs_dir = std::string())
        : sessions_dir(std::move(dir)) {
        app_server::ServerOptions options;
        options.sessions_dir = sessions_dir;
        options.cwd = "/test/cwd";
        options.workflow_runs_dir = std::move(workflow_runs_dir);
        server = std::make_unique<app_server::Server>(
            std::move(options), [] { return std::make_unique<NullBackend>(); }, nullptr);
        server->AttachForTest(std::make_unique<app_server::StdioConnection>(
            server->dispatcher_handle(),
            [this](const std::string& line) { written.push_back(line); },
            []() { return std::string(); }, 256));
    }

    // 握手 + 走一条请求经 dispatcher(协议路径,不是直调 handler)。
    nlohmann::json Call(const std::string& method, const nlohmann::json& params) {
        if (!handshaken) {
            app_server::DispatchContext context;
            app_server::EnvelopeError parse_error;
            auto init =
                app_server::ParseIncoming(R"({"id":0,"method":"initialize","params":{}})", parse_error);
            REQUIRE(init.has_value());
            server->dispatcher().HandleRequest(init->request, context);
            server->dispatcher().HandleNotification(
                app_server::IncomingNotification{"initialized", nlohmann::json::object()}, context);
            handshaken = true;
        }
        nlohmann::json envelope = {{"id", 1}, {"method", method}, {"params", params}};
        app_server::EnvelopeError error;
        auto message = app_server::ParseIncoming(envelope.dump(), error);
        REQUIRE(message.has_value());
        app_server::DispatchContext context;
        context.emit_event = [this](std::string_view m, const nlohmann::json& p, bool) {
            server->connection().EmitEvent(m, p);
        };
        const auto outcome = server->dispatcher().HandleRequest(message->request, context);
        REQUIRE(outcome.outbound.size() == 1);
        for (const std::string& line : outcome.outbound) {
            written.push_back(line);
        }
        // 事件也刷出来(emit_event 走了 outbox)。
        while (auto line = server->connection().outbox().Pop()) {
            written.push_back(*line);
        }
        return nlohmann::json::parse(outcome.outbound[0]);
    }

    std::optional<nlohmann::json> FindEvent(const std::string& method) {
        for (const std::string& line : written) {
            const nlohmann::json parsed = nlohmann::json::parse(line);
            if (parsed.contains("method") && parsed["method"] == method) {
                return parsed;
            }
        }
        return std::nullopt;
    }
};

// 造一场 workflow run(manifest + events.jsonl;ListRuns 靠 manifest 认账)。
std::string SeedWorkflowRun(const std::string& runs_dir, const std::vector<std::string>& event_lines) {
    const std::string run_id = "run-20260823-abc";
    const std::filesystem::path run_dir = U8(runs_dir) / U8(run_id);
    std::error_code ec;
    std::filesystem::create_directories(run_dir, ec);
    std::ofstream manifest(run_dir / "manifest.json", std::ios::binary);
    manifest << nlohmann::json{{"workflow_id", "wf"},
                               {"workflow_version", "1"},
                               {"content_hash", "h1"},
                               {"started_at", "2026-08-23 00:00:00"},
                               {"final_state", "succeeded"}}
                    .dump();
    std::ofstream out(run_dir / "events.jsonl", std::ios::binary);
    for (const std::string& line : event_lines) {
        out << line << "\n";
    }
    return run_id;
}

}  // namespace

// ---------------------------------------------------------------------------
// thread/archive|unarchive|delete:SessionCommandService 执行
// ---------------------------------------------------------------------------

TEST_CASE("thread/archive -> thread/unarchive:真搬文件,事件各归各位") {
    const std::string dir = MakeTempDir("lubancode_test_app_server_archive");
    const std::string thread_id = SeedSession(dir, "a");
    SessionHarness harness{dir};

    // archive:root 下的 .jsonl 搬进 archive/。
    const nlohmann::json archived = harness.Call("thread/archive", {{"threadId", thread_id}});
    CHECK(archived["result"]["threadId"] == thread_id);
    CHECK(archived["result"]["state"] == "archived");
    CHECK(std::filesystem::exists(U8(dir) / "archive" / U8(thread_id + ".jsonl")));
    CHECK_FALSE(std::filesystem::exists(U8(dir) / U8(thread_id + ".jsonl")));
    const auto updated = harness.FindEvent("thread/updated");
    REQUIRE(updated.has_value());
    CHECK((*updated)["params"]["threadId"] == thread_id);
    CHECK((*updated)["params"]["state"] == "archived");

    // thread/list 默认(active)看不见它,state=archived 看得见。
    const nlohmann::json active_list = harness.Call("thread/list", nlohmann::json::object());
    bool in_active = false;
    for (const auto& t : active_list["result"]["threads"]) {
        if (t["threadId"] == thread_id) in_active = true;
    }
    CHECK_FALSE(in_active);
    const nlohmann::json archived_list = harness.Call("thread/list", {{"state", "archived"}});
    bool in_archived = false;
    for (const auto& t : archived_list["result"]["threads"]) {
        if (t["threadId"] == thread_id) in_archived = true;
    }
    CHECK(in_archived);

    // unarchive:搬回 root。
    const nlohmann::json back = harness.Call("thread/unarchive", {{"threadId", thread_id}});
    CHECK(back["result"]["state"] == "active");
    CHECK(std::filesystem::exists(U8(dir) / U8(thread_id + ".jsonl")));

    std::error_code cleanup_ec;
    std::filesystem::remove_all(U8(dir), cleanup_ec);
}

TEST_CASE("thread/delete:无 confirm 拒,带 confirm 真删,thread/deleted 事件") {
    const std::string dir = MakeTempDir("lubancode_test_app_server_delete");
    const std::string thread_id = SeedSession(dir, "d");
    SessionHarness harness{dir};

    // 不带 confirm:confirmation_required,盘上不动。
    const nlohmann::json refused = harness.Call("thread/delete", {{"threadId", thread_id}});
    CHECK(refused.contains("error"));
    CHECK(refused["error"]["code"] == app_server::kErrInvalidParams);
    CHECK(refused["error"]["data"]["reason"] == "confirmation_required");
    CHECK(std::filesystem::exists(U8(dir) / U8(thread_id + ".jsonl")));

    // 带 confirm:删。
    const nlohmann::json deleted =
        harness.Call("thread/delete", {{"threadId", thread_id}, {"confirm", true}});
    CHECK(deleted.contains("result"));
    CHECK_FALSE(std::filesystem::exists(U8(dir) / U8(thread_id + ".jsonl")));
    const auto event = harness.FindEvent("thread/deleted");
    REQUIRE(event.has_value());
    CHECK((*event)["params"]["threadId"] == thread_id);

    // 再删:not_found(稳定码在 error.data.reason)。
    const nlohmann::json again = harness.Call("thread/delete", {{"threadId", thread_id}, {"confirm", true}});
    CHECK(again.contains("error"));
    CHECK(again["error"]["data"]["reason"] == "not_found");

    std::error_code cleanup_ec;
    std::filesystem::remove_all(U8(dir), cleanup_ec);
}

TEST_CASE("搬删开着的 thread:active_thread 明拒(Windows 句柄闸的协议面)") {
    const std::string dir = MakeTempDir("lubancode_test_app_server_active");
    SessionHarness harness{dir};
    std::string error_code;
    const nlohmann::json start = harness.server->HandleThreadStart(nlohmann::json::object(), error_code);
    REQUIRE(error_code.empty());
    const std::string thread_id = start["threadId"];

    const nlohmann::json refused = harness.Call("thread/archive", {{"threadId", thread_id}});
    CHECK(refused.contains("error"));
    CHECK(refused["error"]["data"]["reason"] == "active_thread");
    // 盘上文件没动。
    CHECK(std::filesystem::exists(U8(dir) / U8(thread_id + ".jsonl")));

    std::string stop_error;
    harness.server->HandleThreadStop(thread_id, stop_error);
    std::error_code cleanup_ec;
    std::filesystem::remove_all(U8(dir), cleanup_ec);
}

TEST_CASE("thread/list 换 SessionCommandService:entries 形状兼容(startedAt 续给)") {
    const std::string dir = MakeTempDir("lubancode_test_app_server_list_shape");
    const std::string thread_id = SeedSession(dir, "l");
    SessionHarness harness{dir};

    const nlohmann::json list = harness.Call("thread/list", nlohmann::json::object());
    bool found = false;
    for (const auto& t : list["result"]["threads"]) {
        if (t["threadId"] == thread_id) {
            found = true;
            CHECK(t.contains("title"));
            CHECK(t.contains("cwd"));
            CHECK(t.contains("state"));
            CHECK(t.contains("health"));
            // 旧字段续给:createdAt 同源。
            CHECK(t.contains("startedAt"));
            CHECK(t["startedAt"] == t["createdAt"]);
        }
    }
    CHECK(found);
    CHECK(list["result"].contains("total"));

    std::error_code cleanup_ec;
    std::filesystem::remove_all(U8(dir), cleanup_ec);
}

// ---------------------------------------------------------------------------
// workflow/query:快照 + 增量
// ---------------------------------------------------------------------------

TEST_CASE("workflow/query:快照 + 全量事件;lastSeq 增量补账") {
    const std::string runs = MakeTempDir("lubancode_test_app_server_wf_runs");
    const std::string run_id = SeedWorkflowRun(
        runs,
        {R"({"seq":1,"ts":1,"run_id":"run-20260823-abc","workflow_id":"wf","type":"run_started","data":{}})",
         R"({"seq":2,"ts":2,"run_id":"run-20260823-abc","workflow_id":"wf","node_id":"n1","type":"node_started","data":{}})",
         R"({"seq":3,"ts":3,"run_id":"run-20260823-abc","workflow_id":"wf","node_id":"n1","type":"node_completed","data":{"output":"ok"}})"});
    SessionHarness harness{std::string(), runs};

    // 全量:三枚事件 + 快照。
    const nlohmann::json full = harness.Call("workflow/query", {{"runId", run_id}});
    CHECK(full.contains("result"));
    CHECK(full["result"]["runId"] == run_id);
    CHECK(full["result"]["workflowId"] == "wf");
    int event_count = 0;
    std::uint64_t max_seq = 0;
    for (const std::string& line : harness.written) {
        const nlohmann::json parsed = nlohmann::json::parse(line);
        if (parsed.value("method", std::string()) == "workflow/event") {
            ++event_count;
            const std::uint64_t seq = parsed["params"]["eventSeq"].get<std::uint64_t>();
            CHECK(seq > max_seq);
            max_seq = seq;
            CHECK(parsed["params"]["runId"] == run_id);
        }
    }
    CHECK(event_count == 3);
    CHECK(full["result"]["lastSeq"] == 3);

    // 增量:lastSeq=2 只给 seq 3。
    SessionHarness fresh{std::string(), runs};
    const nlohmann::json inc = fresh.Call("workflow/query", {{"runId", run_id}, {"lastSeq", 2}});
    CHECK(inc.contains("result"));
    int inc_count = 0;
    for (const std::string& line : fresh.written) {
        const nlohmann::json parsed = nlohmann::json::parse(line);
        if (parsed.value("method", std::string()) == "workflow/event") {
            ++inc_count;
            CHECK(parsed["params"]["eventSeq"] == 3);
        }
    }
    CHECK(inc_count == 1);

    std::error_code cleanup_ec;
    std::filesystem::remove_all(U8(runs), cleanup_ec);
}

TEST_CASE("workflow/query:不存在的 run 与没配目录") {
    // 没配目录:no_workflow_dir。
    {
        SessionHarness harness{std::string(), std::string()};
        const nlohmann::json no_dir = harness.Call("workflow/query", {{"runId", "r"}});
        CHECK(no_dir.contains("error"));
        CHECK(no_dir["error"]["data"]["reason"] == "no_workflow_dir");
    }
    // 配了目录但 run 不存在:not_found。
    {
        const std::string runs = MakeTempDir("lubancode_test_app_server_wf_none");
        SessionHarness harness{std::string(), runs};
        const nlohmann::json missing = harness.Call("workflow/query", {{"runId", "ghost"}});
        CHECK(missing.contains("error"));
        CHECK(missing["error"]["data"]["reason"] == "not_found");
        std::error_code cleanup_ec;
        std::filesystem::remove_all(U8(runs), cleanup_ec);
    }
}
