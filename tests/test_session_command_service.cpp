// SessionCommandService(会话管理器单第六步)的测试:runtime 侧的会话
// 查询/搬删命令服务。
//   - thread.list:查询形状(scope/state/sort/search/limit)与结构化
//     SessionSummary(稳定时间串,不算相对时间);
//   - thread.archive/unarchive/delete:typed command 收口,稳定错误码;
//   - delete 的 confirm 门:不带 confirm 一律拒绝不动盘;
//   - 合同枚举往返:thread.archive/unarchive/delete 与 thread.deleted
//     事件,序列化稳定字符串;
//   - 终端与 app-server 同一查询给同一份 id/顺序/状态(验收:两者对同一
//     query 给出相同 id/顺序/状态)。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "agent/session_catalog.hpp"
#include "agent/session_store.hpp"
#include "runtime/command.hpp"
#include "runtime/event.hpp"
#include "runtime/session_command_service.hpp"

using namespace lubancode;

namespace {

// 文件名(可能含中文 id)走 u8 通道:窄串在 Windows 按 ACP 解码成乱码名,
// 与 u8 的服务侧对不上账(HOT 单同款教训)。
std::filesystem::path U8Name(const std::string& s) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string PathUtf8(const std::filesystem::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

struct TempSessionsDir {
    std::filesystem::path base;

    TempSessionsDir(const char* tag) {
        base = std::filesystem::temp_directory_path() / (std::string("lubancode_scs_") + tag);
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
        std::filesystem::create_directories(base);
    }
    ~TempSessionsDir() {
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
    }
    std::string str() const { return PathUtf8(base); }
};

void WriteSession(const TempSessionsDir& dir, const std::string& id, const std::string& title,
                  const std::string& cwd, const std::string& started_at) {
    agent::SessionMeta meta;
    meta.wire = "anthropic";
    meta.model = "m1";
    meta.cwd = cwd;
    meta.started_at = started_at;
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{"首句" + id});
    std::string content = agent::SerializeSessionMeta(meta) + "\n" +
                          agent::SerializeSessionMessage(message, started_at) + "\n";
    if (!title.empty()) {
        content += agent::SerializeTitleEvent(title, started_at) + "\n";
    }
    std::ofstream f(dir.base / U8Name(id + ".jsonl"), std::ios::binary);
    f << content;
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
    TempSessionsDir dir("list");
    WriteSession(dir, "20260820-101010-甲", "甲的场", "D:/房", "2026-08-20 10:10:10");
    WriteSession(dir, "20260821-111111-乙", "", "D:/房", "2026-08-21 11:11:11");
    WriteSession(dir, "20260822-121212-丙", "丙在别处", "E:/别的", "2026-08-22 12:12:12");

    const runtime::SessionCommandService service(dir.str());
    const auto outcome = service.ListThreads(nlohmann::json{{"scope", "all"}});
    REQUIRE(outcome.accepted);
    const auto& threads = outcome.payload.at("threads");
    CHECK(outcome.payload.at("total") == 3);
    REQUIRE(threads.size() == 3);
    // updated 倒序:丙(0822)在头,甲(0820)在尾。
    CHECK(threads[0].at("threadId") == "20260822-121212-丙");
    CHECK(threads[2].at("threadId") == "20260820-101010-甲");
    // 结构化字段:标题/首句/cwd/模型/时间串/消息数/状态/健康。
    CHECK(threads[2].at("title") == "甲的场");
    CHECK(threads[2].at("firstUserText").get<std::string>().starts_with("首句"));
    CHECK(threads[2].at("cwd") == "D:/房");
    CHECK(threads[2].at("model") == "m1");
    CHECK(threads[2].at("createdAt") == "2026-08-20 10:10:10");  // 稳定串,不是相对时间
    CHECK(threads[2].at("messageCount") == 1);
    CHECK(threads[2].at("state") == "active");
    CHECK(threads[2].at("health") == "ok");

    // 搜索:标题命中,只留一场。
    const auto searched = service.ListThreads(nlohmann::json{{"scope", "all"}, {"search", "丙在"}});
    CHECK(searched.payload.at("total") == 1);
    CHECK(searched.payload.at("threads")[0].at("threadId") == "20260822-121212-丙");

    // limit 截页,total 不受分页影响。
    const auto paged = service.ListThreads(nlohmann::json{{"scope", "all"}, {"limit", 1}});
    CHECK(paged.payload.at("total") == 3);
    CHECK(paged.payload.at("threads").size() == 1);

    // HandleCommand 总入口:thread.list 同一形状同一份账(list 只读,
    // 非 const 服务上调用——HandleCommand 统一收口搬删与查询)。
    runtime::SessionCommandService mutable_service(dir.str());
    runtime::ClientCommand command;
    command.kind = runtime::ClientCommandKind::ListThreads;
    command.payload = {{"scope", "all"}};
    const auto receipt = mutable_service.HandleCommand(command);
    CHECK(receipt.accepted);
    CHECK(receipt.payload.at("total") == 3);
}

TEST_CASE("thread.archive/unarchive/delete: typed command 收口与错误码") {
    TempSessionsDir dir("manage");
    WriteSession(dir, "20260820-101010-甲", "甲的场", "D:/房", "2026-08-20 10:10:10");
    runtime::SessionCommandService service(dir.str());

    // archive:成功,payload 带 state=archived。
    auto outcome = service.ArchiveThread("20260820-101010-甲");
    REQUIRE(outcome.accepted);
    CHECK(outcome.payload.at("state") == "archived");
    CHECK(std::filesystem::exists(dir.base / "archive" / U8Name("20260820-101010-甲.jsonl")));

    // 归档后 list(active)不见,archived 见。
    const runtime::SessionCommandService const_view(dir.str());
    CHECK(const_view.ListThreads({{"scope", "all"}}).payload.at("total") == 0);
    CHECK(const_view.ListThreads({{"scope", "all"}, {"state", "archived"}}).payload.at("total") == 1);

    // unarchive:成功,payload 带 state=active。
    outcome = service.UnarchiveThread("20260820-101010-甲");
    REQUIRE(outcome.accepted);
    CHECK(outcome.payload.at("state") == "active");

    // delete:不带 confirm 一律拒绝,盘上不动。
    runtime::ClientCommand no_confirm;
    no_confirm.kind = runtime::ClientCommandKind::DeleteThread;
    no_confirm.thread_id = "20260820-101010-甲";
    const auto refused = service.HandleCommand(no_confirm);
    CHECK_FALSE(refused.accepted);
    CHECK(refused.error_code == "confirmation_required");
    CHECK(std::filesystem::exists(dir.base / U8Name("20260820-101010-甲.jsonl")));

    // 带 confirm:删掉。
    runtime::ClientCommand confirmed = no_confirm;
    confirmed.payload = {{"confirm", true}};
    const auto deleted = service.HandleCommand(confirmed);
    CHECK(deleted.accepted);
    CHECK_FALSE(std::filesystem::exists(dir.base / U8Name("20260820-101010-甲.jsonl")));

    // not_found:删不存在的。
    const auto missing = service.DeleteThread("99999999-000000-无", {{"confirm", true}});
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

TEST_CASE("终端与 app-server 同一查询同一份账:catalog 直查与 service 对齐") {
    TempSessionsDir dir("parity");
    WriteSession(dir, "20260820-101010-甲", "甲", "D:/房", "2026-08-20 10:10:10");
    WriteSession(dir, "20260821-111111-乙", "乙", "D:/房", "2026-08-21 11:11:11");

    // 终端路(SessionCatalog 直查,picker 吃这份)。
    agent::SessionCatalog catalog(dir.str());
    catalog.Scan();
    agent::SessionQuery query;
    query.scope = agent::SessionScope::All;
    query.sort = agent::SessionSort::Updated;
    query.limit = 0;
    const auto page = catalog.Query(query);

    // 协议路(SessionCommandService,app-server 吃这份)。
    const runtime::SessionCommandService service(dir.str());
    const auto outcome = service.ListThreads({{"scope", "all"}});

    REQUIRE(page.entries.size() == outcome.payload.at("threads").size());
    for (std::size_t i = 0; i < page.entries.size(); ++i) {
        CHECK(page.entries[i].id == outcome.payload.at("threads")[i].at("threadId").get<std::string>());
        CHECK(page.entries[i].title == outcome.payload.at("threads")[i].at("title").get<std::string>());
        CHECK(page.entries[i].updated_at ==
              outcome.payload.at("threads")[i].at("updatedAt").get<std::string>());
    }
}

TEST_CASE("sessions_dir 空:service 拒搬删,list 给空表") {
    const std::string empty_dir;
    runtime::SessionCommandService service(empty_dir);
    CHECK(service.ListThreads({}).accepted);
    CHECK(service.ListThreads({}).payload.at("total") == 0);
    CHECK_FALSE(service.ArchiveThread("x").accepted);
    CHECK_FALSE(service.DeleteThread("x", {{"confirm", true}}).accepted);
}
