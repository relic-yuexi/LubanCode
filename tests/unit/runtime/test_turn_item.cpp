// TurnItem/DiffTable 单测(显示系统剥离单第五步:拆领域条目)。
//
// 钉的是:
//   1. BuildDiffTable 的行级 diff 与 cli::ComputeLineDiff/BuildEditDiff/
//      BuildWriteDiff 同一算法——同一份输入两边行数、行类、行号、正文
//      逐行一致(中立行表不是第二颗算法);
//   2. DiffTable 的事实摘要(located/replaced_count/old_exists/added/
//      removed)与 cli 侧 BuildFileDiffPreview 的 header 事实对得上;
//   3. 别的工具给 nullopt;读不出的路径按新文件处理,不崩;
//   4. TruncateUtf8Bytes 与 cli::TruncateUtf8Bytes 同一套解码(不劈多字节);
//   5. 枚举 <-> 稳定字符串单射。

#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "cli/diff.hpp"
#include "cli/transcript.hpp"
#include "runtime/turn_item.hpp"

namespace rt = lubancode::runtime;
namespace cli = lubancode::cli;

namespace {

// 临时文件 RAII:先关柄再删、remove_all 用 error_code 形态(MSVC 盲区单)。
class TempFile {
public:
    explicit TempFile(const std::string& content) {
        path_ = (std::filesystem::temp_directory_path() /
                 ("lubancode-turn-item-" + std::to_string(counter_++) + ".txt"))
                    .string();
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        out << content;
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    const std::string& path() const { return path_; }

private:
    static inline int counter_ = 0;
    std::string path_;
};

rt::DiffRowKind ToRtKind(cli::DiffLineKind kind) {
    switch (kind) {
        case cli::DiffLineKind::Context: return rt::DiffRowKind::Context;
        case cli::DiffLineKind::Del: return rt::DiffRowKind::Del;
        case cli::DiffLineKind::Add: return rt::DiffRowKind::Add;
    }
    return rt::DiffRowKind::Context;
}

void CheckRowsMatch(const std::vector<rt::DiffRow>& rows, const std::vector<cli::DiffLine>& lines) {
    REQUIRE(rows.size() == lines.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        CHECK(rows[i].kind == ToRtKind(lines[i].kind));
        CHECK(rows[i].text == lines[i].text);
        CHECK(rows[i].old_no == lines[i].old_no);
        CHECK(rows[i].new_no == lines[i].new_no);
    }
}

}  // namespace

TEST_CASE("BuildDiffTable:write_file 新文件全 Add,与 cli 同行表") {
    const nlohmann::json input = {{"path", "no-such-dir-lubancode/no-file.txt"}, {"content", "a\nb\nc"}};
    const auto table = rt::BuildDiffTable("write_file", input);
    REQUIRE(table.has_value());
    CHECK_FALSE(table->old_exists);
    CHECK(table->path == "no-such-dir-lubancode/no-file.txt");
    // cli 侧同款(write 旧文 nullopt = 全 Add)。
    const auto cli_lines = cli::BuildWriteDiff(std::nullopt, "a\nb\nc");
    CheckRowsMatch(table->rows, cli_lines);
    CHECK(table->added_lines() == 3);
    CHECK(table->removed_lines() == 0);
}

namespace {
// 临时目录里的旧文读回来喂 cli 侧(同一份磁盘真值,两家算法吃同一输入)。
std::optional<std::string> Slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
}  // namespace

TEST_CASE("BuildDiffTable:write_file 覆盖,行级 diff 两家一致") {
    const TempFile old_file("line1\nline2\nline3\n");
    const nlohmann::json input = {{"path", old_file.path()}, {"content", "line1\nCHANGED\nline3\nextra"}};
    const auto table = rt::BuildDiffTable("write_file", input);
    REQUIRE(table.has_value());
    CHECK(table->old_exists);
    const auto cli_lines = cli::BuildWriteDiff(Slurp(old_file.path()), "line1\nCHANGED\nline3\nextra");
    CheckRowsMatch(table->rows, cli_lines);
    CHECK(table->added_lines() == 2);
    CHECK(table->removed_lines() == 1);
}

TEST_CASE("BuildDiffTable:edit_file 定位成功,facts 与行表对齐") {
    const TempFile old_file("keep\nTARGET\nkeep2\nkeep3\n");
    const nlohmann::json input = {
        {"path", old_file.path()}, {"old_string", "TARGET"}, {"new_string", "REPLACED"}};
    const auto table = rt::BuildDiffTable("edit_file", input);
    REQUIRE(table.has_value());
    CHECK(table->located);
    CHECK(table->replaced_count == 1);
    const auto cli_edit = cli::BuildEditDiff(*Slurp(old_file.path()), "TARGET", "REPLACED", false);
    REQUIRE(cli_edit.located);
    CheckRowsMatch(table->rows, cli_edit.lines);
}

TEST_CASE("BuildDiffTable:edit_file 定位失败走段内回退") {
    const TempFile old_file("nothing relevant here\n");
    const nlohmann::json input = {
        {"path", old_file.path()}, {"old_string", "GONE"}, {"new_string", "NEW\nSTUFF"}};
    const auto table = rt::BuildDiffTable("edit_file", input);
    REQUIRE(table.has_value());
    CHECK_FALSE(table->located);
    CHECK(table->replaced_count == 0);
    const auto cli_edit = cli::BuildEditDiff(*Slurp(old_file.path()), "GONE", "NEW\nSTUFF", false);
    REQUIRE_FALSE(cli_edit.located);
    CheckRowsMatch(table->rows, cli_edit.lines);
    // 回退行表照样可用:全是增删。
    CHECK(table->added_lines() == 2);
    CHECK(table->removed_lines() == 1);
}

TEST_CASE("BuildDiffTable:edit_file replace_all 记出现次数") {
    const TempFile old_file("x\nMARK\ny\nMARK\nz\nMARK\n");
    const nlohmann::json input = {{"path", old_file.path()},
                                  {"old_string", "MARK"},
                                  {"new_string", "HIT"},
                                  {"replace_all", true}};
    const auto table = rt::BuildDiffTable("edit_file", input);
    REQUIRE(table.has_value());
    CHECK(table->located);
    CHECK(table->replaced_count == 3);
    const auto cli_edit = cli::BuildEditDiff(*Slurp(old_file.path()), "MARK", "HIT", true);
    CheckRowsMatch(table->rows, cli_edit.lines);
}

TEST_CASE("BuildDiffTable:别的工具没有领域 diff,给 nullopt") {
    for (const std::string name : {"run_command", "read_file", "search", "agent", "todo_write", "mcp__x__y"}) {
        CHECK_FALSE(rt::BuildDiffTable(name, nlohmann::json{{"path", "a.txt"}}).has_value());
    }
}

TEST_CASE("BuildDiffTable:CRLF 旧文与 LF 新文对得上,不生假差异") {
    const TempFile old_file("a\r\nb\r\n");
    const nlohmann::json input = {{"path", old_file.path()}, {"content", "a\nb\n"}};
    const auto table = rt::BuildDiffTable("write_file", input);
    REQUIRE(table.has_value());
    // cli 侧同款输入也全 Context(两端算法同钉)。
    const auto cli_lines = cli::BuildWriteDiff(Slurp(old_file.path()), "a\nb\n");
    CheckRowsMatch(table->rows, cli_lines);
    CHECK(table->removed_lines() == 0);
    CHECK(table->added_lines() == 0);
    CHECK(table->rows.size() == 2);
    for (const auto& row : table->rows) {
        CHECK(row.kind == rt::DiffRowKind::Context);
    }
}

TEST_CASE("TruncateUtf8Bytes:与 cli 同解码,不劈多字节") {
    const std::string han = "三五个汉字一串";
    CHECK(rt::TruncateUtf8Bytes(han, 64) == han);
    // 9 汉字 27 字节;限 10 字节落在第二个汉字中间,退到 9(前三字节)。
    CHECK(rt::TruncateUtf8Bytes(han, 10) == han.substr(0, 9));
    CHECK(rt::TruncateUtf8Bytes(han, 10) == cli::TruncateUtf8Bytes(han, 10));
    CHECK(rt::TruncateUtf8Bytes(han, 0).empty());
    CHECK(rt::TruncateUtf8Bytes(std::string(), 10).empty());
}

TEST_CASE("TurnItem 枚举 <-> 稳定字符串单射") {
    const rt::TurnItemStatus statuses[] = {
        rt::TurnItemStatus::Pending, rt::TurnItemStatus::Running,   rt::TurnItemStatus::Succeeded,
        rt::TurnItemStatus::Failed,  rt::TurnItemStatus::Declined,  rt::TurnItemStatus::Cancelled,
    };
    for (const auto status : statuses) {
        rt::TurnItemStatus parsed{};
        CHECK(rt::ParseTurnItemStatus(rt::ToString(status), parsed));
        CHECK(parsed == status);
    }
    rt::TurnItemStatus sink{};
    CHECK_FALSE(rt::ParseTurnItemStatus("nope", sink));

    const rt::TurnItemKind kinds[] = {
        rt::TurnItemKind::Tool, rt::TurnItemKind::SubTool, rt::TurnItemKind::Thinking,
        rt::TurnItemKind::Text, rt::TurnItemKind::Command, rt::TurnItemKind::Diff,
        rt::TurnItemKind::Todo, rt::TurnItemKind::Subagent,
    };
    for (const auto kind : kinds) {
        rt::TurnItemKind parsed{};
        CHECK(rt::ParseTurnItemKind(rt::ToString(kind), parsed));
        CHECK(parsed == kind);
    }
    rt::TurnItemKind kind_sink{};
    CHECK_FALSE(rt::ParseTurnItemKind("nope", kind_sink));
}

TEST_CASE("TurnItem:finished() 只认终态四分") {
    rt::TurnItem item;
    CHECK_FALSE(item.finished());
    item.status = rt::TurnItemStatus::Pending;
    CHECK_FALSE(item.finished());
    for (const auto status : {rt::TurnItemStatus::Succeeded, rt::TurnItemStatus::Failed,
                              rt::TurnItemStatus::Declined, rt::TurnItemStatus::Cancelled}) {
        item.status = status;
        CHECK(item.finished());
    }
}
