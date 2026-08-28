// plugins 运行时第 1 步(冻结合同)的单测:manifest 强校验、标识符规矩、
// ${plugin_dir} 结构化替换与路径圈禁、进程协议帧、入参 Schema 子集。
//
// 章法:全部纯函数直测,不起进程;process 链的真机测试在
// test_plugin_process.cpp。坏 manifest 一律整件拒绝,不悄悄宽化——这是
// 与 DLL 老路(退化宽 object)刻意相反的合同,单测钉死。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "runtime/plugin_contract.hpp"
#include "runtime/plugin_tool.hpp"

using namespace lubancode;
using namespace lubancode::runtime;

namespace {

// 一份合法 process manifest 的样板,单测各自掰弯一处。
const char* kGoodManifest = R"json({
  "manifest_version": 1,
  "id": "local-math",
  "version": "1.0.0",
  "language": "python",
  "runtime": {
    "kind": "process",
    "command": "python3",
    "args": ["${plugin_dir}/add.py"],
    "timeout_ms": 30000
  },
  "tools": [
    {
      "name": "add",
      "description": "Add two numbers.",
      "entry": "add",
      "input_schema": {
        "type": "object",
        "properties": {
          "a": {"type": "number"},
          "b": {"type": "number"}
        },
        "required": ["a", "b"],
        "additionalProperties": false
      }
    }
  ],
  "permissions": {"cwd": "project", "network": false, "env": []}
})json";

// 临时插件目录(每个 TEST_CASE 各建各的,收尾自删;MSVC/Windows 的文件柄
// 规矩:先关流再删,remove_all 用 error_code 形态)。
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        std::error_code ec;
        path = std::filesystem::temp_directory_path() /
              ("lubancode_plugin_test_" + std::to_string(++counter_));
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

  private:
    static int counter_;
};
int TempDir::counter_ = 0;

}  // namespace

// ---------------------------------------------------------------------------
// /plugin test 的自测入口发现(P3-1):纯函数,不起进程;真跑的回执在
// integration/plugins/test_plugin_trust_ui.cpp 那册对命令层验。
// ---------------------------------------------------------------------------

TEST_CASE("ResolvePluginSelfTest: 插件目录里有 test_runner.py 就按 manifest 解释器组计划") {
    TempDir dir;
    std::ofstream(dir.path / "test_runner.py", std::ios::binary) << "# self test\n";
    auto manifest = ParsePluginManifest(kGoodManifest, dir.path);
    REQUIRE(manifest.has_value());
    const auto plan = ResolvePluginSelfTest(*manifest);
    REQUIRE(plan.has_value());
    REQUIRE(plan->argv.size() == 2);
    CHECK(plan->argv[0] == "python3");  // manifest.runtime.command 原样
    CHECK(plan->argv[1].find("test_runner.py") != std::string::npos);
    CHECK(plan->timeout_ms == 30000);   // manifest.timeout_ms 原样带出
}

TEST_CASE("ResolvePluginSelfTest: .py/.js/.mjs/.cjs 按序认,一个不落") {
    TempDir dir;
    auto manifest = ParsePluginManifest(kGoodManifest, dir.path);
    REQUIRE(manifest.has_value());
    // 都没有:nullopt。
    CHECK_FALSE(ResolvePluginSelfTest(*manifest).has_value());
    // 只有 .js:也认(Node 插件的形状)。
    std::ofstream(dir.path / "test_runner.js", std::ios::binary) << "// t\n";
    const auto js_plan = ResolvePluginSelfTest(*manifest);
    REQUIRE(js_plan.has_value());
    CHECK(js_plan->argv[1].find("test_runner.js") != std::string::npos);
    // .py 与 .js 并存:py 在前(scaffold 的形状优先)。
    std::ofstream(dir.path / "test_runner.py", std::ios::binary) << "# t\n";
    const auto py_plan = ResolvePluginSelfTest(*manifest);
    REQUIRE(py_plan.has_value());
    CHECK(py_plan->argv[1].find("test_runner.py") != std::string::npos);
    // .mjs 单独在(先删掉 .py/.js):也认。
    std::error_code ec;
    std::filesystem::remove(dir.path / "test_runner.py", ec);
    std::filesystem::remove(dir.path / "test_runner.js", ec);
    std::ofstream(dir.path / "test_runner.mjs", std::ios::binary) << "// t\n";
    const auto mjs_plan = ResolvePluginSelfTest(*manifest);
    REQUIRE(mjs_plan.has_value());
    CHECK(mjs_plan->argv[1].find("test_runner.mjs") != std::string::npos);
}

