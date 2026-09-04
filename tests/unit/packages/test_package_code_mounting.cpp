// 统一 Package 封装单阶段 5 的挂载事务册:暂存/发布/回滚三段(单子 §十
// "整包成,整包败"的执行版)。测试账对齐单子 §十六"事务与生命周期"与
// 阶段 5 验收线:
//   - 三件套好包(两插件一 MCP)整包进 ToolRegistry,wire 名与 ToolOrigin
//     来源账齐;
//   - 一件起不来,三件全不进、已起进程全停(进程零残留)、诊断指到坏件;
//   - 占位符展开(${package_dir}/${package_data}/${env:NAME})与 mcp.yaml
//     折 McpServerConfig;
//   - 信任门联动:未批的包压根不进事务;
//   - store 选中版本同路(scope=Store 的候选走同一事务)。
// 真进程夹具:code-stack 与 broken/code-failure(见各自 README);POSIX 上
// 解释器名是 python3,拷进临时层时改写 command。
#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "app/tool_runtime.hpp"  // RegisterMcpTools/PublishPackagedPlugins(发布段)
#include "mcp/client.hpp"
#include "package/catalog.hpp"
#include "package/code_mounting.hpp"
#include "package/component.hpp"
#include "package/inventory.hpp"
#include "package/manifest.hpp"
#include "package/mounting.hpp"
#include "package/trust.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "runtime/plugin_contract.hpp"
#include "runtime/plugin_tool.hpp"
#include "tools/registry.hpp"

using namespace lubancode::package;
namespace fs = std::filesystem;

namespace {

const fs::path kFixturesRoot = fs::path(LUBANCODE_SOURCE_DIR) / "tests" / "fixtures" / "packages";

// 真夹具用哪个 python:Windows 装的是 python.exe,Linux/macOS 发行版惯例
// 是 python3(裸 python 常常不存在)——与 test_mcp_client.cpp 同一口径。
#ifdef _WIN32
constexpr const char* kPythonCmd = "python";
#else
constexpr const char* kPythonCmd = "python3";
#endif

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_pkg_code_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const fs::path& Get() const { return dir_; }

private:
    fs::path dir_;
};

std::string ReadFileText(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void WriteFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << content;
}

// 把一只夹具包拷进临时层,顺手把 command 的解释器改成当前平台的那枚。
// plugin.json 是 "command": "python",mcp.yaml 是 command: python。
fs::path CopyFixturePackage(const fs::path& src_root, const fs::path& layer, const std::string& dir_name) {
    const fs::path dst_root = layer / dir_name;
    std::error_code ec;
    fs::create_directories(dst_root, ec);
    for (auto it = fs::recursive_directory_iterator(src_root); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec || !it->is_regular_file()) continue;
        const fs::path rel = it->path().lexically_relative(src_root);
        const fs::path dst = dst_root / rel;
        fs::create_directories(dst.parent_path(), ec);
        const std::string name = it->path().filename().string();
        if (name == "plugin.json" || name == "mcp.yaml") {
            std::string text = ReadFileText(it->path());
            const std::string from_json = "\"command\": \"python\"";
            const std::string to_json = std::string("\"command\": \"") + kPythonCmd + "\"";
            const std::size_t hit_json = text.find(from_json);
            if (hit_json != std::string::npos) {
                text.replace(hit_json, from_json.size(), to_json);
            }
            const std::string from_yaml = "command: python";
            const std::size_t hit_yaml = text.find(from_yaml);
            if (hit_yaml != std::string::npos) {
                text.replace(hit_yaml, from_yaml.size(),
                             std::string("command: ") + kPythonCmd);
            }
            WriteFile(dst, text);
        } else {
            fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, ec);
        }
    }
    return dst_root;
}

// 建挂载快照(临时 dev 层,先扫一遍拿哈希、再带信任账扫一遍)。
PackageMount MountTrustedDev(const fs::path& layer) {
    PackageMountInput pending_input;
    pending_input.scan.dev_roots.push_back(layer);
    const PackageMount pending = BuildPackageMount(pending_input);
    PackageTrustSnapshot snapshot;
    for (const auto& entry : pending.entries) {
        snapshot.keys.insert(entry.package_id + "\n" + entry.content_hash);
    }
    PackageMountInput trusted_input;
    trusted_input.scan.dev_roots.push_back(layer);
    trusted_input.trust = snapshot;
    return BuildPackageMount(trusted_input);
}

