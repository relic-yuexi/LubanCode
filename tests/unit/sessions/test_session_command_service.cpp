// SessionCommandService(会话管理器单第六步;P0-2 换 workspace 新账)的
// 测试:runtime 侧的会话查询/搬删命令服务。
//   - thread.list:查询形状(scope/state/sort/search/limit)与结构化
//     SessionSummary(稳定时间串,不算相对时间),数据源是 workspace
//     可重建索引;
//   - thread.archive/unarchive/delete:typed command 收口,lifecycle +
//     状态图 + tombstone,稳定错误码;
//   - delete 的 confirm 门:不带 confirm 一律拒绝不动盘;
//   - 合同枚举往返:thread.archive/unarchive/delete 与 thread.deleted
//     事件,序列化稳定字符串。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "runtime/command.hpp"
#include "runtime/event.hpp"
#include "runtime/session_command_service.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/directory.hpp"
#include "workspace/identity.hpp"

using namespace lubancode;

namespace {

struct TempWorkspacesRoot {
    std::filesystem::path base;

    TempWorkspacesRoot(const char* tag) {
        base = std::filesystem::temp_directory_path() / (std::string("lubancode_scs_") + tag);
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
        std::filesystem::create_directories(base);
    }
    ~TempWorkspacesRoot() {
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
    }
    std::filesystem::path root() const { return base / "workspaces"; }
};

// 造一场封了口的会话:turn 一问一答 + 可选标题。正常 Close 后是 closed
// 态(索引可见、可搬删)。间隔 2ms 造场,updated_at 的排序才稳定。
std::string MakeSession(const TempWorkspacesRoot& dir, const std::string& title,
                        const std::string& cwd) {
    static int counter = 0;
    ++counter;
    if (counter % 2 == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    std::error_code ec;
    std::filesystem::create_directories(dir.base / "repo", ec);
    runtime::TrajectorySessionLedger::Options options;
    options.workspaces_root = dir.root();
    options.workspace_identity = workspace::MakeFallbackIdentity(dir.base / "repo");
    options.lubancode_version = "test";
    options.launch_cwd = cwd;
    auto ledger = runtime::TrajectorySessionLedger::Open(options);
    REQUIRE(ledger.has_value());
    auto bridge = ledger->NewTurnBridge({"demo", "responses", "terminal"});
    REQUIRE(bridge != nullptr);
    bridge->BeginTurn("turn-1", "external_user");
    api::Message input;
    input.role = api::Role::User;
    input.content.push_back(api::TextBlock{"首句 " + title});
    bridge->RecordInput(input);
    api::Request prepared;
    prepared.model = "m1";
    const std::string request = bridge->OnRequestPrepared(prepared, agent::RequestPreparedContext{});
    REQUIRE_FALSE(request.empty());
    bridge->OnRequestSent(request);
    api::Message answer;
    answer.role = api::Role::Assistant;
    answer.content.push_back(api::TextBlock{"回一句"});
    REQUIRE(bridge->OnOutputCompleted(request, answer, "end_turn", "resp-1"));
    bridge->EndTurn(true, false, "done");
    if (!title.empty()) {
        ledger->RecordTitleChanged(title, "");
    }
    const std::string id = ledger->session_id();
    (void)ledger->CloseSession("exit");
    return id;
}

runtime::SessionCommandService::Options ServiceOptions(const TempWorkspacesRoot& dir) {
    runtime::SessionCommandService::Options options;
    options.workspaces_root = dir.root();
    options.workspace_key =
        workspace::MakeFallbackIdentity(dir.base / "repo").workspace_key;
    return options;
}

}  // namespace

TEST_CASE("合同枚举:thread.archive/unarchive/delete 与 thread.deleted 往返") {
    // 命令枚举 <-> 稳定字符串。
    CHECK(runtime::ToString(runtime::ClientCommandKind::ArchiveThread) == "thread.archive");
    CHECK(runtime::ToString(runtime::ClientCommandKind::UnarchiveThread) == "thread.unarchive");
    CHECK(runtime::ToString(runtime::ClientCommandKind::DeleteThread) == "thread.delete");
    runtime::ClientCommandKind kind = runtime::ClientCommandKind::StartTurn;
    CHECK(runtime::ParseClientCommandKind("thread.archive", kind));
    CHECK(kind == runtime::ClientCommandKind::ArchiveThread);
    CHECK(runtime::ParseClientCommandKind("thread.unarchive", kind));
    CHECK(kind == runtime::ClientCommandKind::UnarchiveThread);
    CHECK(runtime::ParseClientCommandKind("thread.delete", kind));
    CHECK(kind == runtime::ClientCommandKind::DeleteThread);

    // 事件枚举 <-> 稳定字符串。
    CHECK(runtime::ToString(runtime::ServerEventKind::ThreadDeleted) == "thread.deleted");
    runtime::ServerEventKind event_kind = runtime::ServerEventKind::Error;
    CHECK(runtime::ParseServerEventKind("thread.deleted", event_kind));
    CHECK(event_kind == runtime::ServerEventKind::ThreadDeleted);
    runtime::ServerEvent deleted_event;
    deleted_event.kind = runtime::ServerEventKind::ThreadDeleted;
    CHECK(runtime::LayerOf(deleted_event) == runtime::EventLayer::Thread);

    // 序列化往返保真(thread.delete 带 confirm payload)。
    runtime::ClientCommand command;
    command.kind = runtime::ClientCommandKind::DeleteThread;
    command.thread_id = "20260820-101010-甲";
    command.payload = {{"confirm", true}};
    const auto round = runtime::ClientCommand::from_json(command.to_json());
    CHECK(round.kind == runtime::ClientCommandKind::DeleteThread);
    CHECK(round.thread_id == "20260820-101010-甲");
    CHECK(round.payload.value("confirm", false) == true);
}

TEST_CASE("thread.list: 结构化 SessionSummary,稳定时间串,搜索/排序/limit") {
    TempWorkspacesRoot dir("list");
    const std::string id_a = MakeSession(dir, "甲的场", "D:/房");
    const std::string id_b = MakeSession(dir, "", "D:/房");
    const std::string id_c = MakeSession(dir, "丙在别处", "E:/别的");
    REQUIRE_FALSE(id_a.empty());
    REQUIRE_FALSE(id_b.empty());
    REQUIRE_FALSE(id_c.empty());

    const runtime::SessionCommandService service(ServiceOptions(dir));
    const auto outcome = service.ListThreads(nlohmann::json{{"scope", "all"}});
    REQUIRE(outcome.accepted);
    const auto& threads = outcome.payload.at("threads");
    CHECK(outcome.payload.at("total") == 3);
    REQUIRE(threads.size() == 3);
    // updated 倒序:丙(最后造)在头,甲(最先造)在尾。
    CHECK(threads[0].at("threadId") == id_c);
    CHECK(threads[2].at("threadId") == id_a);
    // 结构化字段:标题/首句/cwd/模型/时间串/消息数/状态/健康。
    CHECK(threads[2].at("title") == "甲的场");
    CHECK(threads[2].at("firstUserText").get<std::string>().starts_with("首句"));
    CHECK(threads[2].at("cwd") == "D:/房");
    CHECK(threads[2].at("model").get<std::string>().empty() == false);
    CHECK(threads[2].at("createdAt").get<std::string>().size() == 19);  // 稳定串,不是相对时间
    CHECK(threads[2].at("messageCount") == 2);                           // 一问一答
    CHECK(threads[2].at("state") == "active");
    CHECK(threads[2].at("health") == "ok");

    // 搜索:标题命中,只留一场。
    const auto searched = service.ListThreads(nlohmann::json{{"scope", "all"}, {"search", "丙在"}});
    CHECK(searched.payload.at("total") == 1);
    CHECK(searched.payload.at("threads")[0].at("threadId") == id_c);

    // limit 截页,total 不受分页影响。
    const auto paged = service.ListThreads(nlohmann::json{{"scope", "all"}, {"limit", 1}});
    CHECK(paged.payload.at("total") == 3);
    CHECK(paged.payload.at("threads").size() == 1);

    // HandleCommand 总入口:thread.list 同一形状同一份账(list 只读,
    // 非 const 服务上调用——HandleCommand 统一收口搬删与查询)。
    runtime::SessionCommandService mutable_service(ServiceOptions(dir));
    runtime::ClientCommand command;
    command.kind = runtime::ClientCommandKind::ListThreads;
    command.payload = {{"scope", "all"}};
    const auto receipt = mutable_service.HandleCommand(command);
    CHECK(receipt.accepted);
    CHECK(receipt.payload.at("total") == 3);
}

TEST_CASE("thread.archive/unarchive/delete: typed command 收口与错误码") {
    TempWorkspacesRoot dir("manage");
    const std::string id = MakeSession(dir, "甲的场", "D:/房");
    runtime::SessionCommandService service(ServiceOptions(dir));

    // archive:成功,payload 带 state=archived;目录不搬,manifest 转态。
    auto outcome = service.ArchiveThread(id);
    REQUIRE(outcome.accepted);
    CHECK(outcome.payload.at("state") == "archived");

    // 归档后 list(active)不见,archived 见。
    const runtime::SessionCommandService const_view(ServiceOptions(dir));
    CHECK(const_view.ListThreads({{"scope", "all"}}).payload.at("total") == 0);
    CHECK(const_view.ListThreads({{"scope", "all"}, {"state", "archived"}}).payload.at("total") == 1);

    // unarchive:成功,payload 带 state=active。
    outcome = service.UnarchiveThread(id);
    REQUIRE(outcome.accepted);
    CHECK(outcome.payload.at("state") == "active");

    // delete:不带 confirm 一律拒绝,盘上不动。
    runtime::ClientCommand no_confirm;
    no_confirm.kind = runtime::ClientCommandKind::DeleteThread;
    no_confirm.thread_id = id;
    const auto refused = service.HandleCommand(no_confirm);
    CHECK_FALSE(refused.accepted);
    CHECK(refused.error_code == "confirmation_required");

    // 带 confirm:删掉(tombstone 留痕,目录消失)。
    runtime::ClientCommand confirmed = no_confirm;
    confirmed.payload = {{"confirm", true}};
    const auto deleted = service.HandleCommand(confirmed);
    CHECK(deleted.accepted);

    // not_found:删不存在的。
    const auto missing = service.DeleteThread("99999999-000000-XXXXXX", {{"confirm", true}});
    CHECK_FALSE(missing.accepted);
    CHECK(missing.error_code == "not_found");

    // invalid_request:空 id;别的 kind 不归本服务。
    const auto bad = service.ArchiveThread("");
    CHECK_FALSE(bad.accepted);
    CHECK(bad.error_code == "invalid_request");
    runtime::ClientCommand other;
    other.kind = runtime::ClientCommandKind::StartTurn;
    CHECK_FALSE(service.HandleCommand(other).accepted);
}

TEST_CASE("service 与终端 picker 同一份账:索引直查与 service 对齐") {
    TempWorkspacesRoot dir("parity");
    const std::string id_a = MakeSession(dir, "甲", "D:/房");
    const std::string id_b = MakeSession(dir, "乙", "D:/房");
    REQUIRE_FALSE(id_a.empty());
    REQUIRE_FALSE(id_b.empty());

    // 终端路(/sessions、picker 吃索引这份)。
    trajectory::SessionIndexQuery query;
    query.current_workspace_key = workspace::MakeFallbackIdentity(dir.base / "repo").workspace_key;
    const auto page = trajectory::QueryWorkspaceSessions(dir.root(), query);

    // 协议路(SessionCommandService,app-server 吃这份)。
    const runtime::SessionCommandService service(ServiceOptions(dir));
    const auto outcome = service.ListThreads({{"scope", "all"}});

    REQUIRE(page.entries.size() == outcome.payload.at("threads").size());
    for (std::size_t i = 0; i < page.entries.size(); ++i) {
        CHECK(page.entries[i].session_id ==
              outcome.payload.at("threads")[i].at("threadId").get<std::string>());
        CHECK(page.entries[i].title ==
              outcome.payload.at("threads")[i].at("title").get<std::string>());
    }
}

TEST_CASE("workspaces 根空:service 拒搬删,list 给空表") {
    runtime::SessionCommandService::Options empty_options;
    runtime::SessionCommandService service(empty_options);
    CHECK(service.ListThreads({}).accepted);
    CHECK(service.ListThreads({}).payload.at("total") == 0);
    CHECK_FALSE(service.ArchiveThread("x").accepted);
    CHECK_FALSE(service.DeleteThread("x", {{"confirm", true}}).accepted);
}