TEST_CASE("ResolvePluginSelfTest: runner.py(不带 test_)不算自测入口") {
    TempDir dir;
    std::ofstream(dir.path / "runner.py", std::ios::binary) << "# tool runner\n";
    auto manifest = ParsePluginManifest(kGoodManifest, dir.path);
    REQUIRE(manifest.has_value());
    CHECK_FALSE(ResolvePluginSelfTest(*manifest).has_value());
}

TEST_CASE("ResolvePluginSelfTest: embedded-lua 插件一律无自测入口") {
    TempDir dir;
    std::ofstream(dir.path / "test_runner.py", std::ios::binary) << "# t\n";
    const std::string lua_manifest = R"json({
  "manifest_version": 1, "id": "lua-probe", "version": "1.0.0",
  "runtime": {"kind": "embedded-lua"},
  "tools": [{"name": "echo", "description": "d", "input_schema": {"type": "object"}}]
})json";
    auto manifest = ParsePluginManifest(lua_manifest, dir.path);
    REQUIRE(manifest.has_value());
    CHECK(manifest->kind == RuntimeKind::EmbeddedLua);
    CHECK_FALSE(ResolvePluginSelfTest(*manifest).has_value());
}

// ---------------------------------------------------------------------------
// 标识符与工具名
// ---------------------------------------------------------------------------

TEST_CASE("标识符规矩:字母数字_-、字母数字开头、长度上限") {
    CHECK(IsValidPluginIdentifier("local-math", 64));
    CHECK(IsValidPluginIdentifier("a", 64));
    CHECK(IsValidPluginIdentifier("A1_b-2", 64));
    CHECK_FALSE(IsValidPluginIdentifier("", 64));          // 空
    CHECK_FALSE(IsValidPluginIdentifier("-lead", 64));     // 标志符开头
    CHECK_FALSE(IsValidPluginIdentifier("_lead", 64));
    CHECK_FALSE(IsValidPluginIdentifier("has space", 64)); // 空格
    CHECK_FALSE(IsValidPluginIdentifier("中文", 64));       // 非 ASCII
    CHECK_FALSE(IsValidPluginIdentifier("a;b", 64));       // shell 元字符
    CHECK_FALSE(IsValidPluginIdentifier("a&b", 64));
    CHECK_FALSE(IsValidPluginIdentifier("a|b", 64));
    CHECK_FALSE(IsValidPluginIdentifier(std::string(65, 'a'), 64));  // 超长
    CHECK(IsValidPluginIdentifier(std::string(64, 'a'), 64));        // 恰好上限
}

TEST_CASE("工具完整名 plugin__<id>__<tool>") {
    CHECK(BuildPluginToolName("local-math", "add") == "plugin__local-math__add");
    CHECK(BuildPluginToolName("a", "b") == "plugin__a__b");
}

// ---------------------------------------------------------------------------
// manifest 强校验
// ---------------------------------------------------------------------------

TEST_CASE("合法 process manifest 全字段解析") {
    TempDir dir;
    auto manifest = ParsePluginManifest(kGoodManifest, dir.path);
    REQUIRE(manifest.has_value());
    CHECK(manifest->id == "local-math");
    CHECK(manifest->version == "1.0.0");
    CHECK(manifest->language == "python");
    CHECK(manifest->kind == RuntimeKind::Process);
    REQUIRE(manifest->argv.size() == 2);
    CHECK(manifest->argv[0] == "python3");
    // ${plugin_dir} 已替换成 canonical 目录的 UTF-8 写法。逐字节比对在
    // Windows 上不稳:临时目录的短名/长名(RUNNER~1)与分隔符方向都会岔,
    // 拿 filesystem::equivalent 比目录、另比文件名,语义就是"替换结果指进
    // 插件目录里的 add.py"。
    {
        const std::filesystem::path got = platform::Utf8ToPath(manifest->argv[1]);
        std::error_code eq_ec;
        CHECK(std::filesystem::equivalent(got.parent_path(), dir.path, eq_ec));
        CHECK_FALSE(eq_ec);
        CHECK(got.filename() == "add.py");
    }
    CHECK(manifest->timeout_ms == 30000);
    REQUIRE(manifest->tools.size() == 1);
    CHECK(manifest->tools[0].name == "add");
    CHECK(manifest->tools[0].full_name == "plugin__local-math__add");
    CHECK(manifest->tools[0].entry == "add");
    CHECK(manifest->tools[0].input_schema["required"] == nlohmann::json::array({"a", "b"}));
}

