// app-server 轨迹 v3 P3 第二棒:thread/resume 与 thread/read 两枚只读
// 旧史方法。钉的是:
//   1. 冷 thread v3 场(索引跨 workspace 定位):read 回完整时间线
//      (kind=message|compact_marker + inCurrentContext/removedByCompacts 等
//      标志);resume 只回当前链上的消息 + 压缩标记 + contextSummary;
//   2. lastSeq 分页:回大于游标的条目,水位记全时间线最大 seq(与
//      trace/query 同口径);
//   3. hidden 不默认外发(§4.28):标志照回,正文要 includeHidden 才带;
//   4. 活 thread(v2 账)与冷 v2 场:sourceFormat="v2" + 空 items,不冒充;
//   5. 参数错与档读不到。
// 只读 replay:零模型调用、零工具重跑——两法子只回 v3 显示投影。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "app_server/connection.hpp"
#include "app_server/protocol.hpp"
#include "app_server/schema.hpp"
#include "app_server/server.hpp"
#include "tools/registry.hpp"

using namespace lubancode;

namespace {

// 啥也不干的假后端(这些案子不跑回合,只读)。
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

std::filesystem::path Fixture(const char* name) {
    return std::filesystem::path(LUBANCODE_SOURCE_DIR) / "tests" / "fixtures" /
           "trajectory_v3" / name;
}

// 找 workspaces 根下唯一 workspace 房的 sessions/ 根(种 v3 场用)。
std::filesystem::path FindSessionsDir(const std::string& workspaces_dir) {
    const std::filesystem::path root = U8(workspaces_dir);
    std::error_code ec;
    for (const auto& room : std::filesystem::directory_iterator(root, ec)) {
        const auto sessions = room.path() / "sessions";
        if (std::filesystem::exists(sessions, ec)) {
            return sessions;
        }
    }
    return {};
}

// 把 v3 fixture 种成 workspace 里的会话目录 sessions/<id>/<id>.jsonl。
void PlantV3Session(const std::string& workspaces_dir, const char* fixture,
                    const std::string& session_id) {
    const auto sessions = FindSessionsDir(workspaces_dir);
    REQUIRE_FALSE(sessions.empty());
    const auto dir = sessions / U8(session_id);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::filesystem::copy_file(Fixture(fixture), dir / U8(session_id + ".jsonl"),
                               std::filesystem::copy_options::overwrite_existing, ec);
    REQUIRE_FALSE(ec);
}

struct HistoryHarness {
    std::unique_ptr<app_server::Server> server;
    std::string sessions_dir;
    std::vector<std::string> written;
    bool handshaken = false;

    explicit HistoryHarness(std::string dir) : sessions_dir(std::move(dir)) {
        app_server::ServerOptions options;
        options.workspaces_dir = sessions_dir + "/workspaces";
        options.cwd = sessions_dir;
        options.session_wire = "anthropic";
        options.session_model = "test-model";
        server = std::make_unique<app_server::Server>(
            std::move(options), [] { return std::make_unique<NullBackend>(); }, nullptr);
        server->AttachForTest(std::make_unique<app_server::StdioConnection>(
            server->dispatcher_handle(),
            [this](const std::string& line) { written.push_back(line); },
            []() { return std::string(); }, 256));
    }

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
        return nlohmann::json::parse(outcome.outbound[0]);
    }

    // 开一场(建 workspace 房 + v2 账),回 thread id。
    std::string StartThread() {
        std::string error_code;
        const nlohmann::json start = server->HandleThreadStart(nlohmann::json::object(), error_code);
        REQUIRE(error_code.empty());
        return start["threadId"].get<std::string>();
    }

    void StopThread(const std::string& thread_id) {
        std::string stop_error;
        server->HandleThreadStop(thread_id, stop_error);
    }
};

// 按 messageId 找一条 message 条目。
const nlohmann::json* FindMessage(const nlohmann::json& result, const std::string& id) {
    for (const auto& item : result["items"]) {
        if (item.value("kind", std::string()) == "message" &&
            item.value("messageId", std::string()) == id) {
            return &item;
        }
    }
    return nullptr;
}

const nlohmann::json* FindCompact(const nlohmann::json& result) {
    for (const auto& item : result["items"]) {
        if (item.value("kind", std::string()) == "compact_marker") {
            return &item;
        }
    }
    return nullptr;
}

constexpr const char* kV3Id = "20260910-083000-V3FIX4";  // compact_full fixture 的 sessionId

}  // namespace

// ---------------------------------------------------------------------------
// 冷 v3 thread:thread/read 完整时间线
// ---------------------------------------------------------------------------

