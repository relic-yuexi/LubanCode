// SessionCatalog(会话管理器单第一步)的测试:纯函数 + 临时目录薄壳。
//   - 时间排序键:标准串/id 前缀认得出,坏串折 0;
//   - 摘要:meta/title/cwd 事件/首句/消息计数/最后 ts(含 queue 事件行);
//   - 坏 meta 单项 damaged,不拖垮整表;
//   - 两排序(Updated/Created)、两 scope(Cwd/All)、搜索命中四路
//     (title/首句/id/cwd,ASCII 不分大小写)、分页、空列表;
//   - 指纹缓存:文件没动不重读,动了重读。
//
// MSVC 盲区:临时目录先关柄再删、remove_all 用 error_code 形态(文件写完
// 显式 close)。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "agent/session_catalog.hpp"
#include "agent/session_store.hpp"
#include "api/types.hpp"

using namespace lubancode;

namespace {

api::Message UserText(const std::string& text) {
    api::Message m;
    m.role = api::Role::User;
    m.content.push_back(api::TextBlock{text});
    return m;
}

std::string PathUtf8(const std::filesystem::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// 一间临时目录,析构时清掉。
struct TempSessionsDir {
    std::filesystem::path base;

    TempSessionsDir(const char* tag) {
        base = std::filesystem::temp_directory_path() / (std::string("lubancode_catalog_") + tag);
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

// 写一场会话(写完关柄)。lines 是 meta 行之后的账;给 nullopt 表示整个
// 文件就这些原始字节(测坏 meta 用)。
void WriteSession(const std::filesystem::path& dir, const std::string& id,
                  const agent::SessionMeta& meta, const std::vector<std::string>& lines) {
    std::ofstream f(dir / (id + ".jsonl"), std::ios::binary);
    f << agent::SerializeSessionMeta(meta) << "\n";
    for (const auto& line : lines) {
        f << line << "\n";
    }
}

void WriteRaw(const std::filesystem::path& dir, const std::string& id, const std::string& content) {
    std::ofstream f(dir / (id + ".jsonl"), std::ios::binary);
    f << content;
}

agent::SessionMeta Meta(const std::string& cwd, const std::string& started_at) {
    agent::SessionMeta meta;
    meta.wire = "anthropic";
    meta.model = "m1";
    meta.cwd = cwd;
    meta.started_at = started_at;
    return meta;
}

}  // namespace

// ---------------------------------------------------------------------------
// 排序键
// ---------------------------------------------------------------------------

TEST_CASE("SessionTimeSortKey: 标准串成键,坏串折 0") {
    CHECK(agent::SessionTimeSortKey("2026-08-20 09:30:15") == 20260820093015ULL);
    CHECK(agent::SessionTimeSortKey("2026-08-20 09:30:15") > agent::SessionTimeSortKey("2026-08-19 23:59:59"));
    CHECK(agent::SessionTimeSortKey("") == 0);
    CHECK(agent::SessionTimeSortKey("abc") == 0);
    CHECK(agent::SessionTimeSortKey("2026-08-20 09:30") == 0);  // 位不够(只有 12 位数字)
}

TEST_CASE("SessionIdTimeSortKey: id 前缀成键,认不出折 0") {
    CHECK(agent::SessionIdTimeSortKey("20260820-093015-试试") == 20260820093015ULL);
    CHECK(agent::SessionIdTimeSortKey("short") == 0);
    CHECK(agent::SessionIdTimeSortKey("2026082X-093015-a") == 0);
    CHECK(agent::SessionIdTimeSortKey("") == 0);
}

// ---------------------------------------------------------------------------
// 摘要
// ---------------------------------------------------------------------------

TEST_CASE("SummarizeSessionContent: meta/title/cwd 事件/首句/最后 ts") {
    const std::string content = agent::SerializeSessionMeta(Meta("D:/房", "2026-08-20 08:00:00")) + "\n" +
                                agent::SerializeSessionMessage(UserText("第一问\n第二行"), "2026-08-20 08:00:01") +
                                "\n" +
                                agent::SerializeSessionMessage(UserText("第二问"), "2026-08-20 08:00:02") + "\n" +
                                agent::SerializeTitleEvent("起了标题", "2026-08-20 08:00:03") + "\n" +
                                agent::SerializeCwdEvent("D:/新房", "2026-08-20 08:00:04") + "\n";
    const auto s = agent::SummarizeSessionContent("20260820-080000-x", "p", content);
    CHECK(s.health == agent::SessionHealth::Ok);
    CHECK(s.title == "起了标题");            // 最后一条 title 事件
    CHECK(s.cwd == "D:/新房");               // cwd 事件盖 meta.cwd
    CHECK(s.model == "m1");
    CHECK(s.created_at == "2026-08-20 08:00:00");  // meta.started_at
    CHECK(s.updated_at == "2026-08-20 08:00:04");  // 最后合法账(事件行也算)
    CHECK(s.updated_at_key == 20260820080004ULL);
    CHECK(s.message_count == 2);                   // title/cwd 事件不算消息
    CHECK(s.first_user_text == "第一问");          // 只取第一行
}

TEST_CASE("SummarizeSessionContent: meta 缺 started_at 退 id 时间") {
    const std::string content =
        agent::SerializeSessionMeta(Meta("D:/房", "")) + "\n" +
        agent::SerializeSessionMessage(UserText("问"), "2026-08-20 08:00:01") + "\n";
    const auto s = agent::SummarizeSessionContent("20260820-093015-x", "p", content);
    CHECK(s.created_at == "2026-08-20 09:30:15");  // 从 id 前缀还原
    CHECK(s.health == agent::SessionHealth::Ok);
}

TEST_CASE("SummarizeSessionContent: queue 排队快照的 ts 也是最近的账") {
    std::vector<agent::ArchivedQueueItem> items;
    agent::ArchivedQueueItem item;
    item.id = 1;
    item.text = "排队的话";
    items.push_back(item);
    const std::string content =
        agent::SerializeSessionMeta(Meta("D:/房", "2026-08-20 08:00:00")) + "\n" +
        agent::SerializeSessionMessage(UserText("第一问"), "2026-08-20 08:00:01") + "\n" +
        agent::SerializeQueueEvent(items, "2026-08-20 08:00:30") + "\n";
    const auto s = agent::SummarizeSessionContent("20260820-080000-x", "p", content);
    CHECK(s.message_count == 1);                    // queue 事件不是消息
    CHECK(s.updated_at == "2026-08-20 08:00:30");   // 但它是最近的账
}

TEST_CASE("SummarizeSessionContent: 坏 meta 单项 damaged,正文照捞") {
    const std::string content = std::string("不是 meta 的一行\n") +
                                agent::SerializeSessionMessage(UserText("首句还在"), "2026-08-20 08:00:01") + "\n";
    const auto s = agent::SummarizeSessionContent("20260820-080000-x", "p", content);
    CHECK(s.health == agent::SessionHealth::Damaged);
    CHECK(s.first_user_text == "首句还在");
    CHECK(s.message_count == 1);
    CHECK(s.created_at == "2026-08-20 08:00:00");  // meta 没有,退 id 时间
}

// ---------------------------------------------------------------------------
// 台账:排序/筛选/搜索/分页/坏档/空表
// ---------------------------------------------------------------------------

namespace {

// 一间标准测试房:三场不同目录的场子 + 一场坏 meta + 一场不同 created/updated。
struct CatalogRoom {
    TempSessionsDir dir{"room"};

    CatalogRoom() {
        // 甲一:created 早,updated 也早。
        WriteSession(dir.base, "20260818-090000-a",
                     Meta("D:\\场子甲", "2026-08-18 09:00:00"),
                     {agent::SerializeSessionMessage(UserText("甲一问"), "2026-08-18 09:00:05"),
                      agent::SerializeTitleEvent("甲一的标题", "2026-08-18 09:00:06")});
        // 甲二:created 晚,updated 晚(同目录)。
        WriteSession(dir.base, "20260820-100000-b",
                     Meta("d:/场子甲/", "2026-08-20 10:00:00"),
                     {agent::SerializeSessionMessage(UserText("乙段首句在这里"), "2026-08-20 10:00:05"),
                      agent::SerializeSessionMessage(UserText("再问一句"), "2026-08-20 10:30:00")});
        // 丙一:别的目录。
        WriteSession(dir.base, "20260819-110000-c",
                     Meta("E:/场子丙", "2026-08-19 11:00:00"),
                     {agent::SerializeSessionMessage(UserText("丙一问"), "2026-08-19 11:00:05")});
        // 坏 meta:damaged。
        WriteRaw(dir.base, "20260821-120000-d", "garbage line\n");
        // 非 .jsonl 不列(WriteRaw 本身不带后缀)。
        std::ofstream note(dir.base / "20260821-130000-e.txt", std::ios::binary);
        note << "x";
        note.close();
    }
};

}  // namespace

TEST_CASE("SessionCatalog: Updated 排序,新→旧") {
    CatalogRoom room;
    agent::SessionCatalog catalog(room.dir.str());
    catalog.Scan();
    REQUIRE(catalog.size() == 4);  // 三场好的 + 一场坏,非 .jsonl 不算

    agent::SessionQuery q;
    q.scope = agent::SessionScope::All;
    q.sort = agent::SessionSort::Updated;
    q.limit = 0;
    const auto page = catalog.Query(q);
    REQUIRE(page.total == 4);
    REQUIRE(page.entries.size() == 4);
    // updated:坏档没 ts 退 mtime(创建于今,最大);然后甲二/丙一/甲一。
    CHECK(page.entries[0].id == "20260821-120000-d");  // 坏档排前(mtime 最新)
    CHECK(page.entries[0].health == agent::SessionHealth::Damaged);
    CHECK(page.entries[1].id == "20260820-100000-b");
    CHECK(page.entries[2].id == "20260819-110000-c");
    CHECK(page.entries[3].id == "20260818-090000-a");
}

TEST_CASE("SessionCatalog: Created 排序按 created_at,坏档退 id 时间") {
    CatalogRoom room;
    agent::SessionCatalog catalog(room.dir.str());
    catalog.Scan();

    agent::SessionQuery q;
    q.scope = agent::SessionScope::All;
    q.sort = agent::SessionSort::Created;
    q.limit = 0;
    const auto page = catalog.Query(q);
    REQUIRE(page.entries.size() == 4);
    // created:坏档 id 是 20260821(最新);然后甲二(0820)/丙一(0819)/甲一(0818)。
    CHECK(page.entries[0].id == "20260821-120000-d");
    CHECK(page.entries[0].created_at == "2026-08-21 12:00:00");  // id 时间
    CHECK(page.entries[1].id == "20260820-100000-b");
    CHECK(page.entries[2].id == "20260819-110000-c");
    CHECK(page.entries[3].id == "20260818-090000-a");
}

TEST_CASE("SessionCatalog: Cwd 视图斜杠方向/大小写/尾斜杠归一后对上") {
    CatalogRoom room;
    agent::SessionCatalog catalog(room.dir.str());
    catalog.Scan();

    agent::SessionQuery q;
    q.scope = agent::SessionScope::Cwd;
    q.cwd = "D:/场子甲\\";  // 跟 meta 写法不同向,照样对上
    q.limit = 0;
    const auto page = catalog.Query(q);
    CHECK(page.total == 2);
    REQUIRE(page.entries.size() == 2);
    CHECK(page.entries[0].cwd == "d:/场子甲/");
    CHECK(page.entries[1].cwd == "D:\\场子甲");
}

TEST_CASE("SessionCatalog: 搜索四路命中,ASCII 不分大小写,中文按原字") {
    CatalogRoom room;
    agent::SessionCatalog catalog(room.dir.str());
    catalog.Scan();

    agent::SessionQuery q;
    q.scope = agent::SessionScope::All;
    q.limit = 0;

    // 命 title。
    q.search = "标题";
    auto page = catalog.Query(q);
    REQUIRE(page.total == 1);
    CHECK(page.entries[0].id == "20260818-090000-a");

    // 命首句(大小写不敏感按中文路径试不到,用 id 路)。
    q.search = "首句";
    page = catalog.Query(q);
    REQUIRE(page.total == 1);
    CHECK(page.entries[0].id == "20260820-100000-b");

    // 命 id(ASCII 大小写折掉)。
    q.search = "090000-A";
    page = catalog.Query(q);
    REQUIRE(page.total == 1);
    CHECK(page.entries[0].id == "20260818-090000-a");

    // 命 cwd(子串)。
    q.search = "场子丙";
    page = catalog.Query(q);
    REQUIRE(page.total == 1);
    CHECK(page.entries[0].id == "20260819-110000-c");

    // 无命中。
    q.search = "压根没有的话";
    page = catalog.Query(q);
    CHECK(page.total == 0);
    CHECK(page.entries.empty());

    // 坏档也在搜索池里(id 可搜)。
    q.search = "120000";
    page = catalog.Query(q);
    REQUIRE(page.total == 1);
    CHECK(page.entries[0].health == agent::SessionHealth::Damaged);
}

TEST_CASE("SessionCatalog: 分页 cursor/limit,total 不受分页影响") {
    CatalogRoom room;
    agent::SessionCatalog catalog(room.dir.str());
    catalog.Scan();

    agent::SessionQuery q;
    q.scope = agent::SessionScope::All;
    q.sort = agent::SessionSort::Created;
    q.cursor = 1;
    q.limit = 2;
    const auto page = catalog.Query(q);
    CHECK(page.total == 4);
    REQUIRE(page.entries.size() == 2);
    CHECK(page.entries[0].id == "20260820-100000-b");
    CHECK(page.entries[1].id == "20260819-110000-c");

    // cursor 越界:空页,total 照报。
    q.cursor = 10;
    const auto tail = catalog.Query(q);
    CHECK(tail.total == 4);
    CHECK(tail.entries.empty());

    // limit = 0:不截,一页全给。
    q.cursor = 0;
    q.limit = 0;
    const auto all = catalog.Query(q);
    CHECK(all.entries.size() == 4);
}

TEST_CASE("SessionCatalog: 坏 meta 单项 damaged,别的场照列照搜") {
    CatalogRoom room;
    agent::SessionCatalog catalog(room.dir.str());
    catalog.Scan();
    REQUIRE(catalog.size() == 4);

    const auto* damaged = catalog.Find("20260821-120000-d");
    REQUIRE(damaged != nullptr);
    CHECK(damaged->health == agent::SessionHealth::Damaged);
    CHECK(damaged->created_at == "2026-08-21 12:00:00");

    const auto* ok = catalog.Find("20260818-090000-a");
    REQUIRE(ok != nullptr);
    CHECK(ok->health == agent::SessionHealth::Ok);
    CHECK(ok->title == "甲一的标题");
}

TEST_CASE("SessionCatalog: 空目录/不存在的目录给空表") {
    TempSessionsDir empty{"empty"};
    agent::SessionCatalog catalog(empty.str());
    catalog.Scan();
    CHECK(catalog.size() == 0);

    agent::SessionQuery q;
    q.scope = agent::SessionScope::All;
    const auto page = catalog.Query(q);
    CHECK(page.total == 0);
    CHECK(page.entries.empty());

    agent::SessionCatalog nowhere("D:/压根不存在的目录/zzz");
    nowhere.Scan();
    CHECK(nowhere.size() == 0);
}

TEST_CASE("SessionCatalog: 缓存指纹——文件没动不重读,动了重读") {
    TempSessionsDir dir{"fingerprint"};
    WriteSession(dir.base, "20260820-090000-x", Meta("D:/房", "2026-08-20 09:00:00"),
                 {agent::SerializeSessionMessage(UserText("旧问"), "2026-08-20 09:00:01")});

    agent::SessionCatalog catalog(dir.str());
    catalog.Scan();
    agent::SessionQuery q;
    q.scope = agent::SessionScope::All;
    q.limit = 0;
    auto page = catalog.Query(q);
    REQUIRE(page.entries.size() == 1);
    const std::string first_fingerprint = page.entries[0].file_fingerprint;
    CHECK_FALSE(first_fingerprint.empty());
    CHECK(page.entries[0].message_count == 1);

    // 没动:再查,指纹同、内容同。
    page = catalog.Query(q);
    CHECK(page.entries[0].file_fingerprint == first_fingerprint);
    CHECK(page.entries[0].message_count == 1);

    // 动了(追加一条):updated_at 与消息数跟上。
    {
        std::ofstream f(dir.base / "20260820-090000-x.jsonl", std::ios::binary | std::ios::app);
        f << agent::SerializeSessionMessage(UserText("新问"), "2026-08-20 10:00:00") << "\n";
    }
    page = catalog.Query(q);
    REQUIRE(page.entries.size() == 1);
    CHECK(page.entries[0].message_count == 2);
    CHECK(page.entries[0].updated_at == "2026-08-20 10:00:00");
    CHECK(page.entries[0].file_fingerprint != first_fingerprint);
}