TEST_CASE("坏 JSON / 坏形状 / 版本不合各拒其名") {
    TempDir dir;
    // 不是 JSON
    auto r1 = ParsePluginManifest("{oops", dir.path);
    REQUIRE_FALSE(r1.has_value());
    CHECK(r1.error().find("合法 JSON") != std::string::npos);

    // 顶层不是 object
    auto r2 = ParsePluginManifest("[1,2]", dir.path);
    REQUIRE_FALSE(r2.has_value());

    // manifest_version 不合
    auto r3 = ParsePluginManifest(
        R"({"manifest_version":2,"id":"a","version":"1","runtime":{"kind":"process","command":"x"},"tools":[]})",
        dir.path);
    REQUIRE_FALSE(r3.has_value());
    CHECK(r3.error().find("manifest_version") != std::string::npos);

    // 缺 id
    auto r4 = ParsePluginManifest(
        R"({"manifest_version":1,"version":"1","runtime":{"kind":"process","command":"x"},"tools":[]})", dir.path);
    REQUIRE_FALSE(r4.has_value());
    CHECK(r4.error().find("id") != std::string::npos);

    // id 字符集坏(分号,shell 元字符)
    auto r5 = ParsePluginManifest(
        R"({"manifest_version":1,"id":"a;rm","version":"1","runtime":{"kind":"process","command":"x"},"tools":[]})",
        dir.path);
    REQUIRE_FALSE(r5.has_value());

    // runtime.kind 不认得
    auto r6 = ParsePluginManifest(
        R"({"manifest_version":1,"id":"a","version":"1","runtime":{"kind":"wasm","command":"x"},"tools":[]})",
        dir.path);
    REQUIRE_FALSE(r6.has_value());
    CHECK(r6.error().find("runtime.kind") != std::string::npos);

    // native-library 属后续批次,明拒不宽化
    auto r7 = ParsePluginManifest(
        R"({"manifest_version":1,"id":"a","version":"1","runtime":{"kind":"native-library","command":"x"},"tools":[]})",
        dir.path);
    REQUIRE_FALSE(r7.has_value());
    CHECK(r7.error().find("native-library") != std::string::npos);

    // tools 空
    auto r8 = ParsePluginManifest(
        R"({"manifest_version":1,"id":"a","version":"1","runtime":{"kind":"process","command":"x"},"tools":[]})",
        dir.path);
    REQUIRE_FALSE(r8.has_value());
    CHECK(r8.error().find("tools") != std::string::npos);
}

TEST_CASE("Schema 坏了拒绝整件,不悄悄宽化成宽 object") {
    TempDir dir;
    // input_schema 缺失
    const std::string no_schema = R"json({
      "manifest_version": 1, "id": "a", "version": "1",
      "runtime": {"kind": "process", "command": "x"},
      "tools": [{"name": "t", "description": "d"}]
    })json";
    auto r1 = ParsePluginManifest(no_schema, dir.path);
    REQUIRE_FALSE(r1.has_value());
    CHECK(r1.error().find("input_schema") != std::string::npos);
    CHECK(r1.error().find("宽化") != std::string::npos);

    // input_schema 不是 object(比如写成了 JSON 字符串)
    const std::string string_schema = R"json({
      "manifest_version": 1, "id": "a", "version": "1",
      "runtime": {"kind": "process", "command": "x"},
      "tools": [{"name": "t", "input_schema": "{\"type\":\"object\"}"}]
    })json";
    auto r2 = ParsePluginManifest(string_schema, dir.path);
    REQUIRE_FALSE(r2.has_value());

    // 声明怪:type 联合数组(v1 不认)
    const std::string union_type = R"json({
      "manifest_version": 1, "id": "a", "version": "1",
      "runtime": {"kind": "process", "command": "x"},
      "tools": [{"name": "t", "input_schema": {"type": ["string", "null"]}}]
    })json";
    auto r3 = ParsePluginManifest(union_type, dir.path);
    REQUIRE_FALSE(r3.has_value());
    CHECK(r3.error().find("联合类型") != std::string::npos);

    // required 不是数组
    const std::string bad_required = R"json({
      "manifest_version": 1, "id": "a", "version": "1",
      "runtime": {"kind": "process", "command": "x"},
      "tools": [{"name": "t", "input_schema": {"type": "object", "required": "a"}}]
    })json";
    auto r4 = ParsePluginManifest(bad_required, dir.path);
    REQUIRE_FALSE(r4.has_value());

    // minLength 不是非负整数
    const std::string bad_length = R"json({
      "manifest_version": 1, "id": "a", "version": "1",
      "runtime": {"kind": "process", "command": "x"},
      "tools": [{"name": "t", "input_schema": {"type": "string", "minLength": -1}}]
    })json";
    auto r5 = ParsePluginManifest(bad_length, dir.path);
    REQUIRE_FALSE(r5.has_value());
}