TEST_CASE("thread/read: 冷 v3 场——完整时间线,标志齐,hidden 默认不发正文") {
    const std::string dir = MakeTempDir("lubancode-as-thr-read-v3");
    {
        HistoryHarness harness(dir);
        harness.StartThread();  // 开一场只为建 workspace 房
        PlantV3Session(dir + "/workspaces", "compact_full.jsonl", kV3Id);

        const nlohmann::json read = harness.Call("thread/read", {{"threadId", kV3Id}});
        CHECK(read.contains("result"));
        const nlohmann::json& result = read["result"];
        CHECK(result["sourceFormat"] == "v3");
        CHECK(result["sessionId"] == kV3Id);
        CHECK(result["count"] == result["items"].size());

        // 被压缩原文(msg-000002):在时间线、带 removedByCompacts 与
        // inCurrentContext=false(§4.10 历史全在,当前上下文另算)。
        const auto* removed = FindMessage(result, "msg-000002");
        REQUIRE(removed != nullptr);
        CHECK((*removed)["inCurrentContext"] == false);
        REQUIRE((*removed)["removedByCompacts"].size() == 1);
        CHECK((*removed)["removedByCompacts"][0] == "compact-000001");
        CHECK((*removed)["hidden"] == false);
        REQUIRE((*removed)["content"].size() > 0);
        CHECK((*removed)["content"][0]["type"] == "text");

        // 生效摘要(msg-000008,hidden):条目在、hidden 标志在,正文默认
        // 省略(§4.28"隐藏不等于删除",正文不默认发全)。
        const auto* summary = FindMessage(result, "msg-000008");
        REQUIRE(summary != nullptr);
        CHECK((*summary)["hidden"] == true);
        CHECK((*summary)["inCurrentContext"] == true);
        CHECK_FALSE((*summary).contains("content"));

        // 压缩标记:kind/持久 token 数/removed-retained refs(§4.11)。
        const auto* marker = FindCompact(result);
        REQUIRE(marker != nullptr);
        CHECK((*marker)["compactId"] == "compact-000001");
        CHECK((*marker)["contextTokensBefore"] == 142800);
        CHECK((*marker)["contextTokensAfter"] == 31600);
        REQUIRE((*marker)["removedMessageRefs"].size() == 2);
        CHECK((*marker)["removedMessageRefs"][0] == "msg-000002");

        // includeHidden=true:hidden 条目带正文(显式要才给)。
        const nlohmann::json raw =
            harness.Call("thread/read", {{"threadId", kV3Id}, {"includeHidden", true}});
        const auto* summary_full = FindMessage(raw["result"], "msg-000008");
        REQUIRE(summary_full != nullptr);
        CHECK((*summary_full).contains("content"));

        // lastSeq 分页:只回 seq > lastSeq 的条目;水位记全时间线最大
        // seq(与 trace/query 同口径,前端无感知差异)。时间线最末条目是
        // 压缩后的新输入 msg-000009(seq=23;事件行不进显示条目)。
        const nlohmann::json inc =
            harness.Call("thread/read", {{"threadId", kV3Id}, {"lastSeq", 20}});
        for (const auto& item : inc["result"]["items"]) {
            CHECK(item["seq"].get<std::uint64_t>() > 20);
        }
        CHECK(inc["result"]["lastSeq"] == 23);
    }
    std::error_code cleanup_ec;
    std::filesystem::remove_all(U8(dir), cleanup_ec);
}

TEST_CASE("thread/read: lastSeq 增量——第二次只拿新条目,不重发老账") {
    const std::string dir = MakeTempDir("lubancode-as-thr-read-inc");
    {
        HistoryHarness harness(dir);
        harness.StartThread();
        PlantV3Session(dir + "/workspaces", "compact_full.jsonl", kV3Id);

        const nlohmann::json first = harness.Call("thread/read", {{"threadId", kV3Id}});
        const std::uint64_t watermark = first["result"]["lastSeq"];
        CHECK(watermark > 0);
        // 用水位再问:同账没有新行,回空表、水位不动。
        const nlohmann::json second =
            harness.Call("thread/read", {{"threadId", kV3Id}, {"lastSeq", watermark}});
        CHECK(second["result"]["count"] == 0);
        CHECK(second["result"]["lastSeq"] == watermark);
    }
    std::error_code cleanup_ec;
    std::filesystem::remove_all(U8(dir), cleanup_ec);
}

// ---------------------------------------------------------------------------
// 冷 v3 thread:thread/resume 恢复视图
// ---------------------------------------------------------------------------

