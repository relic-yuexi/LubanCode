// TUI 排版批 5c(/evolve status/list/show/propose)的输出形状册。
//   - status:标题(尾冒号剥掉)进上边框,采集/落账/账面/账本/警告键值列
//     全表对齐;警告走 error 语义色;
//   - list:簇账与候选仓两张表,schema 列头,n 列右对齐,同类观察 ids 是
//     行尾列(旧"同类:"行的值段);
//   - show:观察页键值对 + 证据表;候选页键值对(空 key 续行收 tool/perm);
//   - propose:落账页一框收口,标题不带尾冒号;
//   - 80 列预算:width=80 时整行显示宽不超 80;
//   - plain 主题:零转义、无框,表头下垫 "-" 横线。
// 文案一字不改由句子级断言兼顾;本册主钉形状。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "app/commands/evolve_commands.hpp"
#include "cli/line_editor.hpp"  // DisplayWidthUtf8:对齐断言按显示列量
#include "cli/theme.hpp"

using namespace lubancode;
using namespace lubancode::app;

namespace {

bool Contains(const std::vector<std::string>& lines, const std::string& needle) {
    for (const auto& line : lines) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool ContainsOne(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// 剥掉 CSI 序列(与 test_insights_commands_frame.cpp 同一把手写的尺)。
std::string StripAnsiLight(const std::string& text) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            i += 2;
            while (i < text.size() &&
                   !((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z'))) {
                ++i;
            }
            if (i < text.size()) {
                ++i;  // 吃掉终结字母
            }
            continue;
        }
        out += text[i];
        ++i;
    }
    return out;
}

// key 段之后 value 的起始显示列(key 补齐 + 两格列距后应处处同列)。
int ValueStartCol(const std::string& row, const std::string& key) {
    const std::size_t at = row.find(key);
    if (at == std::string::npos) {
        return -1;
    }
    std::size_t i = at + key.size();
    while (i < row.size() && row[i] == ' ') {
        ++i;
    }
    return static_cast<int>(lubancode::cli::DisplayWidthUtf8(row.substr(0, i)));
}

// 某个字符在此行(剥转义后)的显示列;找不到给 -1。
int CharCol(const std::string& plain, char needle) {
    const std::size_t at = plain.find(needle);
    if (at == std::string::npos) {
        return -1;
    }
    return static_cast<int>(lubancode::cli::DisplayWidthUtf8(plain.substr(0, at)));
}

// 一段文本在此行的起始显示列(续行值起点用这把尺:值后面只剩衬空与边框,
// 不能用 ValueStartCol 那种"跳过尾巴空格"的量法)。
int StartCol(const std::string& plain, const std::string& needle) {
    const std::size_t at = plain.find(needle);
    if (at == std::string::npos) {
        return -1;
    }
    return static_cast<int>(lubancode::cli::DisplayWidthUtf8(plain.substr(0, at)));
}

std::string JoinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) {
        out += line;
        out += "\n";
    }
    return out;
}

constexpr const char* kBoxLightTopLeft = "\xe2\x94\x8c";  // ┌
constexpr const char* kBoxLightVert = "\xe2\x94\x82";     // │

EvolveStatusModel MakeStatusModel() {
    EvolveStatusModel model;
    model.recordings_scanned = 5;
    model.recordings_skipped = 1;
    model.runs_scanned = 3;
    model.memory_entries = 10;
    model.appended = 2;
    model.duplicates = 1;
    model.suppressed = 0;
    model.ledger_size = 14;
    model.cluster_count = 3;
    model.by_source = {{"recording", 11}, {"run", 3}};
    model.observations_file = "Z:/home/.lubancode/evolution/observations/observations.jsonl";
    model.error = "disk full";
    return model;
}

}  // namespace

