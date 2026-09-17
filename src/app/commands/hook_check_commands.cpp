// LuaHook 单 P1-D 的 hook 子命令实现。validate/test 是纯递材料 + 打印:
// 引擎(runtime::RunHookPackageCheck)四档分账,这里不添第二套判据;init
// 落官方 scaffold(与 examples/hooks/prompt-clean 同款形状,生成即可跑)。
#include "app/commands/hook_check_commands.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

#include "cli/terminal_port.hpp"  // TermOut/TermErr
#include "platform/paths.hpp"     // Utf8ToPath
#include "runtime/hook_package_check.hpp"

namespace lubancode::app {

namespace {
using cli::TermErr;
using cli::TermOut;

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
        std::cout << report.ToJson().dump(2) << "\n";
        std::cout.flush();
        return report.exit_code();
    }

    TermOut() << "hook " << args.verb << " " << args.package_dir << "\n";
    TermOut() << "静态检查:\n";
    for (const runtime::HookCheckReport::Check& check : report.checks) {
        TermOut() << "  [" << (check.pass ? "pass" : "FAIL") << "] " << check.item << "  " << check.detail
                  << "\n";
    }
    if (args.verb == "test") {
        TermOut() << "fixtures(fake adapter,真实 Lua runtime):\n";
        if (report.fixtures.empty()) {
            TermOut() << "  (无用例;行为未验)\n";
        }
        for (const runtime::HookCheckReport::Fixture& fixture : report.fixtures) {
            TermOut() << "  [" << (!fixture.ran ? "SKIP" : fixture.pass ? "pass" : "FAIL") << "] "
                      << fixture.name << "  " << fixture.detail << "\n";
        }
    }
    if (!report.unverified.empty()) {
        TermOut() << "未验项(明列,不冒充):\n";
        for (const std::string& item : report.unverified) {
            TermOut() << "  - " << item << "\n";
        }
    }
    TermOut() << "退出码 " << report.exit_code() << "(0 全过 / 1 有 fail / 2 包读不到)\n";
    TermOut().flush();
    return report.exit_code();
}

int RunHookInitCommand(const HookCliArgs& args) {
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
    TermOut() << "scaffold 已生成: " << package_dir.string() << "\n";
    TermOut() << "下一步:\n";
    TermOut() << "  lubancode hook test " << package_dir.string() << "\n";
    TermOut() << "手册: docs/features/hooks/(挂点 / 中间件配置 / Host API)\n";
    TermOut().flush();
    return 0;
}

}  // namespace lubancode::app
