// TUI 排版批 4(/model 清单与角色表)的输出形状册。
//   - RenderModelChoiceList(管道编号表):编号列右对齐、model 列自适应,
//     current 行的 current 单元格走 table_pass 语义色(单子写的
//     tool_accent 在主题里不存在,批 0 合同取 pass 档);
//   - 长清单(单子验收:结果行数 ≥50 时不破对齐)——55 项模型钉表格行
//     全在框内、model 列起始列一致;
//   - PrintModelRolesTable:roles_header(尾冒号剥掉)作标题,五列 schema 名;
//   - plain 主题:零转义、无框,表头下垫 "-" 横线。

#include <doctest/doctest.h>

#include <sstream>
#include <string>
#include <vector>

#include "agent/model_router.hpp"  // ModelRouteTable/ModelRoute
#include "app/commands/settings_commands.hpp"
#include "cli/line_editor.hpp"  // DisplayWidthUtf8:对齐断言按显示列量
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "runtime/command_service.hpp"  // ModelQueryResult

using namespace lubancode;

namespace {

class OutputCapture {
public:
    OutputCapture() { cli::TermPort().Redirect(&stream_, nullptr); }
    ~OutputCapture() { cli::TermPort().Reset(); }
    std::string text() const { return stream_.str(); }

private:
    std::ostringstream stream_;
};

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

int DisplayColOf(const std::string& row, const std::string& needle) {
    const std::size_t at = row.find(needle);
    if (at == std::string::npos) {
        return -1;
    }
    return static_cast<int>(cli::DisplayWidthUtf8(row.substr(0, at)));
}

// 剥掉 CSI 序列(与 test_plugin_commands_frame.cpp 同一把手写的尺)。
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

constexpr const char* kBoxLightTopLeft = "\xe2\x94\x8c";  // ┌
constexpr const char* kBoxLightVert = "\xe2\x94\x82";     // │

runtime::ModelQueryResult MakeQuery(std::size_t count) {
    runtime::ModelQueryResult query;
    for (std::size_t i = 0; i < count; ++i) {
        std::string suffix = std::to_string(i);
        while (suffix.size() < 3) {
            suffix = "0" + suffix;
        }
        runtime::ModelListEntry entry;
        entry.id = "model-" + suffix;
        // 名字长短不一(9/11 位):对齐断言要的就是不等宽的 label。
        entry.display_name = i % 2 == 0 ? entry.id : entry.id + "-turbo";
        entry.current = i == 7;
        query.models.push_back(std::move(entry));
    }
    query.current_model = "model-007";
    return query;
}

config::ModelCatalog EmptyCatalog() { return config::ModelCatalog{}; }

}  // namespace

TEST_CASE("清单表:current 行走 table_pass,表头三列 schema 名(dark)") {
    const cli::Theme dark = cli::BuiltinTheme("dark");
    // MakeQuery 钉 current 在 model-007,8 项齐。
    const std::vector<std::string> lines =
        app::RenderModelChoiceList(MakeQuery(8), EmptyCatalog(), dark, /*width=*/0);
    REQUIRE(!lines.empty());
    // 表头三列 schema 名(dark 下 lines[0] 是上边框,表头按内容找)。
    std::string header;
    for (const std::string& line : lines) {
        const std::string plain = StripAnsiLight(line);
        if (plain.find("current") != std::string::npos && plain.find("model-") == std::string::npos) {
            header = plain;
            break;
        }
    }
    REQUIRE(!header.empty());
    CHECK(Contains(header, "#"));
    CHECK(Contains(header, "model"));
    // current 行的 current 标记走 table_pass 语义色(批 4:单子写的
    // tool_accent 不在主题,按批 0 合同"色全走 Theme 字段"取 pass 档)。
    bool current_row_colored = false;
    for (const std::string& line : lines) {
        if (line.find("model-007") != std::string::npos && Contains(line, dark.table_pass)) {
            current_row_colored = true;
        }
    }
    CHECK(current_row_colored);
}

