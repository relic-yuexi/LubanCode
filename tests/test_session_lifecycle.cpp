// SessionLifecycle(会话管理器单第四、五步)的测试:搬与删的唯一收口。
//   - 引用解析:完整 id / 唯一前缀 / 标题唯一命中 / 重名列短 id(ambiguous)
//     / 认不出(NotFound);
//   - archive:根 -> archive/ 字节原样;重复归档幂等;目标同名拒绝覆盖;
//   - unarchive:搬回根;根同名拒绝;
//   - delete:缺确认一律 ConfirmationRequired 不动盘;确认后只删目标一场;
//   - 路径校验:分隔符/点点冒充 id、后缀不对,一律拒绝;
//   - Windows 开句柄回归:活动句柄先关再搬(回调被调、拒绝时不搬);
//   - SessionCatalog 的 Archived 视图:根与 archive 各列各的,默认查询
//     不掺归档。
//
// MSVC 盲区:临时目录先关柄再删、remove_all 用 error_code 形态(文件写完
// 显式 close)。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "agent/session_catalog.hpp"
#include "agent/session_lifecycle.hpp"
#include "agent/session_store.hpp"

using namespace lubancode;

namespace {

std::string PathUtf8(const std::filesystem::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

struct TempSessionsDir {
    std::filesystem::path base;

    TempSessionsDir(const char* tag) {
        base = std::filesystem::temp_directory_path() / (std::string("lubancode_lifecycle_") + tag);
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
        std::filesystem::create_directories(base);
    }
    ~TempSessionsDir() {
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
    }
    std::string str() const { return PathUtf8(base); }
    std::filesystem::path archive() const { return base / "archive"; }
};

// 写一场会话(写完关柄——MSVC 下开着的句柄会卡 remove_all)。返回原内容。
// 文件名一律走 u8string:窄串拼 path 在 MSVC 下按 ANSI 代码页解码,中文
// slug 会写出乱码名(旧 PathOf 同错相抵才没现形);产品侧已全走 u8,
// 夹具必须同口径,写出的才是真名。
std::filesystem::path Utf8Name(const std::string& utf8) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string WriteSession(const TempSessionsDir& dir, const std::string& id, const std::string& cwd,
                         const std::string& started_at) {
    agent::SessionMeta meta;
    meta.wire = "anthropic";
    meta.model = "m1";
    meta.cwd = cwd;
    meta.started_at = started_at;
    const std::string line = agent::SerializeSessionMessage(
        []() {
            api::Message m;
            m.role = api::Role::User;
            m.content.push_back(api::TextBlock{"你好"});
            return m;
        }(),
        started_at);
    const std::string content = agent::SerializeSessionMeta(meta) + "\n" + line + "\n";
    std::ofstream f(dir.base / Utf8Name(id + ".jsonl"), std::ios::binary);
    f << content;
    return content;
}

}  // namespace

// ---------------------------------------------------------------------------
// 引用解析(纯函数)
// ---------------------------------------------------------------------------

TEST_CASE("ResolveSessionRef: 完整 id > 唯一前缀 > 标题唯一;重名/认不出") {
    std::vector<agent::SessionRefCandidate> candidates = {
        {"20260820-101010-甲", "/tmp/a.jsonl", "房甲的场"},
        {"20260821-111111-乙", "/tmp/b.jsonl", "同名标题"},
        {"20260822-121212-丙", "/tmp/c.jsonl", "同名标题"},
    };
    bool ambiguous = false;

    // 完整 id。
    auto hit = agent::ResolveSessionRef(candidates, "20260820-101010-甲", ambiguous);
    REQUIRE(hit.has_value());
    CHECK(hit->size() == 1);
    CHECK((*hit)[0].id == "20260820-101010-甲");
    CHECK_FALSE(ambiguous);

    // 唯一前缀(只命中乙)。
    hit = agent::ResolveSessionRef(candidates, "20260821", ambiguous);
    REQUIRE(hit.has_value());
    CHECK(hit->size() == 1);
    CHECK((*hit)[0].id == "20260821-111111-乙");

    // 标题唯一命中。
    hit = agent::ResolveSessionRef(candidates, "房甲的场", ambiguous);
    REQUIRE(hit.has_value());
    CHECK((*hit)[0].id == "20260820-101010-甲");

    // 标题重名:ambiguous,两场都列出来,绝不猜。
    hit = agent::ResolveSessionRef(candidates, "同名标题", ambiguous);
    REQUIRE(hit.has_value());
    CHECK(ambiguous);
    CHECK(hit->size() == 2);

    // 前缀多义:ambiguous。
    ambiguous = false;
    hit = agent::ResolveSessionRef(candidates, "2026", ambiguous);
    REQUIRE(hit.has_value());
    CHECK(ambiguous);
    CHECK(hit->size() == 3);

    // 认不出。
    ambiguous = false;
    hit = agent::ResolveSessionRef(candidates, "不存在的引用", ambiguous);
    CHECK_FALSE(hit.has_value());
    CHECK_FALSE(ambiguous);

    // 空引用:NotFound。
    hit = agent::ResolveSessionRef(candidates, "", ambiguous);
    CHECK_FALSE(hit.has_value());
}

// ---------------------------------------------------------------------------
// archive / unarchive / delete(磁盘薄壳)
// ---------------------------------------------------------------------------

TEST_CASE("archive: 根 -> archive/ 字节原样;重复归档幂等") {
    TempSessionsDir dir("archive_basic");
    const std::string id = "20260820-101010-甲";
    const std::string content = WriteSession(dir, id, "D:/房", "2026-08-20 10:10:10");

    agent::SessionLifecycle lifecycle(dir.str());
    const auto result = lifecycle.ArchiveSession(id);
    REQUIRE(result.ok());
    CHECK(result.file_path == PathUtf8(dir.archive() / Utf8Name(id + ".jsonl")));

    // 文件真搬了,字节一字不差。
    std::ifstream in(dir.archive() / Utf8Name(id + ".jsonl"), std::ios::binary);
    std::string moved((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(moved == content);
    std::error_code ec;
    CHECK_FALSE(std::filesystem::exists(dir.base / Utf8Name(id + ".jsonl"), ec));

    // 重复归档:幂等成功,不搬第二次。
    const auto again = lifecycle.ArchiveSession(id);
    CHECK(again.ok());
    CHECK_FALSE(std::filesystem::exists(dir.base / Utf8Name(id + ".jsonl"), ec));
}

TEST_CASE("archive: 目标同名拒绝覆盖,原文件不动") {
    TempSessionsDir dir("archive_exists");
    const std::string id = "20260820-101010-甲";
    WriteSession(dir, id, "D:/房", "2026-08-20 10:10:10");
    // archive 里先塞一份同名。
    std::error_code ec;
    std::filesystem::create_directories(dir.archive(), ec);
    std::ofstream(dir.archive() / Utf8Name(id + ".jsonl"), std::ios::binary) << "别人先占的";

    agent::SessionLifecycle lifecycle(dir.str());
    const auto result = lifecycle.ArchiveSession(id);
    CHECK(result.code == agent::SessionLifecycleCode::TargetExists);
    // 原文件还在原地。
    CHECK(std::filesystem::exists(dir.base / Utf8Name(id + ".jsonl"), ec));
}

TEST_CASE("unarchive: 搬回根;根同名拒绝;归档场不掺默认列表") {
    TempSessionsDir dir("unarchive_basic");
    const std::string id = "20260820-101010-甲";
    const std::string content = WriteSession(dir, id, "D:/房", "2026-08-20 10:10:10");
    agent::SessionLifecycle lifecycle(dir.str());
    REQUIRE(lifecycle.ArchiveSession(id).ok());

    // 归档后:catalog 默认(active)查不到,Archived 查得到。
    agent::SessionCatalog catalog(dir.str());
    catalog.Scan();
    agent::SessionQuery active_query;
    active_query.scope = agent::SessionScope::All;
    active_query.state = agent::SessionState::Active;
    active_query.limit = 0;
    CHECK(catalog.Query(active_query).total == 0);
    agent::SessionQuery archived_query = active_query;
    archived_query.state = agent::SessionState::Archived;
    const auto archived_page = catalog.Query(archived_query);
    REQUIRE(archived_page.total == 1);
    CHECK(archived_page.entries[0].id == id);

    // unarchive:字节一字不丢。
    const auto result = lifecycle.UnarchiveSession(id);
    REQUIRE(result.ok());
    std::ifstream in(dir.base / Utf8Name(id + ".jsonl"), std::ios::binary);
    std::string back((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(back == content);
    CHECK_FALSE(std::filesystem::exists(dir.archive() / Utf8Name(id + ".jsonl")));

    // 归档场回去后:active 又见,Archived 不见。
    agent::SessionCatalog catalog2(dir.str());
    catalog2.Scan();
    CHECK(catalog2.Query(active_query).total == 1);
    CHECK(catalog2.Query(archived_query).total == 0);

    // 根里已有同名时 unarchive:PathOf 先认根(在场子已在根的语义下),
    // 幂等成功不搬——与 archive 的幂等口径对齐(不会覆盖根里那份)。
    const std::string id_b = "20260821-111111-乙";
    WriteSession(dir, id_b, "D:/房", "2026-08-21 11:11:11");
    REQUIRE(lifecycle.ArchiveSession(id_b).ok());
    std::ofstream(dir.base / Utf8Name(id_b + ".jsonl"), std::ios::binary) << "根里的同名";
    const auto unarchive_again = lifecycle.UnarchiveSession(id_b);
    CHECK(unarchive_again.ok());  // 已在根:无事可做,不覆盖根里那份
    std::ifstream root_in(dir.base / Utf8Name(id_b + ".jsonl"), std::ios::binary);
    std::string root_content((std::istreambuf_iterator<char>(root_in)), std::istreambuf_iterator<char>());
    CHECK(root_content == "根里的同名");
}

TEST_CASE("delete: 缺确认不动盘;确认后只删目标一场") {
    TempSessionsDir dir("delete_basic");
    const std::string id_a = "20260820-101010-甲";
    const std::string id_b = "20260821-111111-乙";
    WriteSession(dir, id_a, "D:/房", "2026-08-20 10:10:10");
    WriteSession(dir, id_b, "D:/房", "2026-08-21 11:11:11");

    agent::SessionLifecycle lifecycle(dir.str());
    // 缺确认:一律拒绝,盘上不动。
    CHECK(lifecycle.DeleteSession(id_a, false).code ==
          agent::SessionLifecycleCode::ConfirmationRequired);
    std::error_code ec;
    CHECK(std::filesystem::exists(dir.base / Utf8Name(id_a + ".jsonl"), ec));

    // 确认:只删甲,乙照在。
    const auto result = lifecycle.DeleteSession(id_a, true);
    REQUIRE(result.ok());
    CHECK_FALSE(std::filesystem::exists(dir.base / Utf8Name(id_a + ".jsonl"), ec));
    CHECK(std::filesystem::exists(dir.base / Utf8Name(id_b + ".jsonl"), ec));

    // 归档场也能删。
    REQUIRE(lifecycle.ArchiveSession(id_b).ok());
    REQUIRE(lifecycle.DeleteSession(id_b, true).ok());
    CHECK_FALSE(std::filesystem::exists(dir.archive() / Utf8Name(id_b + ".jsonl"), ec));

    // 删不存在的:NotFound。
    CHECK(lifecycle.DeleteSession("99999999-999999-无", true).code ==
          agent::SessionLifecycleCode::NotFound);
}

TEST_CASE("路径校验: 分隔符/点点冒充 id 一律拒绝;后缀不对拒绝") {
    TempSessionsDir dir("path_guard");
    WriteSession(dir, "20260820-101010-甲", "D:/房", "2026-08-20 10:10:10");
    agent::SessionLifecycle lifecycle(dir.str());

    // id 里带路径分隔符:PathOf 直接给空(NotFound),拼都不拼。
    CHECK(lifecycle.ArchiveSession("../config").code == agent::SessionLifecycleCode::NotFound);
    CHECK(lifecycle.ArchiveSession("a/b").code == agent::SessionLifecycleCode::NotFound);
    CHECK(lifecycle.ArchiveSession(".").code == agent::SessionLifecycleCode::NotFound);
    CHECK(lifecycle.DeleteSession("..\\config", true).code == agent::SessionLifecycleCode::NotFound);

    // 目录当成目标传不进来(接口只收 id);这里再钉一道:点开头的怪 id
    // 只要文件不存在就是 NotFound。
    CHECK(lifecycle.ArchiveSession(".hidden").code == agent::SessionLifecycleCode::NotFound);
}

TEST_CASE("Windows 开句柄回归: 活动句柄先关再搬,回调拒绝时不搬") {
    TempSessionsDir dir("active_handle");
    const std::string id = "20260820-101010-甲";

    // 活动会话:SessionStore 自己建档开 append 句柄(Windows 上的真实
    // 形态)。不预写文件——Begin 撞名会开 -2 后缀的新场,归档的就不再是
    // 同一场了。
    agent::SessionStore store(dir.str());
    agent::SessionMeta meta;
    meta.wire = "anthropic";
    meta.cwd = "D:/房";
    meta.started_at = "2026-08-20 10:10:10";
    REQUIRE(store.Begin(meta, id));
    CHECK(store.active());
    const std::string content =
        agent::SerializeSessionMeta(meta) + "\n";  // Begin 落的 meta 行

    agent::SessionLifecycle lifecycle(dir.str());
    bool flush_called = false;
    lifecycle.SetActiveFile(store.file_path(), [&](const std::string& path) {
        flush_called = true;
        store.Reset();  // 真关柄
        return !path.empty();
    });
    const auto result = lifecycle.ArchiveSession(id);
    REQUIRE(result.ok());
    CHECK(flush_called);
    CHECK_FALSE(store.active());
    std::ifstream in(dir.archive() / Utf8Name(id + ".jsonl"), std::ios::binary);
    std::string moved((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(moved == content);

    // 回调说收柄失败:拒绝,文件留在原地(半路失败原文件可用)。
    TempSessionsDir dir2("active_handle_fail");
    const std::string id2 = "20260820-101010-甲";
    WriteSession(dir2, id2, "D:/房", "2026-08-20 10:10:10");
    agent::SessionLifecycle lifecycle2(dir2.str());
    lifecycle2.SetActiveFile(PathUtf8(dir2.base / Utf8Name(id2 + ".jsonl")), [](const std::string&) { return false; });
    CHECK(lifecycle2.ArchiveSession(id2).code == agent::SessionLifecycleCode::IoError);
    std::error_code ec;
    CHECK(std::filesystem::exists(dir2.base / Utf8Name(id2 + ".jsonl"), ec));
    CHECK_FALSE(std::filesystem::exists(dir2.archive() / Utf8Name(id2 + ".jsonl"), ec));
}

// Windows 语义的 delete 柄收口(0.26.29 Windows CI 挂掉的回归钉):
// 目标是当前活动会话时,delete 先走 SetActiveFile 注入的收柄回调再
// remove——Windows 上 append 句柄开着就删必吃 sharing violation。
// 本地 WSL/Linux 验不了这条(remove 对开着的句柄也成),写成三平台都
// 跑的形状:断言"回调被调过、删成了、盘上真没了",柄收口语义由
// Windows 实机承担。
TEST_CASE("delete 柄收口: 目标是活动会话时先收柄再删(Windows sharing violation 闸)") {
    TempSessionsDir dir("delete_handle");
    const std::string id = "20260820-101010-甲";

    // 活动会话:SessionStore 建档开 append 句柄(Windows 上的真实形态)。
    agent::SessionStore store(dir.str());
    agent::SessionMeta meta;
    meta.wire = "anthropic";
    meta.cwd = "D:/房";
    meta.started_at = "2026-08-20 10:10:10";
    REQUIRE(store.Begin(meta, id));
    CHECK(store.active());

    agent::SessionLifecycle lifecycle(dir.str());
    bool flush_called = false;
    lifecycle.SetActiveFile(store.file_path(), [&](const std::string& path) {
        flush_called = true;
        store.Reset();  // 真关柄
        return !path.empty();
    });
    const auto result = lifecycle.DeleteSession(id, /*confirmed=*/true);
    REQUIRE(result.ok());
    CHECK(flush_called);
    CHECK_FALSE(store.active());
    std::error_code ec;
    CHECK_FALSE(std::filesystem::exists(dir.base / Utf8Name(id + ".jsonl"), ec));

    // 回调说收柄失败:拒绝删,文件留在原地。
    TempSessionsDir dir2("delete_handle_fail");
    const std::string id2 = "20260820-101010-甲";
    WriteSession(dir2, id2, "D:/房", "2026-08-20 10:10:10");
    agent::SessionLifecycle lifecycle2(dir2.str());
    lifecycle2.SetActiveFile(PathUtf8(dir2.base / Utf8Name(id2 + ".jsonl")),
                             [](const std::string&) { return false; });
    CHECK(lifecycle2.DeleteSession(id2, true).code == agent::SessionLifecycleCode::IoError);
    CHECK(std::filesystem::exists(dir2.base / Utf8Name(id2 + ".jsonl"), ec));
}

TEST_CASE("catalog 的 updated 账在归档里也认: queue 事件行与 clock 换算不折损") {
    // 0.26.18 的 queue 事件行与 0.26.24 的 file_clock 换算:归档里的场子
    // 照样按最后合法 ts 排,queue 快照也是账,不当坏行。
    TempSessionsDir dir("archived_queue");
    const std::string id = "20260820-101010-甲";
    std::string content = WriteSession(dir, id, "D:/房", "2026-08-20 10:10:10");
    // 补一条 queue 事件行(0.26.18 起 queue 快照也是账)。
    std::vector<agent::ArchivedQueueItem> items;
    agent::ArchivedQueueItem item;
    item.id = 1;
    item.text = "排队的话";
    items.push_back(item);
    content += agent::SerializeQueueEvent(items, "2026-08-21 12:00:00") + "\n";
    {
        std::ofstream f(dir.base / Utf8Name(id + ".jsonl"), std::ios::binary);
        f << content;
    }
    agent::SessionLifecycle lifecycle(dir.str());
    REQUIRE(lifecycle.ArchiveSession(id).ok());

    agent::SessionCatalog catalog(dir.str());
    catalog.Scan();
    agent::SessionQuery query;
    query.scope = agent::SessionScope::All;
    query.state = agent::SessionState::Archived;
    query.limit = 0;
    const auto page = catalog.Query(query);
    REQUIRE(page.total == 1);
    // queue 行的 ts 是最后的账:updated_at 取它,不当坏行折 mtime。
    CHECK(page.entries[0].updated_at == "2026-08-21 12:00:00");
    CHECK(page.entries[0].message_count == 1);  // queue 行不是消息
    CHECK(page.entries[0].health == agent::SessionHealth::Ok);
}

// ---------------------------------------------------------------------------
// 逐枚追踪单第 5 期:retention 联动(context 目录随 archive/delete 走)
// ---------------------------------------------------------------------------

TEST_CASE("lifecycle: context 目录(artifact 仓)随 archive 搬、随 delete 删") {
    const TempSessionsDir dir("lifecycle_retention");
    const std::string id = "retention-session";
    WriteSession(dir, id, "/tmp", "2026-08-23 10:00:00");

    // 造 context 目录:与 <id>.jsonl 并排的 <id>/context。
    std::error_code ec;
    const std::filesystem::path context = dir.base / id / "context";
    std::filesystem::create_directories(context, ec);
    {
        std::ofstream blob(context / "blob-0001", std::ios::binary);
        blob << "payload";
    }
    REQUIRE(std::filesystem::exists(context));

    // archive:jsonl 与 context 一起进 archive/。
    {
        agent::SessionLifecycle lifecycle(PathUtf8(dir.base));
        const auto result = lifecycle.ArchiveSession(id);
        REQUIRE(result.ok());
        const std::filesystem::path archived_context =
            dir.base / "archive" / id / "context";
        CHECK(std::filesystem::exists(archived_context));
        CHECK_FALSE(std::filesystem::exists(context));
        CHECK(result.detail.empty());
    }

    // unarchive:一起搬回根。
    {
        agent::SessionLifecycle lifecycle(PathUtf8(dir.base));
        const auto result = lifecycle.UnarchiveSession(id);
        REQUIRE(result.ok());
        CHECK(std::filesystem::exists(context));
    }

    // delete:一起删。
    {
        agent::SessionLifecycle lifecycle(PathUtf8(dir.base));
        const auto result = lifecycle.DeleteSession(id, true);
        REQUIRE(result.ok());
        CHECK_FALSE(std::filesystem::exists(dir.base / (id + ".jsonl")));
        CHECK_FALSE(std::filesystem::exists(context));
        CHECK(result.detail.empty());
    }

    // TempSessionsDir 析构自清

}

TEST_CASE("lifecycle: 没 context 目录的会话照常 archive/delete(空过不报)") {
    const TempSessionsDir dir("lifecycle_retention_bare");
    const std::string id = "bare-session";
    WriteSession(dir, id, "/tmp", "2026-08-23 10:00:00");

    {
        agent::SessionLifecycle lifecycle(PathUtf8(dir.base));
        const auto result = lifecycle.ArchiveSession(id);
        REQUIRE(result.ok());
        CHECK(result.detail.empty());
    }
    {
        agent::SessionLifecycle lifecycle(PathUtf8(dir.base));
        const auto result = lifecycle.UnarchiveSession(id);
        REQUIRE(result.ok());
    }
    {
        agent::SessionLifecycle lifecycle(PathUtf8(dir.base));
        const auto result = lifecycle.DeleteSession(id, true);
        REQUIRE(result.ok());
        CHECK(result.detail.empty());
    }

    std::error_code ec;
    // TempSessionsDir 析构自清

}