TEST_CASE("status:标题(尾冒号剥掉)进上边框,键值列全表对齐(dark)") {
    const cli::Theme dark = cli::BuiltinTheme("dark");
    const std::vector<std::string> lines =
        FormatEvolveStatusLines(MakeStatusModel(), dark, /*width=*/0);
    const std::string joined = JoinLines(lines);
    REQUIRE(ContainsOne(joined, kBoxLightTopLeft));
    // 标题嵌上边框;旧标题的尾冒号剥掉(批 2 裁量 3):"Package:" 不连排。
    CHECK(ContainsOne(joined, "自进化观察账(阶段 1:只观察,不生成 Package)"));
    CHECK(joined.find("Package):") == std::string::npos);
    CHECK(ContainsOne(joined, dark.row_label));
    // 警告走 error 语义色(dark)。
    CHECK(ContainsOne(joined, dark.error));
    CHECK(ContainsOne(joined, "有观察没落住账(disk full)"));
    // key 列对齐:采集/落账/账面/账本/警告的 value 起始显示列一致。
    int collect_col = -1;
    int ledger_col = -1;
    int file_col = -1;
    int warn_col = -1;
    for (const std::string& line : lines) {
        const std::string plain = StripAnsiLight(line);
        if (collect_col < 0) collect_col = ValueStartCol(plain, "采集");
        if (ledger_col < 0) ledger_col = ValueStartCol(plain, "账面");
        if (file_col < 0) file_col = ValueStartCol(plain, "账本");
        if (warn_col < 0) warn_col = ValueStartCol(plain, "警告");
    }
    CHECK(collect_col > 0);
    CHECK(ledger_col > 0);
    CHECK(file_col > 0);
    CHECK(warn_col > 0);
    CHECK(collect_col == ledger_col);
    CHECK(collect_col == file_col);
    CHECK(collect_col == warn_col);
    // 句子级内容:采集/账面照旧。
    CHECK(ContainsOne(joined, "录制件 5(跳过半截 1),workflow run 3,memory 10"));
    CHECK(ContainsOne(joined, "观察 14 条,同类簇 3 个,recording 11,run 3"));
}

TEST_CASE("list 簇账:schema 列头,n 列右对齐,ids 列收同类观察(dark)") {
    const cli::Theme dark = cli::BuiltinTheme("dark");
    std::vector<EvolveClusterRow> rows;
    EvolveClusterRow big;
    big.fingerprint = "fp-alpha-shape";
    big.count = "x12";
    big.source = "recording";
    big.outcome = "success";
    big.summary = "同类经验最多的一簇";
    big.peers = "obs-1 obs-2 obs-3 +9";
    rows.push_back(big);
    EvolveClusterRow small;
    small.fingerprint = "fp-beta";
    small.count = "x2";
    small.source = "run";
    small.outcome = "partial";
    small.summary = "小簇";
    small.peers = "obs-9 obs-10";
    rows.push_back(small);
    const std::vector<std::string> lines =
        FormatEvolveClusterLines(/*ledger_size=*/14, rows, dark, /*width=*/0);
    const std::string joined = JoinLines(lines);
    REQUIRE(ContainsOne(joined, kBoxLightTopLeft));
    // 标题(尾冒号剥掉)与表头语义色。
    CHECK(ContainsOne(joined, "观察账(按同类指纹聚类,14 条 / 2 簇)"));
    CHECK(joined.find("簇):") == std::string::npos);
    CHECK(ContainsOne(joined, "fingerprint"));
    CHECK(ContainsOne(joined, "ids"));
    CHECK(ContainsOne(joined, dark.table_header));
    CHECK(ContainsOne(joined, dark.row_label));  // 首列加粗档
    // n 列右对齐:"x12" 与 "x2" 宽窄不一,尾字符落同一显示列。
    int big_col = -1;
    int small_col = -1;
    for (const std::string& line : lines) {
        const std::string plain = StripAnsiLight(line);
        if (plain.find("fp-alpha-shape") != std::string::npos) {
            big_col = CharCol(plain, '2');  // "x12" 的尾字
        }
        if (plain.find("fp-beta") != std::string::npos) {
            // 行内首个 '2' 是 "x2" 的尾字(summary 无 '2')。
            small_col = CharCol(plain, '2');
        }
    }
    CHECK(big_col > 0);
    CHECK(small_col > 0);
    CHECK(big_col == small_col);
    // 同类观察 ids 在行尾列。
    CHECK(ContainsOne(joined, "obs-1 obs-2 obs-3 +9"));
}

TEST_CASE("list 候选仓:表格 + schema 列头(dark)") {
    const cli::Theme dark = cli::BuiltinTheme("dark");
    std::vector<EvolveCandidateRow> rows;
    EvolveCandidateRow row;
    row.candidate_id = "cand-20260831-001";
    row.state = "validated";
    row.package_id = "evolve.demo-skill";
    row.objective = "把这套做法收成 Skill";
    rows.push_back(row);
    const std::vector<std::string> lines =
        FormatEvolveCandidateListLines(rows, dark, /*width=*/0);
    const std::string joined = JoinLines(lines);
    REQUIRE(ContainsOne(joined, kBoxLightTopLeft));
    CHECK(ContainsOne(joined, "候选仓(1 只,均在候选区,未进 /package)"));
    CHECK(ContainsOne(joined, "id"));
    CHECK(ContainsOne(joined, "state"));
    CHECK(ContainsOne(joined, "package"));
    CHECK(ContainsOne(joined, "objective"));
    CHECK(ContainsOne(joined, "evolve.demo-skill"));
}