// 等一个条件成立(轮询;超时返回 false)。
bool WaitFor(const std::function<bool()>& probe, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (probe()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return probe();
}

// YAML 里嵌路径用正斜杠(双引号串里反斜杠是转义符,Windows 路径会炸)。
std::string GenericUtf8(const fs::path& path) {
    const std::u8string u8 = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

}  // namespace

// ---------------------------------------------------------------------------
// 占位符展开(纯函数)
// ---------------------------------------------------------------------------
TEST_CASE("ExpandMcpValue:package_dir 展开且不越界;越界即拒") {
    TempDir temp;
    const McpExpansionContext ctx{temp.Get() / "pkg", temp.Get() / "data" / "pkg"};
    const std::string pkg_root_utf8 = lubancode::platform::PathToUtf8(temp.Get() / "pkg");

    // 展开 = 占位符替换 + 原样拼接(后继文本的斜杠不动,Windows 上混拼
    // 照样能跑;判越界的是规范化后的整值)。
    const auto inside = ExpandMcpValue("${package_dir}/mcp/ledger/server.py", ctx, false, nullptr);
    REQUIRE(inside.has_value());
    CHECK(*inside == pkg_root_utf8 + "/mcp/ledger/server.py");

    // 夹明文尾巴:照拼。
    const auto suffix = ExpandMcpValue("${package_dir}/bin/tool --flag", ctx, false, nullptr);
    REQUIRE(suffix.has_value());
    CHECK(*suffix == pkg_root_utf8 + "/bin/tool --flag");

    // 两枚绝对占位符拼在一起(值里出现第二根盘符):按越界拒——这是
    // 垃圾值,不是路径。
    CHECK_FALSE(ExpandMcpValue("${package_dir}/a/${package_dir}/b", ctx, false, nullptr).has_value());

    const auto escape = ExpandMcpValue("${package_dir}/../../elsewhere/server.py", ctx, false, nullptr);
    CHECK_FALSE(escape.has_value());
    CHECK(escape.error().find("逃出包根") != std::string::npos);

    // args 里不认 env 占位;project_dir 不认。
    CHECK_FALSE(ExpandMcpValue("${env:HOME}", ctx, false, nullptr).has_value());
    CHECK_FALSE(ExpandMcpValue("${project_dir}/x", ctx, false, nullptr).has_value());
    // env 值里不认 package_dir;只认整值 ${env:NAME}。
    CHECK_FALSE(ExpandMcpValue("${package_dir}/x", ctx, true, nullptr).has_value());
    CHECK_FALSE(ExpandMcpValue("prefix-${env:HOME}", ctx, true, nullptr).has_value());
}

TEST_CASE("ExpandMcpValue:package_data 展开;env 真值运行时取,缺的只报名") {
    TempDir temp;
    const McpExpansionContext ctx{temp.Get() / "pkg", temp.Get() / "data" / "pkg"};
    const auto data = ExpandMcpValue("${package_data}/cache/db.sqlite", ctx, false, nullptr);
    REQUIRE(data.has_value());
    CHECK(*data ==
          lubancode::platform::PathToUtf8(temp.Get() / "data" / "pkg") + "/cache/db.sqlite");

    const EnvLookup lookup = [](const std::string& name) -> std::optional<std::string> {
        if (name == "BROWSER_TOKEN") return std::string("secret-not-logged");
        return std::nullopt;
    };
    std::vector<std::string> notes;
    const auto token = ExpandMcpValue("${env:BROWSER_TOKEN}", ctx, true, lookup, &notes);
    REQUIRE(token.has_value());
    CHECK(*token == "secret-not-logged");
    CHECK(notes.empty());

    const auto missing = ExpandMcpValue("${env:NO_SUCH_VAR}", ctx, true, lookup, &notes);
    REQUIRE(missing.has_value());
    CHECK(missing->empty());  // 空 = 丢这对
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].find("NO_SUCH_VAR") != std::string::npos);
    CHECK(notes[0].find("secret") == std::string::npos);  // 只报名不报值
}

