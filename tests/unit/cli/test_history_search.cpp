// Ctrl+R 提问历史反向搜索(交互抛光总账第二批)的纯逻辑测试:
//   - 范围轮换 Session→Project→All→Session;
//   - 索引构建:三范围过滤、事件身份去重(同一事件合并,同文不同事件
//     各自保留)、最新在前、截上限;
//   - 查询:大小写不敏感子串、空查询=全部、limit 截条;
//   - 状态机:MoveOlder/MoveNewer 到头停、Rerun 归零选中位;
//   - 渲染行:首行带范围与键提示、无命中的空行、选中前缀;
//   - 磁盘与活历史合流(问题 8):仅磁盘、仅内存、完全重叠、尾部部分
//     重叠、同文不同事件五景不重不漏。

#include <doctest/doctest.h>

#include <string>

#include "cli/history_search.hpp"

using namespace lubancode::cli;

namespace {

PromptHistoryEntry MakeEntry(std::string text, std::string session, std::string project_key,
                             std::string ts = "2026-08-16 10:00:00", std::size_t seq = 0) {
    PromptHistoryEntry entry;
    entry.text = std::move(text);
    entry.session_id = std::move(session);
    entry.project_key = std::move(project_key);
    entry.ts = std::move(ts);
    entry.event_seq = seq;
    return entry;
}

PromptHistoryDataset MakeDataset() {
    PromptHistoryDataset data;
    data.current_session_id = "s-new";
    data.current_project_key = "d/work/lubancode";
    // 时间序(旧→新):两场同项目 + 一场别项目;s-new 里同一句真发了
    // 两次(事件序号 0 与 1),各自的 ts 都要留。
    data.entries.push_back(MakeEntry("how to read a file", "s-old", "d/work/lubancode", "2026-08-14 09:00:00", 0));
    data.entries.push_back(MakeEntry("解释一下 CMakeLists", "s-old", "d/work/lubancode", "2026-08-14 09:30:00", 1));
    data.entries.push_back(MakeEntry("How To Read A File 再问一遍", "s-mid", "d/work/lubancode", "2026-08-15 08:00:00", 0));
    data.entries.push_back(MakeEntry("别的项目的提问", "s-other", "d/other/project", "2026-08-15 09:00:00", 0));
    data.entries.push_back(MakeEntry("how to read a file", "s-new", "d/work/lubancode", "2026-08-16 10:00:00", 0));
    data.entries.push_back(MakeEntry("how to read a file", "s-new", "d/work/lubancode", "2026-08-16 10:05:00", 1));
    return data;
}

}  // namespace

TEST_CASE("范围轮换:Session→Project→All→Session") {
    CHECK(NextHistorySearchScope(HistorySearchScope::Session) == HistorySearchScope::Project);
    CHECK(NextHistorySearchScope(HistorySearchScope::Project) == HistorySearchScope::All);
    CHECK(NextHistorySearchScope(HistorySearchScope::All) == HistorySearchScope::Session);
}

TEST_CASE("索引构建:三范围过滤 + 身份去重 + 最新在前") {
    const PromptHistoryDataset data = MakeDataset();

    // 本会话:同一句真发了两次(事件 0 与 1),两条都留,最新在前。
    auto index = BuildHistorySearchIndex(data, HistorySearchScope::Session);
    REQUIRE(index.size() == 2);
    CHECK(index[0].ts == "2026-08-16 10:05:00");
    CHECK(index[1].ts == "2026-08-16 10:00:00");

    // 本项目:三场同项目;s-old 与 s-new 各有一句同文,跨场不去,全留。
    index = BuildHistorySearchIndex(data, HistorySearchScope::Project);
    REQUIRE(index.size() == 5);
    CHECK(index[0].ts == "2026-08-16 10:05:00");  // 新→旧
    CHECK(index[4].ts == "2026-08-14 09:00:00");

    // 全部:别项目那场也进来,仍新→旧。
    index = BuildHistorySearchIndex(data, HistorySearchScope::All);
    REQUIRE(index.size() == 6);
    CHECK(index[0].ts == "2026-08-16 10:05:00");
    CHECK(index[1].ts == "2026-08-16 10:00:00");  // 同场两次真发,新→旧挨着
    CHECK(index[2].ts == "2026-08-15 09:00:00");

    // 上限:截最近 N 条。
    index = BuildHistorySearchIndex(data, HistorySearchScope::All, 2);
    REQUIRE(index.size() == 2);
    CHECK(index[0].ts == "2026-08-16 10:05:00");
}