TEST_CASE("show 观察页:键值对 + 证据表(dark)") {
    const cli::Theme dark = cli::BuiltinTheme("dark");
    EvolveObservationModel model;
    model.id = "obs-abcdef0123456789";
    model.source = "recording";
    model.outcome = "success";
    model.source_id = "20260830-120000-B00001";
    model.source_ref = "recordings/20260830-120000-B00001/events.jsonl";
    model.fingerprint = "fp-alpha-shape";
    model.summary = "一场走通的任务";
    model.created_at = "2026-08-30T12:30:00Z";
    model.evidence = {{"turn-12/tool:read", "读了配置文件"},
                      {"turn-15/tool:write", "落了产物"}};
    const std::vector<std::string> lines =
        FormatEvolveObservationLines(model, dark, /*width=*/0);
    const std::string joined = JoinLines(lines);
    REQUIRE(ContainsOne(joined, kBoxLightTopLeft));
    // 标题 = 观察id + [source outcome];键值列对齐。
    CHECK(ContainsOne(joined, "obs-abcdef0123456789  [recording success]"));
    CHECK(ContainsOne(joined, dark.row_label));
    int source_col = -1;
    int fingerprint_col = -1;
    for (const std::string& line : lines) {
        const std::string plain = StripAnsiLight(line);
        if (source_col < 0) source_col = ValueStartCol(plain, "来源");
        if (fingerprint_col < 0) fingerprint_col = ValueStartCol(plain, "指纹");
    }
    CHECK(source_col > 0);
    CHECK(source_col == fingerprint_col);
    // 证据是一张表(标题的尾冒号剥掉),ref/note 两列。
    CHECK(ContainsOne(joined, "证据(2 条)"));
    CHECK(joined.find("条):") == std::string::npos);
    CHECK(ContainsOne(joined, "turn-12/tool:read"));
    CHECK(ContainsOne(joined, "读了配置文件"));
}

TEST_CASE("show 候选页:tool/perm 续行同栏,缺账如实(dark)") {
    const cli::Theme dark = cli::BuiltinTheme("dark");
    EvolveCandidatePageModel model;
    model.candidate_id = "cand-20260831-001";
    model.state = "validated";
    model.package_id = "evolve.demo";
    model.dir_utf8 = "Z:/home/.lubancode/package-candidates/cand-20260831-001";
    model.content_hash = "abc123";
    model.has_shape = true;
    model.shape = "最小 Skill-only 包;复杂度 1 组件/0 进程";
    model.has_record = true;
    model.candidate_version = "1.0(无父版,与空对照)";
    model.objective = "把这套做法收成 Skill";
    model.sources = "(演化账未记来源)";
    model.generator = "ccmoon / gpt-5.6-sol / r3";
    model.changes = "新增组件 skill.md 权限差异 1 条,新工具 1 件";
    model.tools_added = {"demo_tool"};
    model.permissions_added = {"env:DEMO_KEY"};
    model.created_at = "(未记)";
    model.has_approval = false;
    model.has_eval = false;
    const std::vector<std::string> lines =
        FormatEvolveCandidatePageLines(model, dark, /*width=*/0);
    const std::string joined = JoinLines(lines);
    REQUIRE(ContainsOne(joined, kBoxLightTopLeft));
    CHECK(ContainsOne(joined, "cand-20260831-001  [validated]  evolve.demo"));
    // 缺账如实:批准账缺、评测账空,各是一句。
    CHECK(ContainsOne(joined, "(缺——候选不完整)"));
    CHECK(ContainsOne(joined, "空(先 /evolve test cand-20260831-001)"));
    // tool/perm 续行(空 key)与"目录"的 value 同栏。
    int dir_col = -1;
    int tool_col = -1;
    int perm_col = -1;
    for (const std::string& line : lines) {
        const std::string plain = StripAnsiLight(line);
        if (dir_col < 0) dir_col = ValueStartCol(plain, "目录");
        if (tool_col < 0) tool_col = StartCol(plain, "tool demo_tool");
        if (perm_col < 0) perm_col = StartCol(plain, "perm env:DEMO_KEY");
    }
    CHECK(dir_col > 0);
    CHECK(tool_col == dir_col);
    CHECK(perm_col == dir_col);
}

