// LuaHook 单 P1-D 的 hook 子命令实现。validate/test 是纯递材料 + 打印:
// 引擎(runtime::RunHookPackageCheck)四档分账,这里不添第二套判据;init
// 落官方 scaffold(与 examples/hooks/prompt-clean 同款形状,生成即可跑)。
//
// TUI 排版批 2:非 --json 的报告走 cli::frame::* 三助手(表格 pass/FAIL/
// SKIP 三态色);--json 分支只收口输出端口(std::cout -> TermOut,单子合同
// 第 4 条点名的旧账),落盘字节级不变。CLI 子命令没有会话主题,按
// ManageSession 先例现起一只(管道/重定向自然降 plain)。
#include "app/commands/hook_check_commands.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "cli/terminal_frame.hpp"  // frame::*(TUI 排版批 2:报告渲染段)
#include "cli/terminal_port.hpp"   // TermOut/TermErr
#include "cli/theme.hpp"           // ResolveTheme:CLI 侧主题
#include "platform/console.hpp"    // GetScreenInfo:框宽同一把尺
#include "platform/paths.hpp"      // Utf8ToPath
#include "runtime/hook_package_check.hpp"

namespace lubancode::app {

namespace {
using cli::TermErr;
using cli::TermOut;

namespace frame = lubancode::cli::frame;

int HookCliFrameWidth() {
    if (const auto info = lubancode::platform::GetScreenInfo()) {
        return info->width;
    }
    return 0;
}

void EmitFrameLines(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        TermOut() << line << "\n";
    }
}

std::string TrimAscii(std::string value) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

frame::Field SentenceField(const std::string& sentence,
                           frame::FieldAccent accent = frame::FieldAccent::None) {
    const std::size_t colon = sentence.find(':');
    if (colon == std::string::npos) {
        return frame::Field{"", sentence, accent};
    }
    return frame::Field{TrimAscii(sentence.substr(0, colon)), TrimAscii(sentence.substr(colon + 1)), accent};
}

void PrintNotice(const lubancode::cli::Theme& theme, std::initializer_list<std::string> sentences,
                 frame::FieldAccent accent = frame::FieldAccent::None) {
    std::vector<frame::Field> fields;
    for (const std::string& sentence : sentences) {
        fields.push_back(SentenceField(sentence, accent));
    }
    EmitFrameLines(frame::RenderKeyValues({}, fields, theme, frame::Light(), HookCliFrameWidth()));
}

bool WriteFileIfAbsent(const std::filesystem::path& path, const std::string& content, std::string& error) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec) || ec) {
        error = "已存在,不覆盖: " + path.string();
        return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        error = "写不进: " + path.string();
        return false;
    }
    file << content;
    if (file.bad()) {
        error = "写半截失败: " + path.string();
        return false;
    }
    return true;
}

// scaffold 内容:与 docs/features/hooks 的作者手册同一份合同;生成后
// `lubancode hook test <目录>` 全绿(fixtures 自带断言)。
std::string ScaffoldManifest(const std::string& id) {
    return R"({
  "schemaVersion": 1,
  "id": ")" + id + R"(",
  "version": "0.1.0",
  "entry": "main.lua",
  "hooks": [
    {
      "hookPoint": "PreUser",
      "name": "prompt.normalize",
      "handler": "normalize",
      "priority": 100,
      "match": {"origin": "human", "purpose": "interactive"}
    }
  ]
}
)";
}

const char* kScaffoldMainLua = R"(-- scaffold:PreUser 清洗首尾空白。
-- 合同:handler(ctx, input, next);候选经 next(candidate) 提交宿主校验,
-- 本地改 input 不影响 session。返回 next 的下游值(透传)。
local function trim(s)
    return (s:gsub("^%s*(.-)%s*$", "%1"))
end