TEST_CASE("BuildMcpRuntimePlan:mcp.yaml 折 McpServerConfig(command/args/env/timeout)") {
    TempDir temp;
    const fs::path root = temp.Get() / "demo";
    WriteFile(root / "mcp" / "ledger" / "mcp.yaml", R"yaml(schema: 1
id: ledger
description: 记事。
transport: stdio
runtime:
  command: python
  args:
    - "${package_dir}/mcp/ledger/server.py"
    - "--quiet"
  env:
    BROWSER_TOKEN: "${env:BROWSER_TOKEN}"
    MISSING_ONE: "${env:NO_SUCH_VAR_HERE}"
  timeout_ms: 12000
permissions:
  network: false
)yaml");
    const auto parsed = ParseMcpComponentYaml(ReadFileText(root / "mcp" / "ledger" / "mcp.yaml"), root);
    REQUIRE(parsed.has_value());

    const McpExpansionContext ctx{root, temp.Get() / "data" / "demo"};
    const EnvLookup lookup = [](const std::string& name) -> std::optional<std::string> {
        if (name == "BROWSER_TOKEN") return std::string("tok");
        return std::nullopt;
    };
    std::vector<std::string> notes;
    const auto plan = BuildMcpRuntimePlan(*parsed, ctx, lookup, &notes);
    REQUIRE(plan.has_value());
    CHECK(plan->server.command == "python");
    REQUIRE(plan->server.args.size() == 2);
    CHECK(plan->server.args[0] == lubancode::platform::PathToUtf8(root) + "/mcp/ledger/server.py");
    CHECK(plan->server.args[1] == "--quiet");
    REQUIRE(plan->server.env.size() == 1);  // 缺的那对丢了
    CHECK(plan->server.env[0].first == "BROWSER_TOKEN");
    CHECK(plan->server.env[0].second == "tok");
    CHECK(plan->timeout_ms == 12000);
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].find("NO_SUCH_VAR_HERE") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 探针(真进程)
// ---------------------------------------------------------------------------
TEST_CASE("ProbeProcessPlugin:好件过协议,坏件带码指名") {
    TempDir temp;
    const fs::path good_dir = temp.Get() / "good";
    WriteFile(good_dir / "plugin.json", R"json({
  "manifest_version": 1,
  "id": "good",
  "version": "1.0.0",
  "language": "python",
  "runtime": {"kind": "process", "command": "PYCMD", "args": ["${plugin_dir}/runner.py"]},
  "tools": [{"name": "count", "description": "数词", "input_schema": {"type": "object"}}]
})json");
    WriteFile(good_dir / "runner.py",
              "import json, sys\n"
              "req = json.load(sys.stdin)\n"
              "json.dump({\"protocol\": 1, \"call_id\": req[\"call_id\"], \"ok\": True,\n"
              "           \"content\": [{\"type\": \"text\", \"text\": \"ok\"}]}, sys.stdout)\n");
    {
        std::string manifest_text = ReadFileText(good_dir / "plugin.json");
        const std::size_t hit = manifest_text.find("PYCMD");
        manifest_text.replace(hit, strlen("PYCMD"), kPythonCmd);
        WriteFile(good_dir / "plugin.json", manifest_text);
    }
    const auto good_manifest =
        lubancode::runtime::ParsePluginManifest(ReadFileText(good_dir / "plugin.json"), good_dir);
    REQUIRE(good_manifest.has_value());
    const PluginProbeReport good = ProbeProcessPlugin(*good_manifest, temp.Get().string());
    CHECK(good.ok);

    const fs::path bad_dir = temp.Get() / "bad";
    WriteFile(bad_dir / "plugin.json", R"json({
  "manifest_version": 1,
  "id": "bad",
  "version": "1.0.0",
  "language": "python",
  "runtime": {"kind": "process", "command": "PYCMD", "args": ["${plugin_dir}/runner.py"]},
  "tools": [{"name": "shout", "description": "坏件", "input_schema": {"type": "object"}}]
})json");
    {
        std::string manifest_text = ReadFileText(bad_dir / "plugin.json");
        const std::size_t hit = manifest_text.find("PYCMD");
        manifest_text.replace(hit, strlen("PYCMD"), kPythonCmd);
        WriteFile(bad_dir / "plugin.json", manifest_text);
    }
    WriteFile(bad_dir / "runner.py", "import sys\nsys.stderr.write(\"dying\\n\")\nsys.exit(3)\n");
    const auto bad_manifest =
        lubancode::runtime::ParsePluginManifest(ReadFileText(bad_dir / "plugin.json"), bad_dir);
    REQUIRE(bad_manifest.has_value());
    const PluginProbeReport bad = ProbeProcessPlugin(*bad_manifest, temp.Get().string());
    CHECK_FALSE(bad.ok);
    CHECK(bad.detail.find("tool_exit_non_zero") != std::string::npos);
    CHECK(bad.detail.find("3") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 三件套整包成败(阶段 5 验收线)
// ---------------------------------------------------------------------------
TEST_CASE("好包三件全进:两插件一 MCP,事务发布后 ToolRegistry 见 wire 名与来源账") {
    TempDir temp;
    const fs::path layer = temp.Get() / "dev-packages";
    fs::create_directories(layer);
    CopyFixturePackage(kFixturesRoot / "code-stack", layer, "code-stack");

    const PackageMount mount = MountTrustedDev(layer);
    REQUIRE(mount.entries.size() == 1);
    REQUIRE(mount.entries[0].code_trust == CodeTrustStatus::Trusted);

    PackageCodeMountOptions options;
    options.cwd_utf8 = temp.Get().string();
    options.package_data_root = temp.Get() / "package-data";
    PackageCodeMountResult result = MountPackageCode(mount, options);  // 发布段要移 client,非常量

    CHECK(result.attempted_packages == 1);
    REQUIRE(result.plugins.size() == 2);
    REQUIRE(result.mcp_servers.size() == 1);
    CHECK(result.diagnostics.empty());
    CHECK(result.plugins[0].canonical_id == "moontide.code-stack:count-words");
    CHECK(result.plugins[1].canonical_id == "moontide.code-stack:reverse-text");
    CHECK(result.mcp_servers[0].canonical_id == "moontide.code-stack:ledger");
    CHECK(result.mcp_servers[0].wire_server_name == "moontide%2Ecode-stack%2Eledger");
    CHECK(result.mcp_servers[0].display_server_name == "moontide.code-stack.ledger");
    REQUIRE(result.mcp_servers[0].client != nullptr);
    CHECK(result.mcp_servers[0].client->Alive());
    CHECK(result.mcp_servers[0].tools.size() == 2);  // ping + note

    // ---- 发布段:与 ToolRuntime 构造同一形状 ----
    std::vector<lubancode::app::McpServerRuntime> runtimes;
    for (auto& staged : result.mcp_servers) {
        // 就地 emplace 再填字段,不留"本地默认构造再 move 走"的形状——
        // GCC 13 -O3 对那形状报 maybe-uninitialized(误报),顺手绕开。
        lubancode::app::McpServerRuntime& runtime = runtimes.emplace_back();
        runtime.name = staged.wire_server_name;
        runtime.tools = std::move(staged.tools);
        runtime.package_origin = lubancode::tools::ToolOrigin{
            staged.package_id, staged.package_version, staged.canonical_id};
        runtime.client = std::move(staged.client);
    }
    lubancode::tools::ToolRegistry registry;
    lubancode::app::RegisterMcpTools(runtimes, registry);
    std::vector<lubancode::app::PluginMountInfo> mounted;
    lubancode::app::PublishPackagedPlugins(result, registry, mounted, /*report=*/true);

    // 三件全进正式表:wire 名可查,来源账在册。
    const std::string count_wire = lubancode::runtime::BuildPackagedToolWireName(
        "plugin", "moontide.code-stack", "count-words", "count");
    const std::string reverse_wire = lubancode::runtime::BuildPackagedToolWireName(
        "plugin", "moontide.code-stack", "reverse-text", "reverse");
    const std::string ping_wire =
        lubancode::runtime::BuildPackagedToolWireName("mcp", "moontide.code-stack", "ledger", "ping");
    CHECK(count_wire == "plugin__moontide%2Ecode-stack%2Ecount-words__count");
    REQUIRE(registry.Find(count_wire) != nullptr);
    REQUIRE(registry.Find(reverse_wire) != nullptr);
    REQUIRE(registry.Find(ping_wire) != nullptr);

    const auto* count_registration = registry.RegistrationOf(count_wire);
    REQUIRE(count_registration != nullptr);
    REQUIRE(count_registration->package_origin.has_value());
    CHECK(count_registration->package_origin->package_id == "moontide.code-stack");
    CHECK(count_registration->package_origin->package_version == "0.1.0");
    CHECK(count_registration->package_origin->component_id == "moontide.code-stack:count-words");
    const auto* ping_registration = registry.RegistrationOf(ping_wire);
    REQUIRE(ping_registration != nullptr);
    REQUIRE(ping_registration->package_origin.has_value());
    CHECK(ping_registration->package_origin->component_id == "moontide.code-stack:ledger");
    CHECK(ping_registration->source_kind == lubancode::tools::ToolSourceKind::Mcp);

    // /plugins 的账:展示名带点(canonical 段),kind 记 package-process。
    REQUIRE(mounted.size() == 2);
    CHECK(mounted[0].tool_name == "plugin__moontide.code-stack.count-words__count");
    CHECK(mounted[0].kind == "package-process");
    REQUIRE(mounted[0].package_origin.has_value());
    CHECK(mounted[0].package_origin->package_version == "0.1.0");

    // 真跑一件:wire 覆盖名底下,协议帧仍走 manifest 本地名。
    const auto counted = registry.Find(count_wire)->execute({{"text", "alpha beta gamma"}});
    CHECK_FALSE(counted.is_error);
    CHECK(counted.content == "3");
    const auto pong = registry.Find(ping_wire)->execute(nlohmann::json::object());
    CHECK_FALSE(pong.is_error);
    CHECK(pong.content.find("pong") != std::string::npos);
}

TEST_CASE("坏一件整包回滚:三件全不进,诊断指到坏件,零进程残留") {
    TempDir temp;
    const fs::path layer = temp.Get() / "dev-packages";
    fs::create_directories(layer);
    CopyFixturePackage(kFixturesRoot / "broken" / "code-failure", layer, "code-failure");

    const PackageMount mount = MountTrustedDev(layer);
    REQUIRE(mount.entries.size() == 1);
    REQUIRE(mount.entries[0].code_trust == CodeTrustStatus::Trusted);  // 静态全好,批得过

    PackageCodeMountOptions options;
    options.cwd_utf8 = temp.Get().string();
    options.package_data_root = temp.Get() / "package-data";
    const PackageCodeMountResult result = MountPackageCode(mount, options);

    CHECK(result.attempted_packages == 1);
    CHECK(result.plugins.empty());       // 三件全不进
    CHECK(result.mcp_servers.empty());
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].package_id == "moontide.code-failure");
    CHECK(result.diagnostics[0].component_id == "moontide.code-failure:dies-loud");
    CHECK(result.diagnostics[0].kind_text == "plugin");
    CHECK(result.diagnostics[0].message.find("tool_exit_non_zero") != std::string::npos);
    CHECK(result.diagnostics[0].message.find("3") != std::string::npos);
    const std::string line = result.diagnostics[0].Format();
    CHECK(line.find("moontide.code-failure:dies-loud") != std::string::npos);
    CHECK(line.find("整包回滚") != std::string::npos);
    CHECK(line.find("/package doctor moontide.code-failure") != std::string::npos);

    // 发布段没东西可发:正式表一件不进。
    lubancode::tools::ToolRegistry registry;
    std::vector<lubancode::app::PluginMountInfo> mounted;
    lubancode::app::PublishPackagedPlugins(result, registry, mounted, /*report=*/true);
    CHECK(registry.Find(lubancode::runtime::BuildPackagedToolWireName(
               "plugin", "moontide.code-failure", "count-words", "count")) == nullptr);
    CHECK(mounted.empty());
}