TEST_CASE("同一插件内工具重名在解析期即拒") {
    TempDir dir;
    const std::string dup = R"json({
      "manifest_version": 1, "id": "a", "version": "1",
      "runtime": {"kind": "process", "command": "x"},
      "tools": [
        {"name": "t", "input_schema": {"type": "object"}},
        {"name": "t", "input_schema": {"type": "object"}}
      ]
    })json";
    auto r = ParsePluginManifest(dup, dir.path);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().find("重名") != std::string::npos);
}

TEST_CASE("entry 缺省取工具短名") {
    TempDir dir;
    const std::string no_entry = R"json({
      "manifest_version": 1, "id": "a", "version": "1",
      "runtime": {"kind": "process", "command": "x"},
      "tools": [{"name": "greet", "input_schema": {"type": "object"}}]
    })json";
    auto r = ParsePluginManifest(no_entry, dir.path);
    REQUIRE(r.has_value());
    REQUIRE(r->tools.size() == 1);
    CHECK(r->tools[0].entry == "greet");
}

// ---------------------------------------------------------------------------
// ${plugin_dir} 替换与路径圈禁
// ---------------------------------------------------------------------------

TEST_CASE("${plugin_dir} 结构化替换:多段、前后缀、无占位符") {
    TempDir dir;
    auto r1 = ExpandPluginDirPlaceholder("${plugin_dir}/add.py", dir.path);
    REQUIRE(r1.has_value());
    CHECK(*r1 == platform::PathToUtf8(dir.path) + "/add.py");

    auto r2 = ExpandPluginDirPlaceholder("pre/${plugin_dir}/mid/${plugin_dir}/post", dir.path);
    REQUIRE(r2.has_value());
    CHECK(*r2 == "pre/" + platform::PathToUtf8(dir.path) + "/mid/" + platform::PathToUtf8(dir.path) + "/post");

    auto r3 = ExpandPluginDirPlaceholder("plain.txt", dir.path);
    REQUIRE(r3.has_value());
    CHECK(*r3 == "plain.txt");
}

TEST_CASE("${plugin_dir} 替换后逃出插件目录即拒") {
    // .. 逃逸:canonical 后落在插件目录外。
    TempDir dir;
    const std::string escape = R"json({
      "manifest_version": 1, "id": "a", "version": "1",
      "runtime": {"kind": "process", "command": "x",
                  "args": ["${plugin_dir}/../escape.py"]},
      "tools": [{"name": "t", "input_schema": {"type": "object"}}]
    })json";
    auto r = ParsePluginManifest(escape, dir.path);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().find("逃出") != std::string::npos);

    // 绝对路径直写占位符开头再荡出去:${plugin_dir}/../../etc/passwd
    const std::string escape2 = R"json({
      "manifest_version": 1, "id": "a", "version": "1",
      "runtime": {"kind": "process", "command": "x",
                  "args": ["${plugin_dir}/../../etc/passwd"]},
      "tools": [{"name": "t", "input_schema": {"type": "object"}}]
    })json";
    auto r2 = ParsePluginManifest(escape2, dir.path);
    REQUIRE_FALSE(r2.has_value());
}

TEST_CASE("不带占位符的 command/args(走 PATH 的解释器名)不算逃逸") {
    TempDir dir;
    auto r = ParsePluginManifest(kGoodManifest, dir.path);
    REQUIRE(r.has_value());
    CHECK(r->argv[0] == "python3");  // 相对名,按 PATH 找,明确批准的外部 executable
}

// ---------------------------------------------------------------------------
// 目录扫描
// ---------------------------------------------------------------------------

