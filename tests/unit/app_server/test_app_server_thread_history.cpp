// app-server 轨迹 v3 P3 第二棒 + V3-GAP-04:thread/resume 与 thread/read
// 两枚旧史方法。钉的是:
//   1. 冷 thread v3 场(索引跨 workspace 定位):read 回完整时间线
//      (kind=message|compact_marker + inCurrentContext/removedByCompacts 等
//      标志);resume 只回当前链上的消息 + 压缩标记 + contextSummary;
//   2. lastSeq 分页:回大于游标的条目;水位对齐本场段账行(来源链场
//      祖先段 seq 更高时,水位不冒充全局 max——防本场新行被增量过滤
//      误滤漏账);sourceSessions 亮来源链(祖先在前本场在后);
//   3. hidden 不默认外发(§4.28):标志照回,正文要 includeHidden 才带;
//   4. 活 thread(v2 账)与冷 v2 场:sourceFormat="v2" + 空 items,不冒充;
//   5. 参数错与档读不到;
//   6. V3-GAP-04:主账路径两代分派(v3 场 <id>.jsonl,旧场 main.jsonl);
//   7. thread/resume startExecution=true:经 SessionService 真恢复(v3 源
//      续接同 id,v2 源迁移新场);活场拒、read 带参拒、缺省只读零新建、
//      坏源如实拒(resume_source_rejected)。
// 缺省只读 replay:零模型调用、零工具重跑——两法子只回 v3 显示投影;
// startExecution=true 才启动恢复执行。
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
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
#include "trajectory/journal.hpp"  // Durability(V3-GAP-04 来源链夹具)
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"

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

// V3-GAP-04 的夹具与开关辅助 ----------------------------------------------

// 测试进程内临时设置环境变量,析构还原(本册注册钉 0,个别案显式开 v3)。
struct EnvGuard {
    explicit EnvGuard(const char* name, const char* value) : name_(name) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=" + value).c_str());
#else
        setenv(name_, value, 1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    const char* name_;
};

// v3 writer 的固定时钟(造账行要有稳定 timestamp)。
class FixedClock : public trajectory::v3::V3Clock {
public:
    std::int64_t WallMs() const override { return 1759468800000LL; }
};

// 一轮"user → assistant → 接纳"落进 v3 账(来源链夹具用)。
void InstallRound(trajectory::v3::V3Writer& writer, const std::string& turn_id,
                  const std::string& user_text) {
    trajectory::v3::MessageDraft user;
    user.turn_id = turn_id;
    user.purpose = trajectory::v3::MessagePurpose::Conversation;
    user.origin = trajectory::v3::MessageOrigin::Human;
    user.message = nlohmann::json::object({{"role", "user"}, {"content", user_text}});
    const auto user_receipt =
        writer.AppendMessage(std::move(user), trajectory::Durability::PowerLoss);
    REQUIRE(user_receipt.status == trajectory::v3::WriteReceipt::Status::Committed);
    REQUIRE(writer.AdmitMessages({user_receipt.id}).status ==
            trajectory::v3::WriteReceipt::Status::Committed);

    trajectory::v3::MessageDraft assistant;
    assistant.turn_id = turn_id;
    assistant.step_id = "step-000001";
    assistant.request_id = "request-000001";
    assistant.purpose = trajectory::v3::MessagePurpose::Conversation;
    assistant.origin = trajectory::v3::MessageOrigin::SessionRuntime;
    assistant.provider = "stub";
    assistant.wire = "openai";
    assistant.model = "stub-mini";
    assistant.response_model = nlohmann::json(nullptr);
    assistant.usage = nlohmann::json(nullptr);
    assistant.message = nlohmann::json::object({{"role", "assistant"},
                                                {"content", "收到:" + user_text}});
    const auto assistant_receipt =
        writer.AppendMessage(std::move(assistant), trajectory::Durability::PowerLoss);
    REQUIRE(assistant_receipt.status == trajectory::v3::WriteReceipt::Status::Committed);
    REQUIRE(writer.AdmitMessages({assistant_receipt.id}).status ==
            trajectory::v3::WriteReceipt::Status::Committed);
}

// 在 sessions 根下用 V3Writer 现场种一场 v3 账(合法账实造,不用静态
// fixture 抄本——writer/reader 同源会互相自证)。
std::optional<trajectory::v3::V3Writer> PlantWrittenV3Session(
    const std::filesystem::path& sessions_root, const std::string& session_id) {
    std::error_code ec;
    const auto dir = sessions_root / U8(session_id);
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return std::nullopt;
    }
    static FixedClock clock;
    auto writer = trajectory::v3::V3Writer::Start(
        dir / U8(session_id + ".jsonl"), session_id, "run-" + session_id,
        "你是 LubanCode,读写跑都走工具。", nlohmann::json::object(),
        trajectory::v3::V3WriterOptions{}, &clock);
    if (!writer.has_value()) {
        return std::nullopt;
    }
    return std::move(*writer);
}