TEST_CASE("MCP 居中失败:已起的进程全停(回滚杀进程),一件不发布") {
    TempDir temp;
    const fs::path layer = temp.Get() / "dev-packages";
    const fs::path root = layer / "alpha-beta";
    const std::string pid_file = GenericUtf8(temp.Get() / "alpha.pid");
    WriteFile(root / "package.yaml",
              "schema: 1\nid: test.alpha-beta\nversion: 0.1.0\nname: AB\ndescription: 双 MCP 测试件。\n");
    // alpha(好):起服握手过,启动时把 PID 写到 args 指的文件里。
    WriteFile(root / "mcp" / "alpha" / "mcp.yaml",
              "schema: 1\nid: alpha\ndescription: 好。\ntransport: stdio\nruntime:\n  command: " +
                  std::string(kPythonCmd) +
                  "\n  args:\n    - \"${package_dir}/mcp/alpha/server.py\"\n    - \"" + pid_file +
                  "\"\npermissions:\n  network: false\n");
    WriteFile(root / "mcp" / "alpha" / "server.py",
              "import json, os, sys\n"
              "open(sys.argv[1], \"w\").write(str(os.getpid()))\n"
              "for raw in sys.stdin:\n"
              "    msg = json.loads(raw)\n"
              "    method = msg.get(\"method\", \"\")\n"
              "    if method == \"initialize\":\n"
              "        out = {\"protocolVersion\": \"2024-11-05\", \"capabilities\": {\"tools\": {}},\n"
              "               \"serverInfo\": {\"name\": \"alpha\", \"version\": \"0\"}}\n"
              "        sys.stdout.write(json.dumps({\"jsonrpc\": \"2.0\", \"id\": msg[\"id\"], \"result\": out}) + \"\\n\")\n"
              "        sys.stdout.flush()\n"
              "    elif method == \"tools/list\":\n"
              "        out = {\"tools\": [{\"name\": \"ping\", \"description\": \"p\", \"inputSchema\": {\"type\": \"object\"}}]}\n"
              "        sys.stdout.write(json.dumps({\"jsonrpc\": \"2.0\", \"id\": msg[\"id\"], \"result\": out}) + \"\\n\")\n"
              "        sys.stdout.flush()\n");
    // beta(坏):命令不存在,StartProcess 直接失败。
    WriteFile(root / "mcp" / "beta" / "mcp.yaml",
              "schema: 1\nid: beta\ndescription: 坏。\ntransport: stdio\nruntime:\n  command: "
              "lubancode-no-such-command-xyz\npermissions:\n  network: false\n");

    const PackageMount mount = MountTrustedDev(layer);
    REQUIRE(mount.entries.size() == 1);

    PackageCodeMountOptions options;
    options.cwd_utf8 = temp.Get().string();
    const PackageCodeMountResult result = MountPackageCode(mount, options);

    CHECK(result.attempted_packages == 1);
    CHECK(result.mcp_servers.empty());
    CHECK(result.plugins.empty());
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].component_id == "test.alpha-beta:beta");
    CHECK(result.diagnostics[0].kind_text == "mcp_server");
    CHECK(result.diagnostics[0].message.find("起服失败") != std::string::npos);

    // 零残留:alpha 起过又回滚,进程必须死透。
    REQUIRE(fs::exists(temp.Get() / "alpha.pid"));
    const unsigned long pid = std::strtoul(ReadFileText(temp.Get() / "alpha.pid").c_str(), nullptr, 10);
    REQUIRE(pid != 0);
    CHECK(WaitFor([&] { return !lubancode::platform::IsProcessAlive(pid); }, 10000));
}