TEST_CASE("去重口径:同一事件合并,同文不同事件各自保留") {
    PromptHistoryDataset data;
    data.current_session_id = "s-cur";
    data.current_project_key = "d/work";
    // 同一事件从存档侧与活历史侧各来一份(落盘账与实际文件短暂脱节的
    // 兜底):身份相同(session+序号),合并成一条,档上真时间的这份为准。
    data.entries.push_back(MakeEntry("第一条", "s-cur", "d/work", "2026-08-29 10:00:00", 0));  // 存档份
    data.entries.push_back(MakeEntry("第一条", "s-cur", "d/work", "2026-08-29 22:00:00", 0));  // 尾巴份
    // 同文不同事件:用户真发了两次,序号不同,各自保留、各带时间。
    data.entries.push_back(MakeEntry("再来一遍", "s-cur", "d/work", "2026-08-29 10:01:00", 1));
    data.entries.push_back(MakeEntry("再来一遍", "s-cur", "d/work", "2026-08-29 10:02:00", 2));
    // 跨场同文:不去,时间戳与项目就是区分。
    data.entries.push_back(MakeEntry("再来一遍", "s-old", "d/work", "2026-08-01 09:00:00", 7));

    const auto index = BuildHistorySearchIndex(data, HistorySearchScope::Session);
    REQUIRE(index.size() == 3);  // 新→旧:再来一遍(10:02) / 再来一遍(10:01) / 第一条(档上 ts)
    CHECK(index[0].ts == "2026-08-29 10:02:00");
    CHECK(index[1].ts == "2026-08-29 10:01:00");
    CHECK(index[2].ts == "2026-08-29 10:00:00");  // 合并保先见(存档份,真时间)

    // 全部范围里跨场那条也进来,共四条;新→旧按条目位置翻(跨场旧条
    // 追加在数据集末尾,排最前不奇怪——真数据里场次按时间序拼,不会倒挂)。
    const auto all_index = BuildHistorySearchIndex(data, HistorySearchScope::All);
    REQUIRE(all_index.size() == 4);
    CHECK(all_index[0].session_id == "s-old");
}

