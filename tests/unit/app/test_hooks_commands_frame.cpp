// TUI 排版批 2(tui优化todo.todo P2:/hooks 全族)的输出形状册。
//   - list 主表(id/event/matcher/state/hash/last)+ 明细表(id/command/
//     source/exec)+ 动作提示键值对框;state 列 pass/skip 语义色;
//   - runs 空态/trust 反馈/错误提示全进 frame,不再裸打印;
//   - plain 主题(--no-color/T3 降级路径)钉零转义字节、无框字形——合同
//     第 3 条;
//   - /hooks 无 --json/机器消费分支(源码核实),字节级不变一条天然满足。
//
// 走真 HandleHooksCommand(手造 HookDispatcher + TermPort 改道捕获),
// 断言只看形状与相对位置,不看绝对宽度(测试进程探不到终端,框宽按内容
// 自适应;真机宽度统一由主人会话内连敲验收)。

#include <doctest/doctest.h>

#include <sstream>
#include <string>
#include <vector>

#include "app/commands/hook_commands.hpp"
#include "cli/line_editor.hpp"  // DisplayWidthUtf8:对齐断言按显示列量
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "config/config.hpp"
#include "hooks/dispatcher.hpp"
#include "hooks/hash.hpp"
#include "hooks/loader.hpp"
#include "hooks/protocol.hpp"

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

std::string StripAnsi(const std::string& text) {
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

constexpr const char* kBoxLightTopLeft = "\xe2\x94\x8c";  // ┌
constexpr const char* kBoxLightVert = "\xe2\x94\x82";     // │

hooks::HookDefinition MakeDefinition(int event_no, hooks::HookSourceKind kind, bool trusted,
                                     const std::string& command) {
    hooks::HookDefinition def;
    def.event = static_cast<hooks::HookEvent>(event_no);
    def.source_kind = kind;
    def.source_path = "test://source";
    def.source_label = kind == hooks::HookSourceKind::Project ? "project test://source"
                                                              : "user test://source";
    def.matcher = "*";
    def.handler.command = command;
    def.handler.timeout_ms = 30000;
    def.handler.failure_policy = "warn";
    def.definition_hash = hooks::ComputeDefinitionHash(def.handler);
    def.definition_hash_short = hooks::DefinitionHashShort(def.definition_hash);
    def.trusted = trusted;
    return def;
}

hooks::HookDispatcher MakeDispatcher(std::vector<hooks::HookDefinition> defs) {
    hooks::LoadedHooks loaded;
    loaded.definitions = std::move(defs);
    hooks::HookTrustStore trust = hooks::HookTrustStore::Load(std::nullopt).first;
    hooks::HookContext ctx;
    ctx.session_id = "frame-test-session";
    ctx.turn_id = "frame-test-turn";
    ctx.cwd = "/test";
    ctx.permission_mode = "confirm";
    hooks::HookDispatcher dispatcher;
    dispatcher.Configure(std::move(loaded), std::move(trust), std::move(ctx));
    return dispatcher;
}

struct HooksRig {
    hooks::HookDispatcher dispatcher;
    cli::Theme theme;
    cli::Theme plain_theme;

    explicit HooksRig(std::vector<hooks::HookDefinition> defs, const char* theme_name = "dark")
        : dispatcher(MakeDispatcher(std::move(defs))),
          theme(cli::BuiltinTheme(theme_name)),
          plain_theme(cli::BuiltinTheme("plain")) {}

    std::string Run(const std::string& args, const cli::Theme& use_theme) {
        OutputCapture capture;
        app::HandleHooksCommand(args, &dispatcher, use_theme);
        return capture.text();
    }
};

}  // namespace