TEST_CASE("第一只插件就坏:同样整包回滚,一件不进") {
    TempDir temp;
    const fs::path layer = temp.Get() / "dev-packages";
    const fs::path root = layer / "first-dies";
    WriteFile(root / "package.yaml",
              "schema: 1\nid: test.first-dies\nversion: 0.1.0\nname: FD\ndescription: 首件坏测试件。\n");
    WriteFile(root / "plugins" / "aaa-dies" / "plugin.json", R"json({
  "manifest_version": 1,
  "id": "aaa-dies",
  "version": "1.0.0",
  "language": "python",
  "runtime": {"kind": "process", "command": "PYCMD", "args": ["${plugin_dir}/runner.py"]},
  "tools": [{"name": "shout", "description": "坏", "input_schema": {"type": "object"}}]
})json");
    WriteFile(root / "plugins" / "aaa-dies" / "runner.py", "import sys\nsys.exit(9)\n");
    WriteFile(root / "plugins" / "zzz-good" / "plugin.json", R"json({
  "manifest_version": 1,
  "id": "zzz-good",
  "version": "1.0.0",
  "language": "python",
  "runtime": {"kind": "process", "command": "PYCMD", "args": ["${plugin_dir}/runner.py"]},
  "tools": [{"name": "noop", "description": "好", "input_schema": {"type": "object"}}]
})json");
    WriteFile(root / "plugins" / "zzz-good" / "runner.py",
              "import json, sys\n"
              "req = json.load(sys.stdin)\n"
              "json.dump({\"protocol\": 1, \"call_id\": req[\"call_id\"], \"ok\": True,\n"
              "           \"content\": [{\"type\": \"text\", \"text\": \"ok\"}]}, sys.stdout)\n");
    for (const char* dir : {"plugins/aaa-dies", "plugins/zzz-good"}) {
        std::string text = ReadFileText(root / dir / "plugin.json");
        const std::size_t hit = text.find("PYCMD");
        text.replace(hit, strlen("PYCMD"), kPythonCmd);
        WriteFile(root / dir / "plugin.json", text);
    }

    const PackageMount mount = MountTrustedDev(layer);
    REQUIRE(mount.entries.size() == 1);
    PackageCodeMountOptions options;
    options.cwd_utf8 = temp.Get().string();
    const PackageCodeMountResult result = MountPackageCode(mount, options);

    CHECK(result.attempted_packages == 1);
    CHECK(result.plugins.empty());
    CHECK(result.mcp_servers.empty());
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].component_id == "test.first-dies:aaa-dies");
}

