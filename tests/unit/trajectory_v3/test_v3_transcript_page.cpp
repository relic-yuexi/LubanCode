// 轨迹 v3 P3 第二棒:转录摘要行 + seq 游标分页(Ctrl+T 浮层吃这层)。
//   - RenderRestoredTranscriptLines:hidden 默认不渲染;消息行带角色与
//     上下文状态(被压缩注"已压缩"、降档注"已降档");压缩标记处插
//     "前 ~N → 后 ~M tokens"摘要(数字读持久字段,§4.11);
//   - SliceRestoredTranscript:首开取尾页、before_seq 向旧、after_seq
//     向新,has_older/has_newer 与边界游标如实;
//   - TrajectorySessionLedger::ReadTranscriptPage:v3 场游标切页(投影
//     缓存,翻页不重读),v2 场/找不着的场给 nullopt(浮层走旧头尾截断)。
#include <doctest/doctest.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "runtime/trajectory_history_view.hpp"
#include "runtime/trajectory_session.hpp"
#include "workspace/identity.hpp"

using lubancode::runtime::FindV3HistoryStream;
using lubancode::runtime::RenderRestoredTranscriptLines;
using lubancode::runtime::RestoredTranscriptLine;
using lubancode::runtime::SliceRestoredTranscript;

namespace {

std::filesystem::path Fixture(const char* name) {
    return std::filesystem::path(LUBANCODE_SOURCE_DIR) / "tests" / "fixtures" /
           "trajectory_v3" / name;
}

std::vector<RestoredTranscriptLine> NumberedLines(int count) {
    std::vector<RestoredTranscriptLine> lines;
    for (int i = 1; i <= count; ++i) {
        lines.push_back(RestoredTranscriptLine{static_cast<std::uint64_t>(i),
                                               "line-" + std::to_string(i)});
    }
    return lines;
}

}  // namespace

// ---------------------------------------------------------------------------
// 渲染:角色、上下文状态注脚、压缩标记行、hidden 不渲染
// ---------------------------------------------------------------------------

TEST_CASE("转录渲染: compact 全链——被压缩行带注脚,压缩标记带持久 token 数") {
    const auto view = lubancode::runtime::ProjectRestoredHistory(Fixture("compact_full.jsonl"));
    const auto lines = RenderRestoredTranscriptLines(view);

    REQUIRE_FALSE(lines.empty());
    bool saw_compact_marker = false;
    bool saw_removed_user = false;
    std::uint64_t marker_seq = 0;
    for (const auto& line : lines) {
        // hidden(compact prompt/摘要,§4.28)默认不渲染:正文里就不该出现
        // 摘要正文与压缩请求正文。
        CHECK(line.text.find("压缩以下对话") == std::string::npos);
        CHECK(line.text.find("# 摘要") == std::string::npos);
        if (line.text.find("◆ 上下文已压缩") != std::string::npos) {
            saw_compact_marker = true;
            marker_seq = line.seq;
            // §4.11:数字读持久字段(142,800 → 31,600),不重算。
            CHECK(line.text.find("142,800") != std::string::npos);
            CHECK(line.text.find("31,600") != std::string::npos);
        }
        // msg-000002(第一轮 user,被 compact-000001 移出上下文):行在、
        // 正文在、注"已压缩"(§1.3"哪段已压缩,界面要分得清")。
        if (line.text.find("第一轮") != std::string::npos) {
            saw_removed_user = true;
            CHECK(line.text.find("user ·") != std::string::npos);
            CHECK(line.text.find("已压缩") != std::string::npos);
        }
        if (line.text.find("压缩后接着问") != std::string::npos) {
            // msg-000009:压缩后的新输入,还在当前上下文——不带"已压缩"。
            CHECK(line.text.find("已压缩") == std::string::npos);
        }
    }
    CHECK(saw_compact_marker);
    CHECK(saw_removed_user);
    // 标记插在 applied 的发生位置(seq=22):不许挪窝伪造顺序。
    CHECK(marker_seq == 22);
    // 行序 = 时间线原序(seq 升序)。
    for (std::size_t i = 1; i < lines.size(); ++i) {
        CHECK(lines[i - 1].seq < lines[i].seq);
    }
}

