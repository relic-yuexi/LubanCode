// T08/V3-GAP-03(Session v3 旧设计清理单):MemoryLedgerBridge 的 v3 写路。
//
// 断点背景:v3 场 ledger.main()==nullptr,旧桥把它当"召回失败"——memory-on
// 场候选非空却一个不注。本册用 v3 主场(memory-on + 真开卷 + 真回合桥)钉:
//   1. 主路全流:候选落 display=hidden 的正式 user 消息(origin=context_
//      runtime,不冒充人类输入)→ context.input.applied 接纳进链 →
//      memory.recall.injected 事实(memoryId/revision/hash/messageRef)→
//      真实回合的 model.request.prepared.inputMessageRefs 沿链带上快照消息。
//      验卷收尾(哈希链不破)。
//   2. 快照不变性:记忆正文改版后旧消息行一字不动——恢复拿快照解释旧请求,
//      不用新 topic 正文倒推。
//   3. 快照提交失败:注入零发生(fail-closed),账上不冒领,链不长。
//   4. 派工冻结:父账只落事实(targetRunId + 快照),不写隐藏消息不进链
//      ——父模型没见过这段,请求引用不冒领;大正文走内容寻址 blob。
//   5. 写入因果边:memory.save.requested 只记发起(requested),不冒充
//      queued/committed(那两态各有 receipted/lifecycle 的真回执)。
//   6. 老调用方无回合号:writer 自家号池兜底,注入不丢。
//   7. schema 合同:两枚 kind 名字稳定、statusless、入册。
#include <doctest/doctest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"  // RequestPreparedContext(回合桥的模型边界口)
#include "api/types.hpp"
#include "app/memory_ledger_bridge.hpp"
#include "hooks/hash.hpp"
#include "memory/project_memory.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/blob_store.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/session_switch.hpp"  // FindV3SessionStream
#include "trajectory/v3/writer.hpp"          // VerifyV3File
#include "workspace/identity.hpp"

using namespace lubancode;
using lubancode::runtime::TrajectorySessionLedger;
using lubancode::runtime::TrajectoryTurnBridge;