// ---------------------------------------------------------------------------
// 信任门联动与 store 同路
// ---------------------------------------------------------------------------
TEST_CASE("未批的包压根不进事务(code 件连暂存都不进)") {
    TempDir temp;
    const fs::path layer = temp.Get() / "dev-packages";
    fs::create_directories(layer);
    CopyFixturePackage(kFixturesRoot / "code-stack", layer, "code-stack");

    PackageMountInput input;  // 不喂信任账:dev 层一律待信任
    input.scan.dev_roots.push_back(layer);
    const PackageMount mount = BuildPackageMount(input);
    REQUIRE(mount.entries.size() == 1);
    CHECK(mount.entries[0].code_trust == CodeTrustStatus::PendingTrust);

    PackageCodeMountOptions options;
    options.cwd_utf8 = temp.Get().string();
    const PackageCodeMountResult result = MountPackageCode(mount, options);
    CHECK(result.attempted_packages == 0);
    CHECK(result.empty());
}

TEST_CASE("store 选中版本同路:scope=Store 的候选照走同一事务") {
    TempDir temp;
    const fs::path layer = temp.Get() / "store-in";
    fs::create_directories(layer);
    CopyFixturePackage(kFixturesRoot / "code-stack", layer, "code-stack");

    // 折一只 store 候选(进化侧哈希验完好才递——这里直接给完好拷贝)。
    PackageCandidate candidate;
    candidate.scope = PackageScope::Store;
    candidate.layer_root = layer;
    candidate.package_root = layer / "code-stack";
    candidate.dir_name = "code-stack";
    {
        const auto manifest_text = ReadFileText(layer / "code-stack" / "package.yaml");
        auto parsed = ParsePackageManifest(manifest_text);
        REQUIRE(parsed.has_value());
        candidate.manifest = std::move(*parsed);
    }
    PackageMountInput input;
    input.store_candidates.push_back(candidate);
    const PackageMount mount = BuildPackageMount(input);
    REQUIRE(mount.entries.size() == 1);
    CHECK(mount.entries[0].scope == PackageScope::Store);
    CHECK(mount.entries[0].code_trust == CodeTrustStatus::Trusted);  // store 免门(进化侧已验)

    PackageCodeMountOptions options;
    options.cwd_utf8 = temp.Get().string();
    options.package_data_root = temp.Get() / "package-data";
    const PackageCodeMountResult result = MountPackageCode(mount, options);
    CHECK(result.attempted_packages == 1);
    CHECK(result.plugins.size() == 2);
    CHECK(result.mcp_servers.size() == 1);
    CHECK(result.diagnostics.empty());
}

// ---------------------------------------------------------------------------
// 第四类 code 组件:embedded-lua(Lua 受控 HTTP 单·阶段 4,设计单 §13.5)
// ---------------------------------------------------------------------------

namespace {

// 一只 v2 embedded-lua 插件目录(纯计算 handler,不碰网——事务与 wire 名
// 的账在这里,受控 HTTP 的行为账在 test_plugin_lua_manifest.cpp)。
void WriteLuaPlugin(const fs::path& plugin_dir, const std::string& lua_script,
                    const std::string& extra_permissions = std::string()) {
    const std::string manifest_text = R"json({
  "manifest_version": 2,
  "id": "lua-tools",
  "version": "0.1.0",
  "language": "lua",
  "runtime": {"kind": "embedded-lua", "entry": "demo.lua"},
  "permissions": {)json" + extra_permissions + R"json(
    "secrets": []
  },
  "tools": [
    {
      "name": "echo",
      "entry": "echo",
      "description": "Echo the text back.",
      "input_schema": {
        "type": "object",
        "properties": {"text": {"type": "string"}},
        "required": ["text"],
        "additionalProperties": false
      }
    }
  ]
})json";
    WriteFile(plugin_dir / "plugin.json", manifest_text);
    WriteFile(plugin_dir / "demo.lua", lua_script);
}

}  // namespace