// resume.source.attached:五键指源末行(来源链续场的链头)。
void AttachSource(trajectory::v3::V3Writer& writer, const std::string& sessions_root,
                  const std::string& source_id) {
    const auto source = trajectory::v3::ReadV3Ledger(
        sessions_root / U8(source_id) / U8(source_id + ".jsonl"));
    REQUIRE(source.has_value());
    const auto last = source->LastEntry();
    REQUIRE(last.has_value());
    const std::string id = last->is_message
                               ? source->messages[last->index].message_id
                               : source->events[last->index].event_id;
    const std::string hash = last->is_message ? source->messages[last->index].line_hash
                                              : source->events[last->index].line_hash;
    trajectory::v3::EventDraft attached;
    attached.kind = trajectory::v3::EventKindV3::ResumeSourceAttached;
    attached.payload = nlohmann::json::object({
        {"sourceRef",
         nlohmann::json::object({{"sessionId", source_id},
                                 {"runId", source->run_id},
                                 {"seq", last->seq},
                                 {"id", id},
                                 {"hash", hash}})},
        {"contextRevision", source->context.revision},
        {"systemMessageRef", source->context.system_message_ref},
        {"branch", "main"},
    });
    REQUIRE(writer.AppendEvent(std::move(attached), trajectory::Durability::PowerLoss).status ==
            trajectory::v3::WriteReceipt::Status::Committed);
}

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
// 活/冷 thread:如实报格式,不冒充(V3-LEGACY-01 后新建唯一 v3)
// ---------------------------------------------------------------------------

TEST_CASE("thread/read|resume: 活 thread(v3 账)走账本路,如实回 v3") {
    const std::string dir = MakeTempDir("lubancode-as-thr-hot-v3");
    {
        HistoryHarness harness(dir);
        const std::string thread_id = harness.StartThread();  // 不停场:活 thread

        for (const char* method : {"thread/read", "thread/resume"}) {
            const nlohmann::json read = harness.Call(method, {{"threadId", thread_id}});
            CHECK(read.contains("result"));
            CHECK(read["result"]["sourceFormat"] == "v3");
            CHECK(read["result"]["count"] == 0);
            CHECK(read["result"]["items"].empty());
            // contextSummary 的有无归读面合同(有摘要才报,不编数),不再钉
            // "v2 场必无"的旧口径。
        }
    }
    std::error_code cleanup_ec;
    std::filesystem::remove_all(U8(dir), cleanup_ec);
}