TEST_CASE("ScanPluginDirectories:好插件挂上,坏插件警告跳过,跨插件重名拒后到") {
    TempDir root;
    // 好插件 alpha
    {
        std::error_code ec;
        std::filesystem::create_directories(root.path / "alpha", ec);
        std::ofstream out(root.path / "alpha" / "plugin.json", std::ios::binary);
        out << R"json({
          "manifest_version": 1, "id": "alpha", "version": "1.0.0",
          "runtime": {"kind": "process", "command": "python3"},
          "tools": [{"name": "echo", "description": "回声", "input_schema": {"type": "object"}}]
        })json";
    }
    // 坏插件 beta(manifest 不是 JSON)
    {
        std::error_code ec;
        std::filesystem::create_directories(root.path / "beta", ec);
        std::ofstream out(root.path / "beta" / "plugin.json", std::ios::binary);
        out << "{oops";
    }
    // 插件 zeta:目录名不同但 manifest.id 也叫 alpha——完整工具名
    // plugin__alpha__echo 与先到的 alpha 真撞,整件拒。目录名排在后面,
    // "先到"确定(扫描按目录名排序)。
    {
        std::error_code ec;
        std::filesystem::create_directories(root.path / "zeta", ec);
        std::ofstream out(root.path / "zeta" / "plugin.json", std::ios::binary);
        out << R"json({
          "manifest_version": 1, "id": "alpha", "version": "0.9.0",
          "runtime": {"kind": "process", "command": "python3"},
          "tools": [{"name": "echo", "description": "同 id 同名,撞完整名", "input_schema": {"type": "object"}}]
        })json";
    }
    // 没有 plugin.json 的目录(venv 之类),静默略过
    {
        std::error_code ec;
        std::filesystem::create_directories(root.path / "venv", ec);
    }

    auto result = ScanPluginDirectories(root.path);
    REQUIRE(result.manifests.size() == 1);  // alpha 挂上;beta 坏;zeta 撞完整名被拒
    CHECK(result.manifests[0]->id == "alpha");
    CHECK(result.manifests[0]->version == "1.0.0");  // 先到的 alpha,不是 zeta 那份 0.9.0
    REQUIRE(result.warnings.size() == 2);
    // 警告点名(beta 坏、zeta/alpha 撞名;venv 无 plugin.json 不算警告)
    bool saw_beta = false, saw_duplicate = false;
    for (const auto& warning : result.warnings) {
        if (warning.find("beta") != std::string::npos) {
            saw_beta = true;
        }
        if (warning.find("撞") != std::string::npos && warning.find("plugin__alpha__echo") != std::string::npos) {
            saw_duplicate = true;
        }
    }
    CHECK(saw_beta);
    CHECK(saw_duplicate);
}

TEST_CASE("ScanPluginDirectories:目录不存在静默返回空") {
    auto result = ScanPluginDirectories(std::filesystem::temp_directory_path() / "lubancode_no_such_plugins_dir");
    CHECK(result.manifests.empty());
    CHECK(result.warnings.empty());
}

// ---------------------------------------------------------------------------
// 模型可见性(验收第 3 样):定义不含宿主元数据
// ---------------------------------------------------------------------------

TEST_CASE("模型可见定义只有 name/description/input_schema,不含宿主元数据") {
    TempDir dir;
    auto manifest = ParsePluginManifest(kGoodManifest, dir.path);
    REQUIRE(manifest.has_value());
    auto shared = std::make_shared<const PluginManifest>(std::move(*manifest));
    PluginToolAdapter tool(shared, &shared->tools[0]);

    // 三样之外,接口上根本拿不到宿主元数据;再把可见文本拼起来断言不漏。
    const std::string visible = tool.name() + " " + tool.description() + " " + tool.input_schema().dump();
    CHECK(visible.find("python3") == std::string::npos);        // command
    CHECK(visible.find("python") == visible.find("python3"));  // language=python 不在
    CHECK(visible.find("30000") == std::string::npos);         // timeout
    CHECK(visible.find("plugin_dir") == std::string::npos);    // path
    CHECK(visible.find("env") == std::string::npos);           // env allowlist
    // description 就是 manifest 里的原文,不加前缀
    CHECK(tool.description() == "Add two numbers.");
    CHECK(tool.needs_confirm());
    CHECK(tool.deferred());
}

// ---------------------------------------------------------------------------
// 协议帧
// ---------------------------------------------------------------------------

TEST_CASE("请求帧序列化:嵌套 object/中文/多行字符串原样进 JSON") {
    plugin_protocol::ProcessRequest request;
    request.call_id = "call_1";
    request.plugin = "local-math";
    request.tool = "add";
    request.entry = "add";
    request.arguments = nlohmann::json{
        {"text", "中文往返\n第二行\t带制表"},
        {"nested", {{"list", {1, 2, 3}}, {"flag", true}, {"nothing", nullptr}}},
        {"quote", "a\"b\\c;d&f|g"},
    };
    request.context_cwd = "D:/项目 根";
    const nlohmann::json frame = plugin_protocol::SerializeRequest(request);
    // v2 宿主通告(工具结果图片回喂单);v1 插件不读这个字段,照旧能跑。
    CHECK(frame["protocol"] == 2);
    CHECK(frame["call_id"] == "call_1");
    CHECK(frame["context"]["cwd"] == "D:/项目 根");
    // 往返不变形
    const nlohmann::json parsed = nlohmann::json::parse(frame.dump());
    CHECK(parsed["arguments"] == request.arguments);
}