TEST_CASE("propose 落账页:一框收口,标题不带尾冒号(dark)") {
    const cli::Theme dark = cli::BuiltinTheme("dark");
    EvolveProposeModel model;
    model.candidate = "cand-20260831-002  [evolve.demo2 1.0]";
    model.content_hash = "def456";
    model.dir_utf8 = "Z:/home/.lubancode/package-candidates/cand-20260831-002";
    model.shape = "最小 Skill-only 包(默认答案;Agent 是升档不是标配)";
    model.components = "skills/demo/SKILL.md(content-only,无进程无网络)";
    model.cluster_skipped = 2;
    model.next_step = "/evolve diff cand-20260831-002(分档看形状)或 /evolve test "
                      "cand-20260831-002(评测五道门)";
    const std::vector<std::string> lines =
        FormatEvolveProposeLines(model, dark, /*width=*/0);
    const std::string joined = JoinLines(lines);
    REQUIRE(ContainsOne(joined, kBoxLightTopLeft));
    CHECK(ContainsOne(joined, "候选已落(只进候选仓,/package 看不见它)"));
    CHECK(joined.find("看不见它):") == std::string::npos);
    CHECK(ContainsOne(joined, dark.row_label));
    CHECK(ContainsOne(joined, "簇外另有 2 条同指纹观察找不到可读录制件,未进簇"));
    CHECK(ContainsOne(joined, "content-only,无进程无网络"));
}

TEST_CASE("80 列预算:status 与簇账整行显示宽不超 80(dark)") {
    const cli::Theme dark = cli::BuiltinTheme("dark");
    EvolveStatusModel model = MakeStatusModel();
    model.observations_file =
        "Z:/home/.lubancode/evolution/observations/observations.jsonl";
    const std::vector<std::string> status =
        FormatEvolveStatusLines(model, dark, /*width=*/80);
    REQUIRE(!status.empty());
    for (const std::string& line : status) {
        CHECK(lubancode::cli::DisplayWidthUtf8(StripAnsiLight(line)) <= 80);
    }
    std::vector<EvolveClusterRow> rows;
    EvolveClusterRow row;
    row.fingerprint = "fp-with-a-rather-long-name-for-width-pressure";
    row.count = "x3";
    row.source = "recording";
    row.outcome = "success";
    row.summary =
        "一条很长很长的摘要,用来逼表格的列帽,超预算时从最宽列起逐列削,削到保底为止";
    row.peers = "obs-1 obs-2 obs-3";
    rows.push_back(row);
    const std::vector<std::string> clusters =
        FormatEvolveClusterLines(/*ledger_size=*/3, rows, dark, /*width=*/80);
    REQUIRE(!clusters.empty());
    for (const std::string& line : clusters) {
        CHECK(lubancode::cli::DisplayWidthUtf8(StripAnsiLight(line)) <= 80);
    }
}

TEST_CASE("plain 主题:零转义、无框,表头下垫 - 横线,标题独立行") {
    const cli::Theme plain = cli::BuiltinTheme("plain");
    const std::vector<std::string> status =
        FormatEvolveStatusLines(MakeStatusModel(), plain, /*width=*/0);
    const std::string joined = JoinLines(status);
    CHECK(joined.find("\x1b") == std::string::npos);
    CHECK(!ContainsOne(joined, kBoxLightTopLeft));
    CHECK(!ContainsOne(joined, kBoxLightVert));
    CHECK(Contains(status, "自进化观察账(阶段 1:只观察,不生成 Package)"));
    CHECK(Contains(status, "观察 14 条,同类簇 3 个,recording 11,run 3"));

    std::vector<EvolveClusterRow> rows;
    EvolveClusterRow row;
    row.fingerprint = "fp-beta";
    row.count = "x2";
    row.source = "run";
    row.outcome = "partial";
    row.summary = "小簇";
    row.peers = "obs-9 obs-10";
    rows.push_back(row);
    const std::vector<std::string> clusters =
        FormatEvolveClusterLines(/*ledger_size=*/2, rows, plain, /*width=*/0);
    const std::string cluster_joined = JoinLines(clusters);
    CHECK(cluster_joined.find("\x1b") == std::string::npos);
    CHECK(!ContainsOne(cluster_joined, kBoxLightTopLeft));
    CHECK(Contains(clusters, "观察账(按同类指纹聚类,2 条 / 1 簇)"));
    // 表头行下垫一条 "-" 横线顶替颜色分隔(降级合同)。
    bool saw_header = false;
    bool has_header_rule = false;
    for (const std::string& line : clusters) {
        if (line.find("fingerprint") != std::string::npos) {
            saw_header = true;
            continue;
        }
        if (saw_header && !line.empty() && line.find_first_not_of('-') == std::string::npos) {
            has_header_rule = true;
        }
    }
    CHECK(saw_header);
    CHECK(has_header_rule);
    CHECK(Contains(clusters, "fp-beta"));
}