TEST_CASE("list: 主表 + 明细表 + 动作提示框,state 列 pass/skip 语义色") {
    std::vector<hooks::HookDefinition> defs;
    defs.push_back(MakeDefinition(0, hooks::HookSourceKind::User, /*trusted=*/true, "echo user-hook"));
    defs.push_back(
        MakeDefinition(0, hooks::HookSourceKind::Project, /*trusted=*/false, "echo project-hook"));
    HooksRig rig(std::move(defs));
    const std::string out = rig.Run("list", rig.theme);

    // 主表:标题(既有首句进框顶,尾冒号剥掉)+ 表头行 + 两行 hook。
    REQUIRE(Contains(out, kBoxLightTopLeft));
    const std::string plain = StripAnsi(out);
    CHECK(Contains(plain, "已装载 2 条 hook 定义"));
    CHECK(plain.find("已装载 2 条 hook 定义(user 与项目配置相加;项目级须先信任才执行):") ==
          std::string::npos);  // 引导下文的冒号不再连排
    bool saw_header = false;
    std::istringstream lines(plain);
    std::string line;
    while (std::getline(lines, line)) {
        if (Contains(line, "matcher") && Contains(line, "state") && Contains(line, "hash")) {
            saw_header = true;
        }
    }
    CHECK(saw_header);
    // 明细表:命令全文与来源在第二张表(列头 command/source)。
    CHECK(Contains(plain, "command"));
    CHECK(Contains(plain, "source"));
    CHECK(Contains(plain, "echo project-hook"));
    CHECK(Contains(plain, "user test://source"));
    // 动作提示进键值对框("动作"拆列,不再裸打印)。
    CHECK(Contains(plain, "动作"));
    CHECK(Contains(plain, "/hooks trust <#id>"));

    // state 语义色:user 级(免审查)与 project 未信任同表,pass/skip 分档。
    CHECK(Contains(out, rig.theme.table_pass + "用户级(免审查)"));
    CHECK(Contains(out, rig.theme.table_skip + "待审查(未信任,已跳过)"));

    // 列对齐:两行的 state 单元格起始显示列一致(id/event/matcher 列宽
    // 对齐后 state 处处同列)。
    std::istringstream align_lines(plain);
    int user_col = -1;
    int project_col = -1;
    while (std::getline(align_lines, line)) {
        if (Contains(line, "用户级(免审查)")) user_col = DisplayColOf(line, "用户级(免审查)");
        if (Contains(line, "待审查(未信任,已跳过)")) project_col = DisplayColOf(line, "待审查");
    }
    CHECK(user_col > 0);
    CHECK(project_col > 0);
    CHECK(user_col == project_col);
}

TEST_CASE("list: 空态带 frame,不再裸打印") {
    HooksRig rig({});
    const std::string out = rig.Run("list", rig.theme);
    REQUIRE(Contains(out, kBoxLightTopLeft));
    CHECK(Contains(StripAnsi(out), "没有装载任何 hooks"));
}

TEST_CASE("runs: 空态进框") {
    std::vector<hooks::HookDefinition> defs;
    defs.push_back(MakeDefinition(0, hooks::HookSourceKind::User, true, "echo user-hook"));
    HooksRig rig(std::move(defs));
    const std::string out = rig.Run("runs", rig.theme);
    REQUIRE(Contains(out, kBoxLightTopLeft));
    CHECK(Contains(StripAnsi(out), "还没有任何 hook 运行记录"));
}

TEST_CASE("trust/enable 反馈进 frame;managed 拒绝与查无此条上 error 色") {
    std::vector<hooks::HookDefinition> defs;
    defs.push_back(MakeDefinition(0, hooks::HookSourceKind::Project, /*trusted=*/false, "echo project-hook"));
    HooksRig rig(std::move(defs));
    {
        const std::string out = rig.Run("trust #1", rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "已信任当前 hash"));
    }
    {
        const std::string out = rig.Run("disable #99", rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "没有 #99 这条定义"));
        CHECK(Contains(out, rig.theme.error));
    }
    {
        const std::string out = rig.Run("enable #1", rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "已重新启用"));
    }
}

TEST_CASE("plain 主题: 全族输出零转义字节、无框字形(T3/--no-color 路径)") {
    std::vector<hooks::HookDefinition> defs;
    defs.push_back(MakeDefinition(0, hooks::HookSourceKind::Project, /*trusted=*/false, "echo project-hook"));
    HooksRig rig(std::move(defs));
    for (const std::string& args : {"list", "runs", "trust #1", "bogus-sub"}) {
        CAPTURE(args);
        const std::string out = rig.Run(args, rig.plain_theme);
        CHECK(out.find("\x1b") == std::string::npos);
        CHECK(!Contains(out, kBoxLightTopLeft));
        CHECK(!Contains(out, kBoxLightVert));
        // 信息一字不少:表头与 state 文本都在,只是没了色与框。
        if (args == "list") {
            CHECK(Contains(out, "matcher"));
            CHECK(Contains(out, "待审查(未信任,已跳过)"));
        }
    }
}
