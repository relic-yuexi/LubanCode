// Ctrl+R 提问历史反向搜索(交互抛光总账第二批)的纯逻辑测试:
//   - 范围轮换 Session→Project→All→Session;
//   - 索引构建:三范围过滤、连续同文去重(跨场不去)、最新在前、截上限;
//   - 查询:大小写不敏感子串、空查询=全部、limit 截条;
//   - 状态机:MoveOlder/MoveNewer 到头停、Rerun 归零选中位;
//   - 渲染行:首行带范围与键提示、无命中的空行、选中前缀。

#include <doctest/doctest.h>

#include <string>

#include "cli/history_search.hpp"

using namespace lubancode::cli;

namespace {

PromptHistoryEntry MakeEntry(std::string text, std::string session, std::string project_key,
                             std::string ts = "2026-08-16 10:00:00") {
    PromptHistoryEntry entry;
    entry.text = std::move(text);
    entry.session_id = std::move(session);
    entry.project_key = std::move(project_key);
    entry.ts = std::move(ts);
    return entry;
}

PromptHistoryDataset MakeDataset() {
    PromptHistoryDataset data;
    data.current_session_id = "s-new";
    data.current_project_key = "d/work/lubancode";
    // 时间序(旧→新):两场同项目 + 一场别项目。
    data.entries.push_back(MakeEntry("how to read a file", "s-old", "d/work/lubancode", "2026-08-14 09:00:00"));
    data.entries.push_back(MakeEntry("解释一下 CMakeLists", "s-old", "d/work/lubancode", "2026-08-14 09:30:00"));
    data.entries.push_back(MakeEntry("How To Read A File 再问一遍", "s-mid", "d/work/lubancode", "2026-08-15 08:00:00"));
    data.entries.push_back(MakeEntry("别的项目的提问", "s-other", "d/other/project", "2026-08-15 09:00:00"));
    data.entries.push_back(MakeEntry("how to read a file", "s-new", "d/work/lubancode", "2026-08-16 10:00:00"));
    data.entries.push_back(MakeEntry("how to read a file", "s-new", "d/work/lubancode", "2026-08-16 10:05:00"));
    return data;
}

}  // namespace

TEST_CASE("范围轮换:Session→Project→All→Session") {
    CHECK(NextHistorySearchScope(HistorySearchScope::Session) == HistorySearchScope::Project);
    CHECK(NextHistorySearchScope(HistorySearchScope::Project) == HistorySearchScope::All);
    CHECK(NextHistorySearchScope(HistorySearchScope::All) == HistorySearchScope::Session);
}

TEST_CASE("索引构建:三范围过滤 + 去重 + 最新在前") {
    const PromptHistoryDataset data = MakeDataset();

    // 本会话:两条同文连续提问只留最新一条(去重保新)。
    auto index = BuildHistorySearchIndex(data, HistorySearchScope::Session);
    REQUIRE(index.size() == 1);
    CHECK(index[0].ts == "2026-08-16 10:05:00");

    // 本项目:三场同项目;两处非连续同文(s-old 与 s-new 各一)都留。
    index = BuildHistorySearchIndex(data, HistorySearchScope::Project);
    REQUIRE(index.size() == 4);
    CHECK(index[0].ts == "2026-08-16 10:05:00");  // 新→旧
    CHECK(index[3].ts == "2026-08-14 09:00:00");

    // 全部:别项目那场也进来,仍新→旧。
    index = BuildHistorySearchIndex(data, HistorySearchScope::All);
    REQUIRE(index.size() == 5);
    CHECK(index[0].ts == "2026-08-16 10:05:00");
    CHECK(index[1].ts == "2026-08-15 09:00:00");

    // 上限:截最近 N 条。
    index = BuildHistorySearchIndex(data, HistorySearchScope::All, 2);
    REQUIRE(index.size() == 2);
    CHECK(index[0].ts == "2026-08-16 10:05:00");
}

TEST_CASE("查询:大小写不敏感子串,空查询=全部") {
    const auto index = BuildHistorySearchIndex(MakeDataset(), HistorySearchScope::All);
    auto hits = SearchHistoryEntries(index, "how to read");
    REQUIRE(hits.size() == 3);  // 三处同文(两场),去重只去了同场连续那条
    CHECK(index[hits[0]].ts == "2026-08-16 10:05:00");  // 最新优先

    hits = SearchHistoryEntries(index, "EXPLAIN 一");  // 无命中(中文原样比)
    CHECK(hits.empty());

    hits = SearchHistoryEntries(index, "");
    CHECK(hits.size() == 5);

    hits = SearchHistoryEntries(index, "", 2);
    CHECK(hits.size() == 2);  // limit 截条
}

TEST_CASE("状态机:选位到头停,Rerun 归零") {
    HistorySearchSession session;
    session.Open(MakeDataset(), HistorySearchScope::All);
    session.Rerun("");
    REQUIRE(session.matches().size() == 5);

    session.MoveOlder();
    session.MoveOlder();
    CHECK(session.selected() == 2);
    CHECK(session.SelectedEntry() != nullptr);
    CHECK(session.SelectedEntry()->ts == "2026-08-15 08:00:00");

    // 到头再按停住;回新到 0 停住。
    for (int i = 0; i < 10; ++i) {
        session.MoveOlder();
    }
    CHECK(session.selected() == 4);
    for (int i = 0; i < 10; ++i) {
        session.MoveNewer();
    }
    CHECK(session.selected() == 0);

    // 查询变化:选中位归零,命中集重建。
    session.Rerun("别的项目");
    REQUIRE(session.matches().size() == 1);
    CHECK(session.selected() == 0);

    // 范围轮换:索引换血,matches 清空(渲染层会拿当前查询重跑)。
    session.CycleScope();
    CHECK(session.scope() == HistorySearchScope::Session);
    CHECK(session.matches().empty());

    CHECK(session.active());
    session.Close();
    CHECK_FALSE(session.active());
}

TEST_CASE("渲染行:首行带范围,选中前缀,无命中的空行") {
    HistorySearchSession session;
    session.Open(MakeDataset(), HistorySearchScope::Project);
    session.Rerun("cmake");

    auto lines = BuildHistorySearchLines(session, "cmake", 100, "", "");
    REQUIRE(lines.size() == 2);  // 头行 + 一条命中
    CHECK(lines[0].find("历史搜索") != std::string::npos);
    CHECK(lines[0].find("本项目") != std::string::npos);
    CHECK(lines[1].rfind("❯ ", 0) == 0);  // 选中行前缀
    CHECK(lines[1].find("CMakeLists") != std::string::npos);
    CHECK(lines[1].find("2026-08-14 09:30") != std::string::npos);  // 时间掐到分

    // 无命中:一行"没有命中",不越界。
    session.Rerun("不存在的查询");
    lines = BuildHistorySearchLines(session, "不存在的查询", 100, "", "");
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].find("没有命中") != std::string::npos);

    // 高亮色串透传(plain 主题传空串,行内无 ANSI)。
    CHECK(lines[0].find("\x1b[") == std::string::npos);
}