return {
    normalize = function(ctx, input, next)
        if type(input.prompt) ~= "string" then
            return next(input)
        end
        local candidate = {
            prompt = trim(input.prompt),
        }
        for key, value in pairs(input) do
            if key ~= "prompt" then
                candidate[key] = value
            end
        end
        return next(candidate)
    end,
}
)";

const char* kScaffoldFixtureClean = R"({
  "name": "trims-surrounding-whitespace",
  "trigger": {
    "input": {"prompt": "  你好,LubanCode  "},
    "origin": "human",
    "purpose": "interactive",
    "deliveryMode": "steer"
  },
  "expect": {
    "kind": "completed",
    "prompt": "你好,LubanCode",
    "terminalRuns": 1,
    "records": {"PreUser/prompt.normalize": {"outcome": "completed", "nextCalls": 1}}
  }
}
)";

const char* kScaffoldFixturePass = R"({
  "name": "leaves-clean-prompt-alone",
  "trigger": {
    "input": {"prompt": "已经干净"},
    "origin": "human",
    "purpose": "interactive"
  },
  "expect": {
    "kind": "completed",
    "prompt": "已经干净"
  }
}
)";

const char* kScaffoldReadme = R"(# <scaffold 生成的 hook 包>

`lubancode hook test .` 应全绿。改法见 docs/features/hooks/ 三册手册
(挂点 / 中间件配置 / Host API)。

- hook.json:清单(schemaVersion 1);定义元数据集中在这,Lua 只返回 handler 表。
- main.lua:入口脚本;顶层只构造函数与常量。
- fixtures/:试跑用例;`lubancode hook test` 用真实 Lua runtime 跑,
  HTTP/文件/工具走 fake adapter。
)";