TEST_CASE("thread/read: 冷 v3 场(停场后经索引定位)回 v3 空表") {
    const std::string dir = MakeTempDir("lubancode-as-thr-cold-v3");
    {
        HistoryHarness harness(dir);
        const std::string thread_id = harness.StartThread();
        harness.StopThread(thread_id);  // 冷 thread:索引定位

        const nlohmann::json read = harness.Call("thread/read", {{"threadId", thread_id}});
        CHECK(read.contains("result"));
        CHECK(read["result"]["sourceFormat"] == "v3");
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

// ---------------------------------------------------------------------------
// V3-GAP-04:主账路径两代分派
// ---------------------------------------------------------------------------

TEST_CASE("thread/start: 主账路径两代分派——v3 场 <id>.jsonl,旧场 main.jsonl") {
    const std::string dir = MakeTempDir("lubancode-as-thr-main-path");
    {
        HistoryHarness harness(dir);
        // 旧场(案内自钉 0——册注册注入的 =0 可能被别案的 EnvGuard 析构
        // 洗成空串):main.jsonl,与旧行为一字不动。
        EnvGuard v2_guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "0");
        const std::string v2_thread = harness.StartThread();
        const std::string v2_path = harness.server->ThreadMainPathForTest(v2_thread);
        REQUIRE_FALSE(v2_path.empty());
        REQUIRE(v2_path.size() >= 10);
        CHECK(v2_path.compare(v2_path.size() - 10, 10, "main.jsonl") == 0);

        // v3 场(显式开):<sessionId>.jsonl(504fddb1 定的主账名)。
        {
            EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
            const std::string v3_thread = harness.StartThread();
            const std::string v3_path = harness.server->ThreadMainPathForTest(v3_thread);
            REQUIRE_FALSE(v3_path.empty());
            const std::string tail = v3_thread + ".jsonl";
            REQUIRE(v3_path.size() >= tail.size());
            CHECK(v3_path.compare(v3_path.size() - tail.size(), tail.size(), tail) == 0);
            // v3 开场即落首行 system:主账文件在盘上。
            CHECK(std::filesystem::exists(U8(v3_path)));
        }
    }
    std::error_code cleanup_ec;
    std::filesystem::remove_all(U8(dir), cleanup_ec);
}

// ---------------------------------------------------------------------------
// V3-GAP-04:来源链场的游标——sourceSessions 亮链,水位对齐本场段
// ---------------------------------------------------------------------------

TEST_CASE("thread/read: 来源链场——亮来源链,水位对齐本场段不冒充全局 max") {
    const std::string dir = MakeTempDir("lubancode-as-thr-chain");
    const std::string ancestor = "20260924-chain-anc-a";
    const std::string own = "20260924-chain-own-b";
    {
        HistoryHarness harness(dir);
        harness.StartThread();  // 开一场只为建 workspace 房
        const std::filesystem::path sessions = FindSessionsDir(dir + "/workspaces");
        REQUIRE_FALSE(sessions.empty());

        // 祖先场:三轮对话(seq 抬高);本场:attached 指祖先 + 一轮对话
        //(本场段 seq 低于祖先段——正是裸全局 max(seq) 会漏账的形状)。
        {
            auto ancestor_writer = PlantWrittenV3Session(sessions, ancestor);
            REQUIRE(ancestor_writer.has_value());
            InstallRound(*ancestor_writer, "turn-000001", "祖先第一问");
            InstallRound(*ancestor_writer, "turn-000002", "祖先第二问");
            InstallRound(*ancestor_writer, "turn-000003", "祖先第三问");
        }
        {
            auto own_writer = PlantWrittenV3Session(sessions, own);
            REQUIRE(own_writer.has_value());
            // fs::path 不隐转 std::string(MSVC 的 string_type 是宽串,
            // C2664);显式折 generic_string。
            AttachSource(*own_writer, sessions.generic_string(), ancestor);
            InstallRound(*own_writer, "turn-000001", "本场新问");
        }

        const nlohmann::json read = harness.Call("thread/read", {{"threadId", own}});
        CHECK(read.contains("result"));
        const nlohmann::json& result = read["result"];
        CHECK(result["sourceFormat"] == "v3");
        CHECK(result["sessionId"] == own);

        // 来源链亮出来:祖先在前、本场在后(§5.2"跨 resume 来源链使用
        // 固定来源清单",游标带会话来源的读面基础)。
        REQUIRE(result["sourceSessions"].is_array());
        REQUIRE(result["sourceSessions"].size() == 2);
        CHECK(result["sourceSessions"][0] == ancestor);
        CHECK(result["sourceSessions"][1] == own);

        // 合流时间线:祖先段在前,本场段在后(祖先 6 条 + 本场 2 条)。
        const nlohmann::json& items = result["items"];
        REQUIRE(items.size() == 8);

        // 水位对齐本场段:合流序最后一条(本场段末)的 seq 就是水位——
        // 祖先段 seq 更高也不把它当水位(否则本场后续新行 seq 低于祖先
        // 水位,增量页会漏账)。
        const std::uint64_t watermark = result["lastSeq"].get<std::uint64_t>();
        CHECK(watermark == items.back()["seq"].get<std::uint64_t>());
        std::uint64_t global_max = 0;
        for (const auto& item : items) {
            global_max = std::max(global_max, item["seq"].get<std::uint64_t>());
        }
        CHECK(global_max > watermark);  // 祖先段确实更高,水位没冒充全局 max

        // 增量口径:带水位再问,本场段条目(seq <= 水位)不重发;祖先段
        // seq 高于水位的条目按 at-least-once 重发(静态前缀,客户端按
        // messageId 去重)——不漏账优先于不重发。水位不动(本场无新行)。
        const nlohmann::json again =
            harness.Call("thread/read", {{"threadId", own}, {"lastSeq", watermark}});
        REQUIRE(again["result"]["items"].is_array());
        CHECK(again["result"]["items"].size() > 0);  // 祖先段高 seq 条目重发
        for (const auto& item : again["result"]["items"]) {
            CHECK(item["seq"].get<std::uint64_t>() > watermark);
        }
        CHECK(again["result"]["lastSeq"].get<std::uint64_t>() == watermark);
    }
    std::error_code cleanup_ec;
    std::filesystem::remove_all(U8(dir), cleanup_ec);
}

// ---------------------------------------------------------------------------
// V3-GAP-04:thread/resume 的 startExecution——真恢复执行路
// ---------------------------------------------------------------------------

TEST_CASE("thread/resume: startExecution——v3 冷场经 SessionService 续接源场") {
    const std::string dir = MakeTempDir("lubancode-as-thr-resume-exec");
    {
        EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
        HistoryHarness harness(dir);
        harness.StartThread();
        PlantV3Session(dir + "/workspaces", "compact_full.jsonl", kV3Id);

        // 缺省(不带 startExecution):只读预览,零新建、零恢复。
        const std::size_t before = harness.server->active_thread_count();
        const nlohmann::json preview = harness.Call("thread/resume", {{"threadId", kV3Id}});
        CHECK(preview.contains("result"));
        CHECK(preview["result"].value("resumed", false) == false);
        CHECK(harness.server->active_thread_count() == before);

        // 真恢复:resumed + resumedThreadId(v3 源续接源场,同 id)+
        // 恢复视图照回(threadId 仍是请求 id,老形状不动)。
        const nlohmann::json resumed = harness.Call(
            "thread/resume", {{"threadId", kV3Id}, {"startExecution", true}});
        CHECK(resumed.contains("result"));
        const nlohmann::json& result = resumed["result"];
        CHECK(result["resumed"] == true);
        CHECK(result["resumedThreadId"] == kV3Id);
        CHECK(result["threadId"] == kV3Id);
        CHECK(result["sourceFormat"] == "v3");
        CHECK(result.contains("contextSummary"));
        CHECK(result["sourceSessions"].size() >= 1);
        // 恢复场是活 thread:本进程可续用 turn/start。
        CHECK(harness.server->active_thread_count() == before + 1);
        CHECK_FALSE(harness.server->ThreadMainPathForTest(kV3Id).empty());

        // 恢复过的场再 resume startExecution:活场,明拒。
        const nlohmann::json again = harness.Call(
            "thread/resume", {{"threadId", kV3Id}, {"startExecution", true}});
        CHECK(again.contains("error"));
        CHECK(again["error"]["data"]["code"] == "active_thread");
    }
    std::error_code cleanup_ec;
    std::filesystem::remove_all(U8(dir), cleanup_ec);
}

TEST_CASE("thread/resume: startExecution——v2 冷场迁移新场,新 threadId") {
    const std::string dir = MakeTempDir("lubancode-as-thr-resume-v2exec");
    {
        // 案内自钉 0:册注册注入的 =0 可能被先前案的 EnvGuard 析构洗成
        // 空串(空串非 "0" = v3 默认开),v2 源的断言要在真 v2 场上做。
        EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "0");
        HistoryHarness harness(dir);
        const std::string thread_id = harness.StartThread();  // v2 场
        harness.StopThread(thread_id);  // 冷场:索引定位

        const std::size_t before = harness.server->active_thread_count();
        const nlohmann::json resumed = harness.Call(
            "thread/resume", {{"threadId", thread_id}, {"startExecution", true}});
        CHECK(resumed.contains("result"));
        const nlohmann::json& result = resumed["result"];
        CHECK(result["resumed"] == true);
        // v2 源:开迁移新场(新 id),不是同 id 续写。
        CHECK(result["resumedThreadId"] != thread_id);
        CHECK(result["threadId"] == thread_id);
        CHECK(harness.server->active_thread_count() == before + 1);
        // 本册环境 v2 开关关着:迁移新场也是 v2,无 v3 投影可读,如实报
        // 格式空表不冒充(恢复事实照报)。
        CHECK(result["sourceFormat"] == "v2");
        CHECK(result["items"].empty());
    }
    std::error_code cleanup_ec;
    std::filesystem::remove_all(U8(dir), cleanup_ec);
}