namespace {

namespace fs = std::filesystem;

// ctest 钉 LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0;v3 册显式开回 1(同款见
// test_v3_memory_bypass.cpp)。
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

fs::path FreshRoot(const char* tag) {
    const auto dir =
        fs::temp_directory_path() / ("lubancode-v3-memory-recall-" + std::string(tag));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

TrajectorySessionLedger::Options LedgerOptions(const fs::path& root) {
    TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "repo";
    options.workspace_identity = lubancode::workspace::MakeFallbackIdentity(root / "repo");
    options.launch_cwd = "D:/tmp/repo";
    options.lubancode_version = "0.26.268-test";
    options.v3_system_content = "你是 LubanCode,读写跑都走工具。";
    return options;
}

void WriteText(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
}

std::string ReadText(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// 一只装了 fact.deploy 的记忆仓(worker 已落盘)。
struct MemoryFixture {
    fs::path repo;
    fs::path home;
    std::unique_ptr<memory::ProjectMemory> store;

    MemoryFixture(const fs::path& root, const std::string& content) {
        repo = root / "repo";
        home = root / "home";
        fs::create_directories(repo);
        fs::create_directories(home);
        WriteText(repo / "build.sh", "#!/bin/sh\necho build v1\n");

        memory::Options options;
        options.global_allowed = true;
        options.enabled = true;
        auto identity = memory::ResolveProjectIdentity(repo, home);
        REQUIRE(identity.has_value());
        store = std::make_unique<memory::ProjectMemory>(std::move(*identity), home, options);

        memory::SaveRequest request;
        request.kind = memory::MemoryKind::Fact;
        request.id = "fact.deploy";
        request.title = "部署命令";
        request.summary = "部署命令";
        request.content = content;
        request.keywords = {"deploy"};
        request.paths = {"build.sh"};
        request.confidence = "verified";
        REQUIRE(store->EnqueueSave(request).has_value());
        REQUIRE(memory::RunPendingMemoryJobs(home).has_value());
    }
};

std::vector<nlohmann::json> StreamLines(const fs::path& stream) {
    std::vector<nlohmann::json> lines;
    const auto raw = trajectory::ReadJournalLines(stream);
    REQUIRE(raw.has_value());
    for (const std::string& line : *raw) {
        lines.push_back(nlohmann::json::parse(line, nullptr, false));
        REQUIRE_FALSE(lines.back().is_discarded());
    }
    return lines;
}

const nlohmann::json* FindEvent(const std::vector<nlohmann::json>& lines, const char* kind) {
    for (const auto& line : lines) {
        if (line.value("type", std::string()) != "event") continue;
        if (line.value("kind", std::string()) == kind) return &line;
    }
    return nullptr;
}

// 召回落账的隐藏 user 消息(origin=context_runtime)。
const nlohmann::json* FindRecallMessage(const std::vector<nlohmann::json>& lines) {
    for (const auto& line : lines) {
        if (line.value("type", std::string()) != "message") continue;
        if (line.value("origin", std::string()) != "context_runtime") continue;
        if (line.value("purpose", std::string()) != "conversation") continue;
        return &line;
    }
    return nullptr;
}

api::Message UserMessage(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 主路全流:断点修复——候选真正进了账、进了链、进了请求引用
// ---------------------------------------------------------------------------
TEST_CASE("v3 主路: 隐藏快照消息 + 链接纳 + 事实行 + prepared 引用") {
    EnvGuard v3gate("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("main");
    MemoryFixture memory_fixture(root, "deploy 走 build.sh。");

    auto opened = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(opened.has_value());
    TrajectorySessionLedger& ledger = *opened;
    // 断点前提:v3 场没有 v2 recorder——旧桥在这里把召回整个判死。
    REQUIRE(ledger.v3_main_writer() != nullptr);
    REQUIRE(ledger.main() == nullptr);

    app::MemoryLedgerBridge bridge(ledger);
    memory_fixture.store->set_accounting(&bridge);
    memory_fixture.store->set_source_session(ledger.session_id());

    // 召回发生在回合开跑之前(回合号先铸好递进去——交互会话同款次序)。
    const std::string context = memory_fixture.store->BuildTurnContext(
        "deploy 怎么跑", memory_fixture.repo, memory::QueryOrigin::User,
        /*force_retrieval=*/false, "turn-1");
    REQUIRE_FALSE(context.empty());
    CHECK(context.find("build.sh") != std::string::npos);

    // 真实主回合:durable 输入落账 + 首请求 prepared(system 与建场一致,
    // 不触发三步换根,行面干净)。
    auto turn = ledger.NewTurnBridge(TrajectoryTurnBridge::Identity{"kimi", "responses", "terminal"});
    REQUIRE(turn != nullptr);
    turn->BeginTurn("turn-1", "external_user");
    const api::Message user = UserMessage("deploy 怎么跑");
    turn->RecordInput(user);
    api::Request request;
    request.model = "kimi-k2.6";
    request.system = LedgerOptions(root).v3_system_content;
    request.messages.push_back(user);
    const std::string request_id = turn->OnRequestPrepared(request, agent::RequestPreparedContext{});
    REQUIRE_FALSE(request_id.empty());

    const auto stream = trajectory::v3::FindV3SessionStream(ledger.session_dir());
    REQUIRE(stream.has_value());
    const auto lines = StreamLines(*stream);

    // (a) 正式隐藏 user 消息:origin 不是 human——重放不得再触发人类召回。
    const nlohmann::json* recall_message = FindRecallMessage(lines);
    REQUIRE(recall_message != nullptr);
    CHECK(recall_message->at("message").value("role", std::string()) == "user");
    CHECK(recall_message->value("turnId", std::string()) == "turn-1");
    REQUIRE(recall_message->contains("display"));
    CHECK(recall_message->at("display").at("mode").get<std::string>() == "hidden");
    const std::string snapshot_text = recall_message->at("message").at("content").get<std::string>();
    CHECK(snapshot_text.find("build.sh") != std::string::npos);
    const std::string message_id = recall_message->value("messageId", std::string());

    // (b) 事实行:候选身份 + 快照指纹 + 消息回指。
    const nlohmann::json* recall_event = FindEvent(lines, "memory.recall.injected");
    REQUIRE(recall_event != nullptr);
    const auto& payload = recall_event->at("payload");
    CHECK(payload.value("memoryId", std::string()) == "fact.deploy");
    CHECK(payload.value("memoryLevel", std::string()) == "project");
    CHECK(payload.value("memoryUpdatedAt", std::string()).empty() == false);
    CHECK(payload.value("contentSha256", std::string()).size() == 64);
    CHECK(payload.value("messageRef", std::string()) == message_id);
    CHECK(recall_event->value("turnId", std::string()) == "turn-1");
    CHECK(hooks::Sha256Hex(snapshot_text) == payload.value("contentSha256", std::string()));

    // (c) context 采用:链接纳事件点名快照消息。
    bool admitted = false;
    for (const auto& line : lines) {
        if (line.value("type", std::string()) != "event") continue;
        if (line.value("kind", std::string()) != "context.input.applied") continue;
        const auto& refs = line.at("payload").at("addedMessageRefs");
        for (const auto& ref : refs) {
            if (ref.get<std::string>() == message_id) admitted = true;
        }
    }
    CHECK(admitted);

    // (d) 实际 request refs:prepared 的 inputMessageRefs 沿链带上快照消息
    //     (排在 durable user 之前——召回先于回合落账,链序如实)。
    const nlohmann::json* prepared = nullptr;
    std::string user_message_id;
    for (const auto& line : lines) {
        if (line.value("type", std::string()) != "event") continue;
        if (line.value("kind", std::string()) != "model.request.prepared") continue;
        if (line.value("requestId", std::string()) != request_id) continue;
        prepared = &line;
    }
    REQUIRE(prepared != nullptr);
    const auto& input_refs = prepared->at("payload").at("inputMessageRefs");
    REQUIRE(input_refs.size() == 2);
    CHECK(input_refs[0].get<std::string>() == message_id);
    const std::string second_ref = input_refs[1].get<std::string>();
    for (const auto& line : lines) {
        if (line.value("type", std::string()) != "message") continue;
        if (line.value("messageId", std::string()) != second_ref) continue;
        user_message_id = second_ref;
        CHECK(line.value("origin", std::string()) == "human");
    }
    REQUIRE(user_message_id == second_ref);
    turn->EndTurn(/*ok=*/true, /*cancelled=*/false, "");

    // (e) 验卷:整卷哈希链不破。
    const auto report = trajectory::v3::VerifyV3File(*stream);
    CHECK(report.ok);
}

// ---------------------------------------------------------------------------
// 2. 快照不变性:记忆改版,旧请求的解释不换正文
// ---------------------------------------------------------------------------
TEST_CASE("v3 快照不变性: 记忆改版后旧消息行一字不动") {
    EnvGuard v3gate("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("immutable");
    MemoryFixture memory_fixture(root, "deploy 走 build.sh。");

    auto opened = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(opened.has_value());
    TrajectorySessionLedger& ledger = *opened;
    app::MemoryLedgerBridge bridge(ledger);
    memory_fixture.store->set_accounting(&bridge);

    REQUIRE_FALSE(memory_fixture.store
                      ->BuildTurnContext("deploy 怎么跑", memory_fixture.repo,
                                         memory::QueryOrigin::User, false, "turn-1")
                      .empty());
    const auto stream = trajectory::v3::FindV3SessionStream(ledger.session_dir());
    REQUIRE(stream.has_value());
    std::string first_snapshot_line;
    std::string first_sha;
    for (const std::string& line : *trajectory::ReadJournalLines(*stream)) {
        if (line.find("context_runtime") == std::string::npos) continue;
        first_snapshot_line = line;
        first_sha = nlohmann::json::parse(line).at("message").at("content").get<std::string>();
    }
    REQUIRE_FALSE(first_snapshot_line.empty());

    // 用户手编正文改版(v2 → v3 措辞),索引重建,再召回一轮。
    const fs::path topic = memory_fixture.store->memory_dir() / "facts" / "deploy.md";
    const std::string topic_text = ReadText(topic);
    const std::string needle = "deploy 走 build.sh。";
    const std::size_t at = topic_text.find(needle);
    REQUIRE(at != std::string::npos);
    WriteText(topic, topic_text.substr(0, at) + "deploy 走 build.sh,已改成 v2。" +
                        topic_text.substr(at + needle.size()));
    REQUIRE(memory::RebuildMemoryIndex(memory_fixture.store->memory_dir()).has_value());
    REQUIRE_FALSE(memory_fixture.store
                      ->BuildTurnContext("deploy 怎么跑", memory_fixture.repo,
                                         memory::QueryOrigin::User, false, "turn-2")
                      .empty());

    bool first_line_intact = false;
    std::size_t snapshot_messages = 0;
    for (const std::string& line : *trajectory::ReadJournalLines(*stream)) {
        if (line.find("context_runtime") == std::string::npos) continue;
        ++snapshot_messages;
        if (line == first_snapshot_line) first_line_intact = true;
    }
    CHECK(snapshot_messages == 2);
    CHECK(first_line_intact);
    // 恢复口径:旧请求的解释钉在旧快照上,不拿新 topic 正文倒推。
    CHECK(first_sha.find("v2") == std::string::npos);
    const auto verify = trajectory::v3::VerifyV3File(*stream);
    CHECK(verify.ok);
}

// ---------------------------------------------------------------------------
// 3. 快照提交失败:本次不注入(fail-closed),账上不冒领
// ---------------------------------------------------------------------------
TEST_CASE("v3 快照提交失败: 本轮零注入,链不长,trace 记 snapshot_failed") {
    EnvGuard v3gate("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("failclosed");
    MemoryFixture memory_fixture(root, "deploy 走 build.sh。");

    std::atomic<bool> armed{false};
    auto ledger_options = LedgerOptions(root);
    ledger_options.v3_main_io_fault = [&armed]() -> std::optional<std::string> {
        if (armed.load()) return std::string("test.injected");
        return std::nullopt;
    };
    auto opened = TrajectorySessionLedger::Open(std::move(ledger_options));
    REQUIRE(opened.has_value());
    TrajectorySessionLedger& ledger = *opened;
    app::MemoryLedgerBridge bridge(ledger);
    memory_fixture.store->set_accounting(&bridge);

    armed.store(true);
    const std::string context = memory_fixture.store->BuildTurnContext(
        "deploy 怎么跑", memory_fixture.repo, memory::QueryOrigin::User, false, "turn-1");
    // 主任务按 optional 策略放行:整段不注入(零命中零脚手架),回合照跑。
    CHECK(context.empty());

    // trace 如实记 snapshot_failed,不装作"没跑过检索"。
    const memory::RecallTrace trace = memory_fixture.store->LastTrace();
    REQUIRE(trace.entries.size() == 1);
    CHECK(trace.entries[0].snapshot_failed);
    CHECK_FALSE(trace.entries[0].injected);

    // 账上不冒领:没有隐藏快照消息,没有召回事实,链只有根 system。
    const auto stream = trajectory::v3::FindV3SessionStream(ledger.session_dir());
    REQUIRE(stream.has_value());
    const auto lines = StreamLines(*stream);
    CHECK(FindRecallMessage(lines) == nullptr);
    CHECK(FindEvent(lines, "memory.recall.injected") == nullptr);
    const auto report = trajectory::v3::VerifyV3File(*stream);
    REQUIRE(report.ok);
    CHECK(report.context.chain.size() == 1);  // 只有根 system,记忆一个没进链
}

// ---------------------------------------------------------------------------
// 4. 派工冻结:父账记事实不进链
// ---------------------------------------------------------------------------
TEST_CASE("v3 派工冻结: 父账事实带 targetRunId 与快照,不写隐藏消息不进链") {
    EnvGuard v3gate("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("dispatch");
    MemoryFixture memory_fixture(root, "deploy 走 build.sh。");

    auto opened = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(opened.has_value());
    TrajectorySessionLedger& ledger = *opened;
    app::MemoryLedgerBridge bridge(ledger);
    memory_fixture.store->set_accounting(&bridge);

    const std::string frozen = memory_fixture.store->BuildTurnContextForDispatch(
        "查 deploy 的跑法", memory_fixture.repo, "agent-run-42");
    REQUIRE_FALSE(frozen.empty());

    const auto stream = trajectory::v3::FindV3SessionStream(ledger.session_dir());
    REQUIRE(stream.has_value());
    const auto lines = StreamLines(*stream);

    const nlohmann::json* recall_event = FindEvent(lines, "memory.recall.injected");
    REQUIRE(recall_event != nullptr);
    const auto& payload = recall_event->at("payload");
    CHECK(payload.value("targetRunId", std::string()) == "agent-run-42");
    CHECK(payload.value("memoryId", std::string()) == "fact.deploy");
    // 小正文按合同内联;指纹对得上。
    CHECK(payload.value("snapshotInline", std::string()).empty() == false);
    CHECK(hooks::Sha256Hex(payload.value("snapshotInline", std::string())) ==
          payload.value("contentSha256", std::string()));
    CHECK(payload.contains("messageRef") == false);

    // 父账不冒领:没有隐藏消息,没有为它落的链接纳;真实回合的请求引用
    // 里也查无此段(冻结正文发给了孩子,父模型没见过)。
    CHECK(FindRecallMessage(lines) == nullptr);
    bool admitted_memory = false;
    for (const auto& line : lines) {
        if (line.value("kind", std::string()) != "context.input.applied") continue;
        if (line.value("type", std::string()) != "event") continue;
        admitted_memory = true;  // 本场没有任何 user 消息,出现即异常
    }
    CHECK_FALSE(admitted_memory);

    auto turn = ledger.NewTurnBridge(TrajectoryTurnBridge::Identity{"kimi", "responses", "terminal"});
    REQUIRE(turn != nullptr);
    turn->BeginTurn("turn-1", "external_user");
    const api::Message user = UserMessage("查 deploy 的跑法");
    turn->RecordInput(user);
    api::Request request;
    request.model = "kimi-k2.6";
    request.system = LedgerOptions(root).v3_system_content;
    request.messages.push_back(user);
    const std::string request_id = turn->OnRequestPrepared(request, agent::RequestPreparedContext{});
    REQUIRE_FALSE(request_id.empty());
    for (const auto& line : StreamLines(*stream)) {
        if (line.value("kind", std::string()) != "model.request.prepared") continue;
        if (line.value("requestId", std::string()) != request_id) continue;
        // 链上只有 durable user 一条输入;派工冻结的记忆不在引用里。
        CHECK(line.at("payload").at("inputMessageRefs").size() == 1);
    }
    const auto report = trajectory::v3::VerifyV3File(*stream);
    CHECK(report.ok);
}

// ---------------------------------------------------------------------------
// 4b. 派工冻结(大正文):超 512B 走内容寻址 blob,snapshotRef 可读回
// ---------------------------------------------------------------------------
TEST_CASE("v3 派工冻结大正文: snapshotRef 进 blob,指纹对得上") {
    EnvGuard v3gate("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    // 路径要短:blob 内容寻址文件名 70 字符,Windows 260 上限经不起深临时
    // 目录折腾(同款见 unit.app.test_memory_ledger_bridge)。
    std::error_code ec;
    const auto root = fs::temp_directory_path() / "lmb-v3-mr-bigdispatch";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    std::string big_content = "deploy 走 build.sh,各环境差异如下。\n";
    while (big_content.size() < 700) big_content += "补充行:冻结快照须与今天的 Memory 分账。\n";
    MemoryFixture memory_fixture(root, big_content);

    auto opened = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(opened.has_value());
    TrajectorySessionLedger& ledger = *opened;
    app::MemoryLedgerBridge bridge(ledger);
    memory_fixture.store->set_accounting(&bridge);

    REQUIRE_FALSE(memory_fixture.store->BuildTurnContextForDispatch("查 deploy 的跑法",
                                                                    memory_fixture.repo, "agent-run-7")
                      .empty());

    const auto stream = trajectory::v3::FindV3SessionStream(ledger.session_dir());
    REQUIRE(stream.has_value());
    const auto lines = StreamLines(*stream);
    const nlohmann::json* recall_event = FindEvent(lines, "memory.recall.injected");
    REQUIRE(recall_event != nullptr);
    const auto& payload = recall_event->at("payload");
    CHECK(payload.contains("snapshotInline") == false);
    REQUIRE(payload.contains("snapshotRef"));
    const std::string snapshot_ref = payload.value("snapshotRef", std::string());
    CHECK(snapshot_ref.find("..") == std::string::npos);  // session 相对路径,不越根

    // 快照可从 session artifacts 读回,指纹对得上(恢复不依赖 memory 仓)。
    trajectory::BlobRef ref;
    ref.sha256 = payload.value("contentSha256", std::string());
    ref.size = payload.value("injectedBytes", std::uint64_t{0});
    trajectory::BlobStore blobs(ledger.session_dir() / "artifacts");
    const auto snapshot = blobs.ReadVerified(ref);
    REQUIRE(snapshot.has_value());
    CHECK(hooks::Sha256Hex(*snapshot) == ref.sha256);
    CHECK(snapshot->find("冻结快照须与今天的 Memory 分账") != std::string::npos);
    const auto report = trajectory::v3::VerifyV3File(*stream);
    CHECK(report.ok);
}

// ---------------------------------------------------------------------------
// 5. 写入因果边:requested 不冒充 queued/committed
// ---------------------------------------------------------------------------
TEST_CASE("v3 写入因果边: memory.save.requested 落 v3 事实行,引用全限定") {
    EnvGuard v3gate("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("saveedge");
    MemoryFixture memory_fixture(root, "deploy 走 build.sh。");

    auto opened = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(opened.has_value());
    TrajectorySessionLedger& ledger = *opened;
    app::MemoryLedgerBridge bridge(ledger);
    memory_fixture.store->set_accounting(&bridge);

    memory::SaveRequest request;
    request.kind = memory::MemoryKind::Fact;
    request.id = "fact.tools";
    request.title = "工具表";
    request.summary = "工具表";
    request.content = "工具表在 src/tools 下。";
    request.confidence = "verified";
    REQUIRE(memory_fixture.store->EnqueueSave(request).has_value());

    const auto stream = trajectory::v3::FindV3SessionStream(ledger.session_dir());
    REQUIRE(stream.has_value());
    const auto lines = StreamLines(*stream);
    const nlohmann::json* edge = FindEvent(lines, "memory.save.requested");
    REQUIRE(edge != nullptr);
    const auto& payload = edge->at("payload");
    CHECK(payload.at("request").value("operation", std::string()) == "upsert");
    CHECK(payload.at("request").value("memoryId", std::string()) == "fact.tools");
    CHECK(payload.at("request").value("layer", std::string()) == "project");
    CHECK(payload.value("originator", std::string()) == "model_tool");
    // requested 只是因果边:不冒充排队与落盘(那两态看 receipted/lifecycle)。
    CHECK(payload.contains("jobId") == false);
    CHECK(payload.contains("outcome") == false);

    // 全限定引用进了 pending job(worker 落进主题的 source_sessions)。
    std::error_code ec;
    fs::directory_iterator it(memory_fixture.home / "memory-jobs" / "pending", ec);
    REQUIRE_FALSE(ec);
    std::string job_ref;
    for (const auto& item : it) {
        const auto job = nlohmann::json::parse(ReadText(item.path()));
        job_ref = job.value("source_event_ref", std::string());
    }
    REQUIRE_FALSE(job_ref.empty());
    CHECK(job_ref.starts_with("workspace_key=" + ledger.workspace_key() + "/session_id=" +
                              ledger.session_id() + "/run_id="));
    CHECK(job_ref.find("event_id=evt-") != std::string::npos);
    const auto report = trajectory::v3::VerifyV3File(*stream);
    CHECK(report.ok);
}

// ---------------------------------------------------------------------------
// 6. 老调用方无回合号:注入不丢,writer 号池兜底
// ---------------------------------------------------------------------------
TEST_CASE("v3 无回合号召回: writer 号池兜底署名,注入本体不丢") {
    EnvGuard v3gate("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("noturn");
    MemoryFixture memory_fixture(root, "deploy 走 build.sh。");

    auto opened = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(opened.has_value());
    TrajectorySessionLedger& ledger = *opened;
    app::MemoryLedgerBridge bridge(ledger);
    memory_fixture.store->set_accounting(&bridge);

    const std::string context =
        memory_fixture.store->BuildTurnContext("deploy 怎么跑", memory_fixture.repo);
    REQUIRE_FALSE(context.empty());

    const auto stream = trajectory::v3::FindV3SessionStream(ledger.session_dir());
    REQUIRE(stream.has_value());
    const nlohmann::json* recall_message = FindRecallMessage(StreamLines(*stream));
    REQUIRE(recall_message != nullptr);
    // writer 自家号池(zero-pad,与宿主 turn-<n> 不撞名):署名弱一档,注入不丢。
    const std::string turn_id = recall_message->value("turnId", std::string());
    CHECK(turn_id.rfind("turn-00000", 0) == 0);
    const auto report = trajectory::v3::VerifyV3File(*stream);
    CHECK(report.ok);
}

// ---------------------------------------------------------------------------
// 7. schema 合同:名字稳定、statusless、入册
// ---------------------------------------------------------------------------
TEST_CASE("v3 schema: memory.recall.injected / memory.save.requested 入册且 statusless") {
    using lubancode::trajectory::v3::AllEventKindsV3;
    using lubancode::trajectory::v3::EventKindV3;
    using lubancode::trajectory::v3::EventKindV3FromName;
    using lubancode::trajectory::v3::EventKindV3Name;
    using lubancode::trajectory::v3::RequiredStatusForKind;

    CHECK(EventKindV3Name(EventKindV3::MemoryRecallInjected) == "memory.recall.injected");
    CHECK(EventKindV3Name(EventKindV3::MemorySaveRequested) == "memory.save.requested");
    CHECK(EventKindV3FromName("memory.recall.injected") == EventKindV3::MemoryRecallInjected);
    CHECK(EventKindV3FromName("memory.save.requested") == EventKindV3::MemorySaveRequested);
    CHECK(RequiredStatusForKind(EventKindV3::MemoryRecallInjected).has_value() == false);
    CHECK(RequiredStatusForKind(EventKindV3::MemorySaveRequested).has_value() == false);
    bool recall_in_book = false;
    bool save_in_book = false;
    for (const EventKindV3 kind : AllEventKindsV3()) {
        if (kind == EventKindV3::MemoryRecallInjected) recall_in_book = true;
        if (kind == EventKindV3::MemorySaveRequested) save_in_book = true;
    }
    CHECK(recall_in_book);
    CHECK(save_in_book);
}