TEST_CASE("响应解析:合法成功帧") {
    const std::string body = R"json({
      "protocol": 1, "call_id": "call_9", "ok": true,
      "content": [{"type": "text", "text": "3"}],
      "structured": 3
    })json";
    const auto parsed = plugin_protocol::ParseResponse(body, "call_9");
    REQUIRE(parsed.status == PluginErrorCode::Ok);
    CHECK(parsed.response.ok);
    CHECK(parsed.response.text == "3");
    CHECK(parsed.response.structured == nlohmann::json(3));
}

TEST_CASE("响应解析:多段 text 合并且保序") {
    const std::string body = R"json({
      "protocol": 1, "call_id": "c", "ok": true,
      "content": [
        {"type": "text", "text": "第一段"},
        {"type": "text", "text": "第二段"}
      ]
    })json";
    const auto parsed = plugin_protocol::ParseResponse(body, "c");
    REQUIRE(parsed.status == PluginErrorCode::Ok);
    CHECK(parsed.response.text == "第一段\n第二段");
}

TEST_CASE("响应解析:插件自报失败是合法帧,错误码原样收") {
    const std::string body = R"json({
      "protocol": 1, "call_id": "call_9", "ok": false,
      "error": {"code": "bad_input", "message": "a must be a number"}
    })json";
    const auto parsed = plugin_protocol::ParseResponse(body, "call_9");
    REQUIRE(parsed.status == PluginErrorCode::Ok);  // 协议层合法,分型在调用方
    CHECK_FALSE(parsed.response.ok);
    CHECK(parsed.response.error_code == "bad_input");
    CHECK(parsed.response.error_message == "a must be a number");
}

TEST_CASE("响应解析的终态矩阵:坏 UTF-8/坏 JSON/多帧/call_id 不合/未知 content") {
    // 坏 UTF-8:孤立的续字节
    const std::string bad_utf8 = std::string("\"\xff\xfe\" + payload");
    CHECK(plugin_protocol::ParseResponse(bad_utf8, "c").status == PluginErrorCode::BadUtf8);

    // 坏 JSON
    CHECK(plugin_protocol::ParseResponse("not json at all", "c").status == PluginErrorCode::BadJson);
    CHECK(plugin_protocol::ParseResponse("", "c").status == PluginErrorCode::BadJson);

    // stdout 前后混日志(不是恰好一份 JSON)
    const std::string mixed = "starting up...\n{\"protocol\":1,\"call_id\":\"c\",\"ok\":true,\"content\":[]}\ndone";
    CHECK(plugin_protocol::ParseResponse(mixed, "c").status == PluginErrorCode::BadJson);

    // 多枚 JSON
    const std::string two =
        R"({"protocol":1,"call_id":"c","ok":true,"content":[]}){"protocol":1,"call_id":"c","ok":true,"content":[]})";
    CHECK(plugin_protocol::ParseResponse(two, "c").status == PluginErrorCode::BadJson);

    // call_id 不合
    const std::string wrong_id = R"({"protocol":1,"call_id":"call_8","ok":true,"content":[]})";
    CHECK(plugin_protocol::ParseResponse(wrong_id, "call_9").status == PluginErrorCode::CallIdMismatch);
    const std::string no_id = R"({"protocol":1,"ok":true,"content":[]})";
    CHECK(plugin_protocol::ParseResponse(no_id, "call_9").status == PluginErrorCode::CallIdMismatch);

    // 未知 content type 不静默转字符串
    const std::string image_content =
        R"({"protocol":1,"call_id":"c","ok":true,"content":[{"type":"image","data":"..."}]})";
    CHECK(plugin_protocol::ParseResponse(image_content, "c").status == PluginErrorCode::UnknownContent);

    // ok=true 缺 content
    const std::string no_content = R"({"protocol":1,"call_id":"c","ok":true})";
    CHECK(plugin_protocol::ParseResponse(no_content, "c").status == PluginErrorCode::BadJson);

    // protocol 不合(v2 已合法,再高的版本才拒)
    const std::string wrong_protocol = R"({"protocol":3,"call_id":"c","ok":true,"content":[]})";
    CHECK(plugin_protocol::ParseResponse(wrong_protocol, "c").status == PluginErrorCode::BadJson);
}

// ---------------------------------------------------------------------------
// 协议 v2:结果带图(工具结果图片回喂单)
// ---------------------------------------------------------------------------