TEST_CASE("Package Lua 第四类:整包挂载,owner 接管,wire 名注册,真跑一件") {
    TempDir temp;
    const fs::path layer = temp.Get() / "dev-packages";
    const fs::path root = layer / "lua-only";
    WriteFile(root / "package.yaml",
              "schema: 1\nid: test.lua-only\nversion: 0.1.0\nname: LO\ndescription: Lua 单件测试包。\n");
    WriteLuaPlugin(root / "plugins" / "lua-tools",
                   "return {\n  echo = function(input) return \"echo:\" .. tostring(input.text or \"\") end,\n}\n");

    const PackageMount mount = MountTrustedDev(layer);
    REQUIRE(mount.entries.size() == 1);
    REQUIRE(mount.entries[0].code_trust == CodeTrustStatus::Trusted);

    PackageCodeMountOptions options;
    options.cwd_utf8 = temp.Get().string();
    options.package_data_root = temp.Get() / "package-data";
    PackageCodeMountResult result = MountPackageCode(mount, options);

    CHECK(result.attempted_packages == 1);
    REQUIRE(result.plugins.size() == 1);
    CHECK(result.diagnostics.empty());
    CHECK(result.plugins[0].canonical_id == "test.lua-only:lua-tools");
    REQUIRE(result.plugins[0].lua != nullptr);  // 第四类成品:暂存 Lua state
    CHECK(result.plugins[0].lua->state != nullptr);
    CHECK(result.plugins[0].lua->package_id == "test.lua-only");
    CHECK(result.plugins[0].lua->local_id == "lua-tools");
    CHECK(result.plugins[0].manifest == result.plugins[0].lua->manifest);

    // 发布段:与 ToolRuntime 构造同一形状——owner 接管,wire 名 + 来源账。
    lubancode::runtime::ManifestLuaRuntime lua_runtime;
    std::vector<lubancode::runtime::ManifestLuaPlugin*> adopted;
    for (auto& staged : result.plugins) {
        adopted.push_back(lua_runtime.Adopt(std::move(staged.lua)));
    }
    lubancode::tools::ToolRegistry registry;
    std::vector<lubancode::app::PluginMountInfo> mounted;
    lubancode::app::PublishPackagedLuaPlugins(adopted, registry, mounted, /*report=*/true);

    const std::string echo_wire = lubancode::runtime::BuildPackagedToolWireName(
        "plugin", "test.lua-only", "lua-tools", "echo");
    CHECK(echo_wire == "plugin__test%2Elua-only%2Elua-tools__echo");
    REQUIRE(registry.Find(echo_wire) != nullptr);
    const auto* registration = registry.RegistrationOf(echo_wire);
    REQUIRE(registration != nullptr);
    REQUIRE(registration->package_origin.has_value());
    CHECK(registration->package_origin->package_id == "test.lua-only");
    CHECK(registration->package_origin->component_id == "test.lua-only:lua-tools");
    CHECK(registration->source_kind == lubancode::tools::ToolSourceKind::PluginLua);

    // /plugins 的账:展示名带点,kind 记 package-embedded-lua。
    REQUIRE(mounted.size() == 1);
    CHECK(mounted[0].tool_name == "plugin__test.lua-only.lua-tools__echo");
    CHECK(mounted[0].kind == "package-embedded-lua");

    // 真跑一件:manifest schema 门 + 动态作用域调用,全程零进程零网络。
    const auto echoed = registry.Find(echo_wire)->execute({{"text", "hi"}});
    CHECK_FALSE(echoed.is_error);
    CHECK(echoed.content == "echo:hi");
}

TEST_CASE("Package Lua 同包坏件:整包回滚,Lua state 一并关,一件不发布") {
    TempDir temp;
    const fs::path layer = temp.Get() / "dev-packages";
    const fs::path root = layer / "lua-rollback";
    WriteFile(root / "package.yaml",
              "schema: 1\nid: test.lua-rollback\nversion: 0.1.0\nname: LR\ndescription: 整包回滚测试件。\n");
    // 好的 Lua 件排在前(component 序:plugins 在 mcp 前),先暂存;
    // 坏 MCP 排在后:命令不存在,StartProcess 失败 -> 整包回滚。
    WriteLuaPlugin(root / "plugins" / "lua-tools",
                   "return {\n  echo = function(input) return \"echo:\" .. tostring(input.text or \"\") end,\n}\n");
    WriteFile(root / "mcp" / "bad-mcp" / "mcp.yaml",
              "schema: 1\nid: bad-mcp\ndescription: 坏。\ntransport: stdio\nruntime:\n  command: "
              "lubancode-no-such-command-xyz\npermissions:\n  network: false\n");

    const PackageMount mount = MountTrustedDev(layer);
    REQUIRE(mount.entries.size() == 1);
    REQUIRE(mount.entries[0].code_trust == CodeTrustStatus::Trusted);

    PackageCodeMountOptions options;
    options.cwd_utf8 = temp.Get().string();
    const PackageCodeMountResult result = MountPackageCode(mount, options);

    CHECK(result.attempted_packages == 1);
    CHECK(result.plugins.empty());  // 已暂存的 Lua state 随回滚弃置,一件不发布
    CHECK(result.mcp_servers.empty());
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].component_id == "test.lua-rollback:bad-mcp");
    CHECK(result.diagnostics[0].kind_text == "mcp_server");

    // 发布段没东西可发:正式表一件不进。
    lubancode::runtime::ManifestLuaRuntime lua_runtime;
    std::vector<lubancode::runtime::ManifestLuaPlugin*> adopted;  // 空:暂存件全数回滚
    lubancode::tools::ToolRegistry registry;
    std::vector<lubancode::app::PluginMountInfo> mounted;
    lubancode::app::PublishPackagedLuaPlugins(adopted, registry, mounted, /*report=*/true);
    CHECK(registry.Find(lubancode::runtime::BuildPackagedToolWireName(
               "plugin", "test.lua-rollback", "lua-tools", "echo")) == nullptr);
    CHECK(mounted.empty());
}