TEST_CASE("thread/read|resume: startExecution 的拒面——read 不认,坏源如实拒") {
    const std::string dir = MakeTempDir("lubancode-as-thr-resume-reject");
    {
        HistoryHarness harness(dir);
        harness.StartThread();
        // read 带参即拒:读面不许被误当恢复口。
        const nlohmann::json read_reject = harness.Call(
            "thread/read", {{"threadId", "any"}, {"startExecution", true}});
        CHECK(read_reject.contains("error"));

        // 坏源(截断尾行,验卷不过):回落开的普通场关掉,如实拒
        // resume_source_rejected,不冒充恢复成功、不留守空场。
        PlantV3Session(dir + "/workspaces", "compact_full.jsonl", kV3Id);
        const std::filesystem::path stream =
            FindSessionsDir(dir + "/workspaces") / U8(kV3Id) / U8(std::string(kV3Id) + ".jsonl");
        {
            std::error_code ec;
            const auto bytes = std::filesystem::file_size(stream, ec);
            REQUIRE_FALSE(ec);
            REQUIRE(bytes > 32);
            std::filesystem::resize_file(stream, bytes - 24, ec);  // 尾行拦腰截断
            REQUIRE_FALSE(ec);
        }
        const std::size_t before = harness.server->active_thread_count();
        const nlohmann::json bad = harness.Call(
            "thread/resume", {{"threadId", kV3Id}, {"startExecution", true}});
        CHECK(bad.contains("error"));
        CHECK(bad["error"]["data"]["code"] == "resume_source_rejected");
        CHECK(harness.server->active_thread_count() == before);
    }
    std::error_code cleanup_ec;
    std::filesystem::remove_all(U8(dir), cleanup_ec);
}