TEST_CASE("清单表:55 项长清单列对齐不破,一行不掉出框(dark)") {
    const cli::Theme dark = cli::BuiltinTheme("dark");
    constexpr std::size_t kModelCount = 55;
    const std::vector<std::string> lines =
        app::RenderModelChoiceList(MakeQuery(kModelCount), EmptyCatalog(), dark, /*width=*/0);
    REQUIRE(!lines.empty());

    int framed_rows = 0;
    int label_col = -1;
    bool col_consistent = true;
    std::size_t seen_models = 0;
    for (const std::string& line : lines) {
        const std::string plain = StripAnsiLight(line);
        if (plain.find("model-") == std::string::npos) {
            continue;
        }
        ++seen_models;
        if (Contains(plain, kBoxLightVert)) {
            ++framed_rows;
        }
        const int col = DisplayColOf(plain, "model-");
        if (label_col < 0) {
            label_col = col;
        } else if (col != label_col) {
            col_consistent = false;
        }
    }
    CHECK(seen_models == kModelCount);
    CHECK(framed_rows == static_cast<int>(kModelCount));  // 一行都不掉出框
    CHECK(col_consistent);  // model 列起始显示列处处一致
    CHECK(label_col > 0);
    // 编号列右对齐:第 1 行的 "1" 与第 54 行(model-053-turbo)的 "54"
    // 个位在同一显示列。
    int unit_of_1 = -1;
    int unit_of_54 = -1;
    for (const std::string& line : lines) {
        const std::string plain = StripAnsiLight(line);
        if (plain.find("model-000") != std::string::npos) {
            unit_of_1 = DisplayColOf(plain, "1");
        }
        if (plain.find("model-053-turbo") != std::string::npos) {
            unit_of_54 = DisplayColOf(plain, "5");
        }
    }
    CHECK(unit_of_1 > 0);
    CHECK(unit_of_54 > 0);
    CHECK(unit_of_1 == unit_of_54);
}

TEST_CASE("roles 表:标题剥尾冒号,三行五列 schema 名(dark)") {
    const cli::Theme dark = cli::BuiltinTheme("dark");
    agent::ModelRouteTable table;
    table.normal.provider = "openai";
    table.normal.model = "gpt-5.4";
    table.normal.effort = "high";
    table.normal.source = "项目配置";
    table.cheap.source = "model_roles 段(项目级)";
    {
        OutputCapture capture;
        app::PrintModelRolesTable(&table, dark);
        const std::string out = capture.text();
        REQUIRE(Contains(out, kBoxLightTopLeft));
        const std::string plain = StripAnsiLight(out);
        // 标题句不带尾冒号(批 2 裁量 3);三行角色与列名都在册。
        CHECK(Contains(plain, "三档模型角色"));
        CHECK(plain.find("normal):") == std::string::npos);
        CHECK(Contains(plain, "cheap"));
        CHECK(Contains(plain, "normal"));
        CHECK(Contains(plain, "lao"));
        CHECK(Contains(plain, "provider"));
        CHECK(Contains(plain, "gpt-5.4"));
        CHECK(Contains(plain, "项目配置"));
        // 未配置的角色如实摆"(未定)"/"(活跃端)"。
        CHECK(Contains(plain, "(活跃端)"));
        CHECK(Contains(plain, "(未定)"));
    }
    {
        // 空指针:roles_unavailable 进键值对框,不装没事发生。
        OutputCapture capture;
        app::PrintModelRolesTable(nullptr, dark);
        CHECK(Contains(capture.text(), kBoxLightTopLeft));
    }
}

TEST_CASE("plain 主题:零转义、无框,表头下垫横线") {
    const cli::Theme plain = cli::BuiltinTheme("plain");
    const std::vector<std::string> lines =
        app::RenderModelChoiceList(MakeQuery(4), EmptyCatalog(), plain, /*width=*/0);
    std::string joined;
    for (const std::string& line : lines) {
        CHECK(line.find("\x1b") == std::string::npos);
        CHECK(!Contains(line, kBoxLightTopLeft));
        CHECK(!Contains(line, kBoxLightVert));
        joined += StripAnsiLight(line);
    }
    REQUIRE(lines.size() >= 3);
    // 表头行下垫一条 "-" 横线顶替颜色分隔(批 0 降级合同)。
    CHECK(lines[1].find('-') != std::string::npos);
    CHECK(Contains(joined, "model-001"));
    CHECK(Contains(joined, "current"));
}