TEST_CASE("v2 响应解析:image 块的 data 与 path 两种来源都收") {
    const std::string base64_frame = R"json({
      "protocol": 2, "call_id": "c", "ok": true,
      "content": [
        {"type": "text", "text": "已截图"},
        {"type": "image", "mime_type": "image/png", "data": "aGVsbG8="}
      ]
    })json";
    {
        const auto parsed = plugin_protocol::ParseResponse(base64_frame, "c");
        REQUIRE(parsed.status == PluginErrorCode::Ok);
        CHECK(parsed.response.ok);
        CHECK(parsed.response.text == "已截图");
        REQUIRE(parsed.response.images.size() == 1);
        CHECK(parsed.response.images[0].mime_type == "image/png");
        CHECK(parsed.response.images[0].data_base64 == "aGVsbG8=");
        CHECK(parsed.response.images[0].path.empty());
    }
    const std::string path_frame = R"json({
      "protocol": 2, "call_id": "c", "ok": true,
      "content": [
        {"type": "image", "mime_type": "image/png", "path": "C:/tmp/shot.png"}
      ]
    })json";
    {
        const auto parsed = plugin_protocol::ParseResponse(path_frame, "c");
        REQUIRE(parsed.status == PluginErrorCode::Ok);
        REQUIRE(parsed.response.images.size() == 1);
        CHECK(parsed.response.images[0].path == "C:/tmp/shot.png");
        CHECK(parsed.response.images[0].data_base64.empty());
    }
}

TEST_CASE("v2 响应解析:image 块的形状错误按 BadJson 收口") {
    // data 与 path 恰给其一:两个都给 / 两个都不给都坏
    const std::string both = R"({"protocol":2,"call_id":"c","ok":true,
      "content":[{"type":"image","mime_type":"image/png","data":"YQ==","path":"x.png"}]})";
    CHECK(plugin_protocol::ParseResponse(both, "c").status == PluginErrorCode::BadJson);
    const std::string neither = R"({"protocol":2,"call_id":"c","ok":true,
      "content":[{"type":"image","mime_type":"image/png"}]})";
    CHECK(plugin_protocol::ParseResponse(neither, "c").status == PluginErrorCode::BadJson);
    // mime_type 缺失/非串
    const std::string no_mime = R"({"protocol":2,"call_id":"c","ok":true,
      "content":[{"type":"image","data":"YQ=="}]})";
    CHECK(plugin_protocol::ParseResponse(no_mime, "c").status == PluginErrorCode::BadJson);
    // data 非串
    const std::string bad_data = R"({"protocol":2,"call_id":"c","ok":true,
      "content":[{"type":"image","mime_type":"image/png","data":42}]})";
    CHECK(plugin_protocol::ParseResponse(bad_data, "c").status == PluginErrorCode::BadJson);
}

TEST_CASE("v1 兼容:v1 响应照旧只认 text,image 块仍是 UnknownContent") {
    const std::string v1_text = R"({"protocol":1,"call_id":"c","ok":true,
      "content":[{"type":"text","text":"纯文本"}]})";
    const auto parsed = plugin_protocol::ParseResponse(v1_text, "c");
    REQUIRE(parsed.status == PluginErrorCode::Ok);
    CHECK(parsed.response.text == "纯文本");
    CHECK(parsed.response.images.empty());

    // v1 帧里冒出 image:违反 v1 合同,老钉子不动
    const std::string v1_image = R"({"protocol":1,"call_id":"c","ok":true,
      "content":[{"type":"image","mime_type":"image/png","data":"YQ=="}]})";
    CHECK(plugin_protocol::ParseResponse(v1_image, "c").status == PluginErrorCode::UnknownContent);
}

TEST_CASE("请求帧说 protocol=2(v2 宿主通告;v1 插件不读这个字段照旧能跑)") {
    plugin_protocol::ProcessRequest request;
    request.plugin = "p";
    request.tool = "t";
    request.entry = "t";
    request.call_id = "c1";
    request.arguments = nlohmann::json{{"x", 1}};
    const nlohmann::json frame = plugin_protocol::SerializeRequest(request);
    CHECK(frame["protocol"] == 2);
}

// ---------------------------------------------------------------------------
// 入参 Schema 子集
// ---------------------------------------------------------------------------