bool IsValidHookId(const std::string& id) {
    if (id.empty() || id.size() > 64) {
        return false;
    }
    for (const unsigned char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

}  // namespace

int RunHookCheckCommand(const HookCliArgs& args) {
    runtime::HookCheckOptions options;
    options.run_fixtures = args.verb == "test";
    const runtime::HookCheckReport report =
        runtime::RunHookPackageCheck(platform::Utf8ToPath(args.package_dir), options);
    if (args.json) {
        // 机器面(--json):输出字节级不变,只收口输出端口——std::cout 的
        // 直写违规是单子合同第 4 条点名的旧账,本批收口。TermOut 默认落
        // stdout,与 std::cout 同一目的地。
        TermOut() << report.ToJson().dump(2) << "\n";
        TermOut().flush();
        return report.exit_code();
    }
    const lubancode::cli::Theme theme = lubancode::cli::ResolveTheme(
        std::string(), lubancode::cli::DetectConsoleCapability().colors_enabled);

    // 命令回显一行照旧(非报告正文,不进框;与批 1 "/memory show 正文框外
    // 原样"同一裁量)。
    TermOut() << "hook " << args.verb << " " << args.package_dir << "\n";
    // 静态检查表:result 列 pass=Pass 色、FAIL=Fail 色(单子批 2 三态色的
    // 前两档;SKIP 只出现在 fixtures)。
    if (!report.checks.empty()) {
        std::vector<frame::TableColumn> columns;
        columns.push_back({"result"});
        columns.push_back({"item"});
        columns.push_back({"detail"});
        std::vector<frame::TableRow> rows;
        for (const runtime::HookCheckReport::Check& check : report.checks) {
            rows.push_back(frame::TableRow{{check.pass ? "pass" : "FAIL", check.item, check.detail},
                                           {check.pass ? frame::CellTone::Pass : frame::CellTone::Fail}});
        }
        EmitFrameLines(
            frame::RenderTable("静态检查", columns, rows, theme, frame::Light(), HookCliFrameWidth()));
    }
    if (args.verb == "test") {
        if (report.fixtures.empty()) {
            PrintNotice(theme, {"(无用例;行为未验)"});
        } else {
            // fixtures 表:SKIP=Skip 色、pass=Pass 色、FAIL=Fail 色——三态
            // 齐了(单子批 2 验收点)。
            std::vector<frame::TableColumn> columns;
            columns.push_back({"result"});
            columns.push_back({"name"});
            columns.push_back({"detail"});
            std::vector<frame::TableRow> rows;
            for (const runtime::HookCheckReport::Fixture& fixture : report.fixtures) {
                const char* result = !fixture.ran ? "SKIP" : fixture.pass ? "pass" : "FAIL";
                const frame::CellTone tone =
                    !fixture.ran ? frame::CellTone::Skip : fixture.pass ? frame::CellTone::Pass : frame::CellTone::Fail;
                rows.push_back(frame::TableRow{{result, fixture.name, fixture.detail}, {tone}});
            }
            EmitFrameLines(frame::RenderTable("fixtures(fake adapter,真实 Lua runtime)", columns, rows, theme,
                                              frame::Light(), HookCliFrameWidth()));
        }
    }
    if (!report.unverified.empty()) {
        std::vector<frame::ListRow> rows;
        for (const std::string& item : report.unverified) {
            rows.push_back(frame::ListRow{item, {}, {}, frame::Bullet::None});
        }
        EmitFrameLines(
            frame::RenderList("未验项(明列,不冒充)", rows, theme, frame::Light(), HookCliFrameWidth()));
    }
    PrintNotice(theme, {"退出码 " + std::to_string(report.exit_code()) + "(0 全过 / 1 有 fail / 2 包读不到)"});
    TermOut().flush();
    return report.exit_code();
}

int RunHookInitCommand(const HookCliArgs& args) {
    const lubancode::cli::Theme theme = lubancode::cli::ResolveTheme(
        std::string(), lubancode::cli::DetectConsoleCapability().colors_enabled);
    if (!IsValidHookId(args.name)) {
        TermErr() << "hook 名字只认字母/数字/-/_(最长 64): " << args.name << "\n";
        return 2;
    }
    std::error_code cwd_ec;
    const std::filesystem::path cwd = std::filesystem::current_path(cwd_ec);
    const std::filesystem::path parent =
        args.parent_dir.empty() ? (cwd_ec ? std::filesystem::path(".") : cwd)
                                : platform::Utf8ToPath(args.parent_dir);
    const std::filesystem::path package_dir = parent / args.name;
    std::error_code ec;
    if (std::filesystem::exists(package_dir, ec) || ec) {
        TermErr() << "目标目录已存在,不覆盖: " << package_dir.string() << "\n";
        return 2;
    }
    std::filesystem::create_directories(package_dir / "fixtures", ec);
    if (ec) {
        TermErr() << "建目录失败: " << package_dir.string() << " (" << ec.message() << ")\n";
        return 2;
    }
    const std::vector<std::pair<std::filesystem::path, std::string>> files = {
        {package_dir / "hook.json", ScaffoldManifest(args.name)},
        {package_dir / "main.lua", kScaffoldMainLua},
        {package_dir / "fixtures" / "clean.json", kScaffoldFixtureClean},
        {package_dir / "fixtures" / "pass.json", kScaffoldFixturePass},
        {package_dir / "README.md", kScaffoldReadme},
    };
    for (const auto& [path, content] : files) {
        std::string error;
        if (!WriteFileIfAbsent(path, content, error)) {
            TermErr() << "scaffold 落盘失败: " << error << "\n";
            return 2;
        }
    }
    // 成功提示进键值对框("scaffold 已生成"/"下一步"/"手册"按冒号拆两列;
    // 下一步的命令行并进 value,信息一字不丢)。错误仍走 TermErr 原样
    // (CLI 错误流,端口不切换)。
    PrintNotice(theme, {"scaffold 已生成: " + package_dir.string(),
                        "下一步: lubancode hook test " + package_dir.string(),
                        "手册: docs/features/hooks/(挂点 / 中间件配置 / Host API)"});
    TermOut().flush();
    return 0;
}

}  // namespace lubancode::app