TEST_CASE("thread/resume: 冷 v3 场——只回当前链 + 压缩标记 + contextSummary") {
    const std::string dir = MakeTempDir("lubancode-as-thr-resume-v3");
    {
        HistoryHarness harness(dir);
        harness.StartThread();
        PlantV3Session(dir + "/workspaces", "compact_full.jsonl", kV3Id);

        const nlohmann::json resume = harness.Call("thread/resume", {{"threadId", kV3Id}});
        CHECK(resume.contains("result"));
        const nlohmann::json& result = resume["result"];
        CHECK(result["sourceFormat"] == "v3");

        // 被压缩原文不在恢复视图里(模型上下文只剩摘要 + 保留消息)。
        CHECK(FindMessage(result, "msg-000002") == nullptr);
        CHECK(FindMessage(result, "msg-000003") == nullptr);
        // 生效摘要与压缩后新输入在(§4.59 发送输入只取所选 main 链)。
        const auto* summary = FindMessage(result, "msg-000008");
        REQUIRE(summary != nullptr);
        CHECK((*summary)["inCurrentContext"] == true);
        CHECK(FindMessage(result, "msg-000009") != nullptr);
        // 压缩标记照插(多次压缩各显示各次数字,§4.11)。
        REQUIRE(FindCompact(result) != nullptr);
        // 恢复视图里的消息全部 inCurrentContext。
        for (const auto& item : result["items"]) {
            if (item["kind"] == "message") {
                CHECK(item["inCurrentContext"] == true);
            }
        }

        // 摘要:hidden 标志照回,正文默认省略;includeHidden 显式要才给。
        CHECK((*summary)["hidden"] == true);
        CHECK_FALSE((*summary).contains("content"));
        const nlohmann::json raw =
            harness.Call("thread/resume", {{"threadId", kV3Id}, {"includeHidden", true}});
        CHECK(FindMessage(raw["result"], "msg-000008")->contains("content"));

        // contextSummary:最近一次 applied 后的持久 token 数(§4.11)。
        CHECK(result["contextSummary"]["contextTokensAfter"] == 31600);
        CHECK(result["contextSummary"]["compacts"] == 1);
        CHECK(result["contextSummary"]["inContextMessages"].get<std::uint64_t>() >= 2);

        // lastSeq 同口径:水位在,增量可用。
        const std::uint64_t watermark = result["lastSeq"];
        const nlohmann::json second =
            harness.Call("thread/resume", {{"threadId", kV3Id}, {"lastSeq", watermark}});
        CHECK(second["result"]["count"] == 0);
    }
    std::error_code cleanup_ec;
    std::filesystem::remove_all(U8(dir), cleanup_ec);
}

// ---------------------------------------------------------------------------
// 活/冷 v2 thread:如实报格式,不冒充
// ---------------------------------------------------------------------------

TEST_CASE("thread/read|resume: 活 thread(v2 账)走账本路,如实回 v2") {
    const std::string dir = MakeTempDir("lubancode-as-thr-hot-v2");
    {
        HistoryHarness harness(dir);
        const std::string thread_id = harness.StartThread();  // 不停场:活 thread

        for (const char* method : {"thread/read", "thread/resume"}) {
            const nlohmann::json read = harness.Call(method, {{"threadId", thread_id}});
            CHECK(read.contains("result"));
            CHECK(read["result"]["sourceFormat"] == "v2");
            CHECK(read["result"]["count"] == 0);
            CHECK(read["result"]["items"].empty());
            if (std::string(method) == "thread/resume") {
                // v2 场没有上下文摘要可报:不给 contextSummary,不编数。
                CHECK_FALSE(read["result"].contains("contextSummary"));
            }
        }
    }
    std::error_code cleanup_ec;
    std::filesystem::remove_all(U8(dir), cleanup_ec);
}

TEST_CASE("thread/read: 冷 v2 场(停场后经索引定位)回 v2 空表") {
    const std::string dir = MakeTempDir("lubancode-as-thr-cold-v2");
    {
        HistoryHarness harness(dir);
        const std::string thread_id = harness.StartThread();
        harness.StopThread(thread_id);  // 冷 thread:索引定位

        const nlohmann::json read = harness.Call("thread/read", {{"threadId", thread_id}});
        CHECK(read.contains("result"));
        CHECK(read["result"]["sourceFormat"] == "v2");
        CHECK(read["result"]["items"].empty());
    }
    std::error_code cleanup_ec;
    std::filesystem::remove_all(U8(dir), cleanup_ec);
}

// ---------------------------------------------------------------------------
// 参数错与档读不到
// ---------------------------------------------------------------------------

TEST_CASE("thread/read|resume: 参数错与档读不到") {
    const std::string dir = MakeTempDir("lubancode-as-thr-err");
    {
        HistoryHarness harness(dir);
        // 缺 threadId。
        CHECK(harness.Call("thread/read", nlohmann::json::object()).contains("error"));
        CHECK(harness.Call("thread/resume", nlohmann::json::object()).contains("error"));
        // lastSeq 类型不对。
        CHECK(harness.Call("thread/read", {{"threadId", "x"}, {"lastSeq", "10"}}).contains("error"));
        // includeHidden 类型不对。
        CHECK(harness
                  .Call("thread/resume", {{"threadId", "x"}, {"includeHidden", "yes"}})
                  .contains("error"));
        // 不存在的 thread:没有会话账。
        const nlohmann::json gone = harness.Call("thread/read", {{"threadId", "ghost-thread"}});
        CHECK(gone.contains("error"));
        CHECK(gone["error"]["code"] == app_server::kErrInvalidParams);
    }
    // 没配 workspaces 根:同样没有会话账可查。
    {
        const std::string bare = MakeTempDir("lubancode-as-thr-bare");
        HistoryHarness harness(bare);
        const nlohmann::json no_dir = harness.Call("thread/read", {{"threadId", "any"}});
        CHECK(no_dir.contains("error"));
        std::error_code bare_ec;
        std::filesystem::remove_all(U8(bare), bare_ec);
    }
    std::error_code cleanup_ec;
    std::filesystem::remove_all(U8(dir), cleanup_ec);
}