TEST_CASE("入参校验:required/type/enum/const") {
    const nlohmann::json schema = nlohmann::json::parse(R"json({
      "type": "object",
      "properties": {
        "mode": {"type": "string", "enum": ["fast", "slow"]},
        "level": {"const": 3},
        "count": {"type": "integer"}
      },
      "required": ["mode"]
    })json");
    CHECK_FALSE(ValidateArgumentsAgainstSchema(nlohmann::json{{"mode", "fast"}}, schema).has_value());
    CHECK(ValidateArgumentsAgainstSchema(nlohmann::json{{"mode", "medium"}}, schema)
              .has_value());  // 枚举外
    CHECK(ValidateArgumentsAgainstSchema(nlohmann::json{{"mode", "fast"}, {"level", 4}}, schema)
              .has_value());  // const 不合
    CHECK(ValidateArgumentsAgainstSchema(nlohmann::json{{"mode", "fast"}, {"level", 3}}, schema).has_value() ==
          false);
    CHECK(ValidateArgumentsAgainstSchema(nlohmann::json{}, schema).has_value());          // 缺 required
    CHECK(ValidateArgumentsAgainstSchema(nlohmann::json{{"mode", "fast"}, {"count", 1.5}}, schema)
              .has_value());  // integer 收 1.5 不行
    CHECK_FALSE(
        ValidateArgumentsAgainstSchema(nlohmann::json{{"mode", "fast"}, {"count", 7}}, schema).has_value());
}

TEST_CASE("入参校验:min/max/长度(码点)/数组界/additionalProperties") {
    const nlohmann::json schema = nlohmann::json::parse(R"json({
      "type": "object",
      "properties": {
        "n": {"type": "number", "minimum": 0, "maximum": 100},
        "s": {"type": "string", "minLength": 2, "maxLength": 5},
        "list": {"type": "array", "minItems": 1, "maxItems": 3, "items": {"type": "integer"}},
        "obj": {"type": "object", "properties": {"x": {"type": "boolean"}}, "additionalProperties": false}
      },
      "additionalProperties": false
    })json");
    CHECK_FALSE(ValidateArgumentsAgainstSchema(
        nlohmann::json{{"n", 50}, {"s", "abc"}, {"list", {1, 2}}, {"obj", {{"x", true}}}}, schema)
                     .has_value());
    CHECK(ValidateArgumentsAgainstSchema(nlohmann::json{{"n", -1}}, schema).has_value());   // 低于 min
    CHECK(ValidateArgumentsAgainstSchema(nlohmann::json{{"n", 101}}, schema).has_value());  // 超过 max
    CHECK(ValidateArgumentsAgainstSchema(nlohmann::json{{"s", "a"}}, schema).has_value());  // 太短
    // 长度按码点:三个中文字 = 3 码点,过 minLength=2 maxLength=5
    CHECK_FALSE(ValidateArgumentsAgainstSchema(nlohmann::json{{"s", "中文串"}}, schema).has_value());
    CHECK(ValidateArgumentsAgainstSchema(nlohmann::json{{"s", "中文串过长了"}}, schema).has_value());
    CHECK(ValidateArgumentsAgainstSchema(nlohmann::json{{"list", {}}}, schema).has_value());  // 空
    CHECK(ValidateArgumentsAgainstSchema(nlohmann::json{{"list", {1, "x"}}}, schema)
              .has_value());  // items 类型不合
    CHECK(ValidateArgumentsAgainstSchema(nlohmann::json{{"obj", {{"y", 1}}}}, schema)
              .has_value());  // additionalProperties=false 拦未知键
    CHECK(ValidateArgumentsAgainstSchema(nlohmann::json{{"extra", 1}}, schema)
              .has_value());  // 顶层 additionalProperties=false
}

TEST_CASE("入参校验:嵌套 object 递归到底") {
    const nlohmann::json schema = nlohmann::json::parse(R"json({
      "type": "object",
      "properties": {
        "nested": {
          "type": "object",
          "properties": {
            "deep": {"type": "array", "items": {"type": "object", "properties": {"v": {"type": "number"}}}}
          }
        }
      }
    })json");
    // nlohmann 的嵌套 init list 有 object/array 二义性,深层结构一律 parse 写。
    const nlohmann::json good = nlohmann::json::parse(R"({"nested": {"deep": [{"v": 1}, {"v": 2.5}]}})");
    const nlohmann::json bad = nlohmann::json::parse(R"({"nested": {"deep": [{"v": "字符串"}]}})");
    CHECK_FALSE(ValidateArgumentsAgainstSchema(good, schema).has_value());
    CHECK(ValidateArgumentsAgainstSchema(bad, schema).has_value());  // 三层底下也能拦住
}

TEST_CASE("入参校验:没声明 type 的位置放行声明怪之外的一切(无从校验)") {
    const nlohmann::json schema = nlohmann::json{{"type", "object"}};
    CHECK_FALSE(ValidateArgumentsAgainstSchema(nlohmann::json{{"anything", {{"deep", true}}}}, schema).has_value());
    CHECK(ValidateArgumentsAgainstSchema(nlohmann::json::array({1, 2}), schema).has_value());  // 顶层非 object
}