TEST_CASE("未信任项目 Package:Lua chunk 一字不跑(顶层 error 无声无息)") {
    TempDir temp;
    const fs::path layer = temp.Get() / "dev-packages";
    const fs::path root = layer / "lua-untrusted";
    WriteFile(root / "package.yaml",
              "schema: 1\nid: test.lua-untrusted\nversion: 0.1.0\nname: LU\ndescription: 未信任测试件。\n");
    // 顶层 error():chunk 若跑过哪怕一行,加载即失败、诊断即点名——这是
    // "一字不跑"的可观察标记(真跑过则 diagnostics 非空)。
    WriteLuaPlugin(root / "plugins" / "lua-tools",
                   "error(\"TOPLEVEL_SHOULD_NOT_RUN\")\nreturn { echo = function(input) return \"ok\" end }\n");

    PackageMountInput input;  // 不喂信任账:一律待信任
    input.scan.dev_roots.push_back(layer);
    const PackageMount mount = BuildPackageMount(input);
    REQUIRE(mount.entries.size() == 1);
    CHECK(mount.entries[0].code_trust == CodeTrustStatus::PendingTrust);

    PackageCodeMountOptions options;
    options.cwd_utf8 = temp.Get().string();
    const PackageCodeMountResult result = MountPackageCode(mount, options);
    CHECK(result.attempted_packages == 0);  // 连事务都不进,更别提建 state
    CHECK(result.empty());
    CHECK(result.diagnostics.empty());  // 若 chunk 跑过,这里会有 TOPLEVEL 诊断
}

TEST_CASE("manifest 权限变更后旧信任失效:network 一改,code_trust 回待信任") {
    TempDir temp;
    const fs::path layer = temp.Get() / "dev-packages";
    const fs::path root = layer / "lua-permission-change";
    WriteFile(root / "package.yaml",
              "schema: 1\nid: test.lua-perm\nversion: 0.1.0\nname: LP\ndescription: 权限变更测试件。\n");
    const fs::path plugin_dir = root / "plugins" / "lua-tools";
    const std::string good_script = "return { echo = function(input) return \"ok\" end }\n";
    // 第一版:不声明网络。
    WriteLuaPlugin(plugin_dir, good_script);

    // 第一次扫,按当时的哈希落一枚信任。
    PackageMountInput pending_input;
    pending_input.scan.dev_roots.push_back(layer);
    const PackageMount pending = BuildPackageMount(pending_input);
    REQUIRE(pending.entries.size() == 1);
    PackageTrustSnapshot snapshot;
    snapshot.keys.insert(pending.entries[0].package_id + "\n" + pending.entries[0].content_hash);
    {
        PackageMountInput trusted_input;
        trusted_input.scan.dev_roots.push_back(layer);
        trusted_input.trust = snapshot;
        const PackageMount trusted = BuildPackageMount(trusted_input);
        REQUIRE(trusted.entries[0].code_trust == CodeTrustStatus::Trusted);
    }

    // 改 manifest:多声明一条 network 权限(§10.1:权限一变,manifest 哈希
    // 变,旧信任立即失效)。
    WriteLuaPlugin(plugin_dir, good_script,
                   "\n    \"network\": [{\"scheme\": \"https\", \"host\": \"api.example.com\", "
                   "\"port\": 443, \"methods\": [\"GET\"]}],");

    PackageMountInput stale_input;
    stale_input.scan.dev_roots.push_back(layer);
    stale_input.trust = snapshot;  // 旧账
    const PackageMount remounted = BuildPackageMount(stale_input);
    REQUIRE(remounted.entries.size() == 1);
    CHECK(remounted.entries[0].code_trust == CodeTrustStatus::PendingTrust);

    PackageCodeMountOptions options;
    options.cwd_utf8 = temp.Get().string();
    const PackageCodeMountResult result = MountPackageCode(remounted, options);
    CHECK(result.attempted_packages == 0);
    CHECK(result.empty());
}