TEST_CASE("磁盘与活历史合流:五景不重不漏") {
    // 场景口径:CollectPromptHistory 把存档侧条目(旧→新,场内事件序号)
    // 与活历史尾巴(序号接全场)拼成一份 entries,这里按同一拼法喂索引。
    const std::string cur = "s-cur";
    const std::string project = "d/work/lubancode";

    SUBCASE("仅磁盘:全落盘,尾巴空") {
        PromptHistoryDataset data;
        data.current_session_id = cur;
        data.current_project_key = project;
        data.entries.push_back(MakeEntry("问一", cur, project, "2026-08-29 10:00:00", 0));
        data.entries.push_back(MakeEntry("问二", cur, project, "2026-08-29 10:01:00", 1));
        data.entries.push_back(MakeEntry("问三", cur, project, "2026-08-29 10:02:00", 2));
        const auto index = BuildHistorySearchIndex(data, HistorySearchScope::Session);
        REQUIRE(index.size() == 3);  // Ctrl+R 只显三条
        CHECK(index[0].text == "问三");
    }

    SUBCASE("仅内存:一条都没落盘,尾巴全出") {
        PromptHistoryDataset data;
        data.current_session_id = cur;
        data.current_project_key = project;
        data.entries.push_back(MakeEntry("问一", cur, project, "2026-08-29 10:00:00", 0));
        data.entries.push_back(MakeEntry("问二", cur, project, "2026-08-29 10:00:01", 1));
        const auto index = BuildHistorySearchIndex(data, HistorySearchScope::Session);
        REQUIRE(index.size() == 2);
    }

    SUBCASE("完全重叠:同一事件两边都见着,合并成一份") {
        PromptHistoryDataset data;
        data.current_session_id = cur;
        data.current_project_key = project;
        data.entries.push_back(MakeEntry("问一", cur, project, "2026-08-29 10:00:00", 0));
        data.entries.push_back(MakeEntry("问二", cur, project, "2026-08-29 10:01:00", 1));
        // 活历史整场被旧路重抄了一遍(同身份再来一份):按事件身份合掉。
        data.entries.push_back(MakeEntry("问一", cur, project, "2026-08-29 22:00:00", 0));
        data.entries.push_back(MakeEntry("问二", cur, project, "2026-08-29 22:00:01", 1));
        const auto index = BuildHistorySearchIndex(data, HistorySearchScope::Session);
        REQUIRE(index.size() == 2);
        CHECK(index[1].ts == "2026-08-29 10:00:00");  // 保档上真时间那份
        CHECK(index[0].ts == "2026-08-29 10:01:00");
    }

    SUBCASE("尾部部分重叠:磁盘出前两条,尾巴只补第三条") {
        PromptHistoryDataset data;
        data.current_session_id = cur;
        data.current_project_key = project;
        data.entries.push_back(MakeEntry("问一", cur, project, "2026-08-29 10:00:00", 0));
        data.entries.push_back(MakeEntry("问二", cur, project, "2026-08-29 10:01:00", 1));
        data.entries.push_back(MakeEntry("问三", cur, project, "2026-08-29 10:02:30", 2));  // 未落盘
        const auto index = BuildHistorySearchIndex(data, HistorySearchScope::Session);
        REQUIRE(index.size() == 3);  // 三条各一次
        CHECK(index[0].text == "问三");
    }

    SUBCASE("同文不同事件:真发两次,两条都留") {
        PromptHistoryDataset data;
        data.current_session_id = cur;
        data.current_project_key = project;
        data.entries.push_back(MakeEntry("同一句话", cur, project, "2026-08-29 10:00:00", 0));
        data.entries.push_back(MakeEntry("同一句话", cur, project, "2026-08-29 10:05:00", 1));
        const auto index = BuildHistorySearchIndex(data, HistorySearchScope::Session);
        REQUIRE(index.size() == 2);
        CHECK(index[0].ts == "2026-08-29 10:05:00");
        CHECK(index[1].ts == "2026-08-29 10:00:00");
    }
}

TEST_CASE("查询:大小写不敏感子串,空查询=全部") {
    const auto index = BuildHistorySearchIndex(MakeDataset(), HistorySearchScope::All);
    auto hits = SearchHistoryEntries(index, "how to read");
    REQUIRE(hits.size() == 4);  // 四处同文(三场),同场两条是两次真发,都留
    CHECK(index[hits[0]].ts == "2026-08-16 10:05:00");  // 最新优先

    hits = SearchHistoryEntries(index, "EXPLAIN 一");  // 无命中(中文原样比)
    CHECK(hits.empty());

    hits = SearchHistoryEntries(index, "");
    CHECK(hits.size() == 6);

    hits = SearchHistoryEntries(index, "", 2);
    CHECK(hits.size() == 2);  // limit 截条
}

TEST_CASE("状态机:选位到头停,Rerun 归零") {
    HistorySearchSession session;
    session.Open(MakeDataset(), HistorySearchScope::All);
    // Open 即就绪:空查询=全部命中(终端层"查询未变不重跑"的短路靠这
    // 一条才不误报"没有命中")。
    REQUIRE(session.matches().size() == 6);
    session.Rerun("");
    REQUIRE(session.matches().size() == 6);

    session.MoveOlder();
    session.MoveOlder();
    CHECK(session.selected() == 2);
    CHECK(session.SelectedEntry() != nullptr);
    CHECK(session.SelectedEntry()->ts == "2026-08-15 09:00:00");

    // 到头再按停住;回新到 0 停住。
    for (int i = 0; i < 10; ++i) {
        session.MoveOlder();
    }
    CHECK(session.selected() == 5);
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