TEST_CASE("转录渲染: 工具配对——assistant 正文行与 tool 结果行各就位") {
    const auto view = lubancode::runtime::ProjectRestoredHistory(Fixture("tool_round.jsonl"));
    const auto lines = RenderRestoredTranscriptLines(view);
    bool saw_assistant = false;
    bool saw_tool_result = false;
    for (const auto& line : lines) {
        // assistant(msg-000003)正文与调用块同在:摘要行摆正文首行。
        if (line.text.find("assistant · 我读一下入口文件") != std::string::npos) {
            saw_assistant = true;
        }
        // tool 结果(user 携带 ToolResultBlock):角色标 tool,首行是
        // exit_code 行。
        if (line.text.find("tool ·") != std::string::npos) {
            saw_tool_result = true;
            CHECK(line.text.find("exit_code: 0") != std::string::npos);
        }
    }
    CHECK(saw_assistant);
    CHECK(saw_tool_result);

    // 只有调用块没有正文的 assistant:行摆 "[工具] 名字"(合成 view,钉
    // 分支)。
    lubancode::runtime::RestoredHistoryView synthetic;
    lubancode::runtime::RestoredHistoryItem item;
    item.kind = lubancode::runtime::RestoredHistoryItem::Kind::Message;
    item.seq = 7;
    item.message.message.role = lubancode::api::Role::Assistant;
    lubancode::api::ToolUseBlock use;
    use.id = "action-000009";
    use.name = "run_command";
    item.message.message.content.push_back(std::move(use));
    synthetic.items.push_back(std::move(item));
    const auto rendered = RenderRestoredTranscriptLines(synthetic);
    REQUIRE(rendered.size() == 1);
    CHECK(rendered[0].text == "  assistant · [工具] run_command");
}

// ---------------------------------------------------------------------------
// 切页:seq 游标
// ---------------------------------------------------------------------------

TEST_CASE("切页: 首开取尾页,游标与两向「还有货」如实") {
    // 10 行,一页 3 行:尾页 = 8/9/10,上方还有 1-7,下方没了。
    const auto page = SliceRestoredTranscript(NumberedLines(10), std::nullopt, std::nullopt, 3);
    REQUIRE(page.lines.size() == 3);
    CHECK(page.lines[0] == "line-8");
    CHECK(page.lines[2] == "line-10");
    CHECK(page.has_older);
    CHECK_FALSE(page.has_newer);
    CHECK(page.oldest_seq == std::optional<std::uint64_t>{8});
    CHECK(page.newest_seq == std::optional<std::uint64_t>{10});
}

TEST_CASE("切页: before_seq 向旧翻,before_seq=1 之后翻到头") {
    const auto lines = NumberedLines(10);
    // 向旧:接着 8 之前取 → 5/6/7,两向都还有。
    const auto older = SliceRestoredTranscript(lines, std::optional<std::uint64_t>{8},
                                               std::nullopt, 3);
    REQUIRE(older.lines.size() == 3);
    CHECK(older.lines[0] == "line-5");
    CHECK(older.has_older);
    CHECK(older.has_newer);
    // 再向旧到头:接着 2 之前取 → 只剩 line-1,上方没了。
    const auto head = SliceRestoredTranscript(lines, std::optional<std::uint64_t>{2},
                                              std::nullopt, 3);
    REQUIRE(head.lines.size() == 1);
    CHECK(head.lines[0] == "line-1");
    CHECK_FALSE(head.has_older);
    CHECK(head.has_newer);
    // 越过头(before_seq=1):空页,上方如实没了,下方还有整条时间线。
    const auto beyond = SliceRestoredTranscript(lines, std::optional<std::uint64_t>{1},
                                                std::nullopt, 3);
    CHECK(beyond.lines.empty());
    CHECK_FALSE(beyond.has_older);
    CHECK(beyond.has_newer);
}

TEST_CASE("切页: after_seq 向新翻,越尾空页") {
    const auto lines = NumberedLines(10);
    const auto newer = SliceRestoredTranscript(lines, std::nullopt,
                                               std::optional<std::uint64_t>{7}, 3);
    REQUIRE(newer.lines.size() == 3);
    CHECK(newer.lines[0] == "line-8");
    CHECK(newer.has_older);
    CHECK_FALSE(newer.has_newer);
    // 越尾(after_seq=10):空页,两向如实。
    const auto beyond = SliceRestoredTranscript(lines, std::nullopt,
                                                std::optional<std::uint64_t>{10}, 3);
    CHECK(beyond.lines.empty());
    CHECK(beyond.has_older);
    CHECK_FALSE(beyond.has_newer);
    // max_lines=0 = 不限:一页给全。
    const auto all = SliceRestoredTranscript(lines, std::nullopt, std::nullopt, 0);
    CHECK(all.lines.size() == 10);
    CHECK_FALSE(all.has_older);
    CHECK_FALSE(all.has_newer);
}

// ---------------------------------------------------------------------------
// 账本读面:ReadTranscriptPage(v3 分页 / v2 回落)
// ---------------------------------------------------------------------------

namespace {

struct LedgerFixture {
    std::filesystem::path dir;
    std::optional<lubancode::runtime::TrajectorySessionLedger> ledger;
    std::string v2_id;
    std::string v3_id;
    std::filesystem::path sessions_dir;

    explicit LedgerFixture(const char* tag)
        : dir(std::filesystem::temp_directory_path() /
              ("lubancode-v3-transcript-" + std::string(tag))) {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir / "repo", ec);
        lubancode::runtime::TrajectorySessionLedger::Options options;
        options.workspaces_root = dir / "workspaces";
        options.workspace_identity = lubancode::workspace::MakeFallbackIdentity(dir / "repo");
        options.lubancode_version = "test";
        auto opened = lubancode::runtime::TrajectorySessionLedger::Open(options);
        REQUIRE(opened.has_value());
        ledger.emplace(std::move(*opened));
        v2_id = ledger->session_id();
        sessions_dir = ledger->session_dir().parent_path();
        // v3 场种进同一 workspace 的 sessions/ 根(布局合同 §1.2)。
        const auto fixture = Fixture("compact_full.jsonl");
        std::error_code copy_ec;
        std::filesystem::create_directories(sessions_dir / "20260910-083000-V3FIX4", copy_ec);
        std::filesystem::copy_file(fixture,
                                   sessions_dir / "20260910-083000-V3FIX4" /
                                       "20260910-083000-V3FIX4.jsonl",
                                   std::filesystem::copy_options::overwrite_existing, copy_ec);
        REQUIRE_FALSE(copy_ec);
        v3_id = "20260910-083000-V3FIX4";
    }

    ~LedgerFixture() {
        ledger.reset();  // 先收账本(Windows 下开着的文件删不动)
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

}  // namespace

TEST_CASE("账本分页: v3 场游标翻页,一路翻到头") {
    LedgerFixture fx("ledger");
    // 首开:尾页(这里 fixture 可见行不足一页,一页给全)。
    const auto first = fx.ledger->ReadTranscriptPage(fx.v3_id, std::nullopt, std::nullopt, 40);
    REQUIRE(first.has_value());
    REQUIRE(first->lines.size() >= 5);
    CHECK(first->newest_seq.has_value());
    CHECK(first->lines.back().find("压缩后接着问") != std::string::npos);
    CHECK(first->lines.front().find("user ·") != std::string::npos);

    // 小页翻旧:一页 2 行,游标一路翻到头,行数对得上、不重不漏。
    std::vector<std::string> walked;
    std::optional<std::uint64_t> cursor;
    for (;;) {
        const auto page = fx.ledger->ReadTranscriptPage(fx.v3_id, cursor, std::nullopt, 2);
        REQUIRE(page.has_value());
        if (page->lines.empty()) {
            CHECK_FALSE(page->has_older);
            break;
        }
        walked.insert(walked.begin(), page->lines.begin(), page->lines.end());
        REQUIRE(page->oldest_seq.has_value());
        cursor = page->oldest_seq;
        if (!page->has_older) {
            break;
        }
    }
    CHECK(walked.size() == first->lines.size());
    CHECK(walked.front() == first->lines.front());
    CHECK(walked.back() == first->lines.back());

    // 向新翻:接着最旧行之后取,回到同一时间线。
    const auto newer = fx.ledger->ReadTranscriptPage(fx.v3_id, std::nullopt,
                                                     std::optional<std::uint64_t>{3}, 2);
    REQUIRE(newer.has_value());
    REQUIRE(newer->lines.size() == 2);
    CHECK(newer->oldest_seq == std::optional<std::uint64_t>{5});
}

TEST_CASE("账本分页: v2 场与找不着的场给 nullopt(浮层走旧路)") {
    LedgerFixture fx("fallback");
    // 活场是 v2(main.jsonl 在):nullopt,调用方照旧头尾截断。
    CHECK_FALSE(fx.ledger->ReadTranscriptPage(fx.v2_id, std::nullopt, std::nullopt, 40)
                     .has_value());
    // 没这个场:nullopt(不是空页——v2 老路自己给空表)。
    CHECK_FALSE(fx.ledger->ReadTranscriptPage("20260101-000000-NOPE", std::nullopt,
                                              std::nullopt, 40)
                     .has_value());
    // FindV3HistoryStream 的转发:v2 目录不给 v3 流。
    CHECK_FALSE(FindV3HistoryStream(fx.ledger->session_dir()).has_value());
    REQUIRE(FindV3HistoryStream(fx.sessions_dir / "20260910-083000-V3FIX4").has_value());
}
