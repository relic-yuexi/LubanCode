// plugins 协议 v2(工具结果图片回喂单)的真机单测:自带测试 helper(测试
// 自己写的小 Python 脚本),PluginToolAdapter 端到端——
//   v2 image 块(path/base64 两源)→ 宿主验身落账 → payload 带 ImageContent
//   → 重灌上 wire(anthropic 原生 image 块)全链一杆子到底;
//   伪 MIME / 无落盘地 → image_rejected 整次收口。
// 规矩与 mcp/rich_result 同源(帽/魔数/内容寻址),这里验的是插件协议这
// 一侧的接线。缺 Python 的环境整文件 SKIP(照 test_plugin_process.cpp)。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "agent/model_image_store.hpp"
#include "agent/tool_result_images.hpp"
#include "api/anthropic/client.hpp"
#include "platform/process.hpp"
#include "runtime/plugin_contract.hpp"
#include "runtime/plugin_tool.hpp"

using namespace lubancode;
using namespace lubancode::runtime;

namespace {

#ifdef _WIN32
constexpr const char* kPythonCmd = "python";
#else
constexpr const char* kPythonCmd = "python3";
#endif

bool PythonAvailable() {
    static const bool available = [] {
        const auto result = platform::RunProcess(std::vector<std::string>{kPythonCmd, "--version"}, 10000);
        return result.exit_code == 0 && result.output.find("Python") != std::string::npos;
    }();
    return available;
}

// 1x1 PNG(与 fixtures/mcp_test_server.py 同一颗)。
const char* kTinyPngB64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQ"
    "AAAABJRU5ErkJggg==";

std::string TinyPngBytes() {
    const auto decoded = agent::DecodeBase64Strict(kTinyPngB64, 1024);
    REQUIRE(decoded.has_value());
    return *decoded;
}

struct TempDir {
    std::filesystem::path path;
    TempDir(const std::string& tag) {
        std::error_code ec;
        path = std::filesystem::temp_directory_path() /
              (tag + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
               std::to_string(++counter_));
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string utf8() const { return path.generic_string(); }
    void Write(const std::string& name, const std::string& bytes) const {
        std::ofstream out(path / name, std::ios::binary);
        out << bytes;
    }

  private:
    static int counter_;
};
int TempDir::counter_ = 0;

// 万能 helper(与 test_plugin_process.cpp 同骨架):build_response 由各用例
// 注入,响应 dict 写 stdout。
const char* kHelperScript = R"py(
import json, sys

sys.stdin.reconfigure(encoding="utf-8")
sys.stdout.reconfigure(encoding="utf-8")
sys.stderr.reconfigure(encoding="utf-8")

request = json.load(sys.stdin)
response = None
try:
    response = build_response(request)
except Exception as error:
    response = {
        "protocol": 2,
        "call_id": request.get("call_id", ""),
        "ok": False,
        "error": {"code": "execution_failed", "message": str(error)},
    }
json.dump(response, sys.stdout, ensure_ascii=False)
sys.stdout.flush()
)py";

std::string BuildManifestText() {
    std::string text = R"json({
      "manifest_version": 1, "id": "image-probe", "version": "1.0.0", "language": "python",
      "runtime": {"kind": "process", "command": ")json";
    text += kPythonCmd;
    text += R"json(", "args": ["${plugin_dir}/helper.py"], "timeout_ms": 30000},
      "tools": [{"name": "shot", "description": "d", "input_schema": {"type": "object"}}]
    })json";
    return text;
}

// adapter 与它指着的 manifest 得同寿(definition_ 是裸指针):holder 把
// 两者钉在一起,测试作用域内都活着。
struct AdapterHolder {
    std::shared_ptr<const PluginManifest> manifest;
    std::unique_ptr<PluginToolAdapter> adapter;

    tools::Tool::Result execute(const nlohmann::json& input, const std::string& artifact_dir) {
        return adapter->execute(input, tools::ToolExecutionContext{nullptr, artifact_dir});
    }
};

AdapterHolder MakeAdapter(const TempDir& dir, const std::string& script_python) {
    dir.Write("helper.py", script_python + "\n" + kHelperScript);
    auto manifest = ParsePluginManifest(BuildManifestText(), dir.path);
    if (!manifest.has_value()) {
        INFO(manifest.error());
        REQUIRE(manifest.has_value());
    }
    AdapterHolder holder;
    holder.manifest = std::make_shared<const PluginManifest>(std::move(*manifest));
    holder.adapter = std::make_unique<PluginToolAdapter>(holder.manifest, &holder.manifest->tools[0]);
    return holder;
}

}  // namespace

TEST_CASE("v2 端到端: path 源图片 → 落账 → payload 带 ImageContent → 重灌上 anthropic wire") {
    if (!PythonAvailable()) {
        return;
    }
    TempDir plugin_dir("lubancode_plugin_img");
    TempDir artifact_dir("lubancode_plugin_art");
    plugin_dir.Write("shot.png", TinyPngBytes());

    auto holder = MakeAdapter(
        plugin_dir, R"py(def build_response(request):
    return {
        "protocol": 2,
        "call_id": request["call_id"],
        "ok": True,
        "content": [
            {"type": "text", "text": "已截图"},
            {"type": "image", "mime_type": "image/png", "path": request["arguments"]["image_path"]},
        ],
    })py");
    const auto result =
        holder.execute(nlohmann::json{{"image_path", (plugin_dir.path / "shot.png").string()}},
                       artifact_dir.utf8());
    if (result.is_error) {
        INFO(result.content);
        REQUIRE_FALSE(result.is_error);
    }
    // payload:text 块 + ImageContent(artifact 已落会话目录,内容寻址)。
    REQUIRE(result.payload.content.size() == 2);
    const auto* image = std::get_if<tools::ImageContent>(&result.payload.content[1]);
    REQUIRE(image != nullptr);
    CHECK(image->mime_type == "image/png");
    CHECK(image->artifact.stored);
    CHECK(image->artifact.filename.rfind("art-", 0) == 0);
    CHECK(std::filesystem::exists(artifact_dir.path / image->artifact.filename));
    // 投影文本带 artifact 短句。
    CHECK(result.content.find("[图片 art-") != std::string::npos);

    // 一杆子到底:payload 进 ToolResultBlock,重灌,anthropic wire 见原生图。
    api::Request wire_request;
    wire_request.model = "m";
    api::ToolResultBlock rich;
    rich.tool_use_id = "c1";
    rich.blocks = result.payload.content;
    rich.content = result.content;
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(std::move(rich));
    wire_request.messages.push_back(message);
    CHECK(agent::RehydrateToolResultImages(wire_request, artifact_dir.utf8()) == 1);
    const auto body = api::anthropic::BuildRequestJson(wire_request);
    const auto& wire_result = body.at("messages")[0].at("content")[0];
    REQUIRE(wire_result.at("content").is_array());
    CHECK(wire_result.at("content").size() == 2);
    CHECK(wire_result.at("content")[1].at("type") == "image");
    CHECK(wire_result.at("content")[1].at("source").at("data") == kTinyPngB64);
}

TEST_CASE("v2 端到端: base64 源图片同样落账") {
    if (!PythonAvailable()) {
        return;
    }
    TempDir plugin_dir("lubancode_plugin_img");
    TempDir artifact_dir("lubancode_plugin_art");
    auto holder = MakeAdapter(
        plugin_dir, std::string(R"py(def build_response(request):
    return {
        "protocol": 2,
        "call_id": request["call_id"],
        "ok": True,
        "content": [
            {"type": "text", "text": "图来了"},
            {"type": "image", "mime_type": "image/png", "data": ")py") +
                                  kTinyPngB64 + R"py("},
        ],
    })py");
    const auto result = holder.execute(nlohmann::json::object(), artifact_dir.utf8());
    REQUIRE_FALSE(result.is_error);
    const auto* image = std::get_if<tools::ImageContent>(&result.payload.content.back());
    REQUIRE(image != nullptr);
    CHECK(image->artifact.stored);
    CHECK(std::filesystem::exists(artifact_dir.path / image->artifact.filename));
}

TEST_CASE("v2 拒收: 伪 MIME(image/png 装 EXE 字节)整次 image_rejected") {
    if (!PythonAvailable()) {
        return;
    }
    TempDir plugin_dir("lubancode_plugin_img");
    TempDir artifact_dir("lubancode_plugin_art");
    plugin_dir.Write("fake.png", "this is definitely not a png file");
    auto holder = MakeAdapter(
        plugin_dir, R"py(def build_response(request):
    return {
        "protocol": 2,
        "call_id": request["call_id"],
        "ok": True,
        "content": [
            {"type": "text", "text": "假图"},
            {"type": "image", "mime_type": "image/png", "path": request["arguments"]["image_path"]},
        ],
    })py");
    const auto result = holder.execute(nlohmann::json{{"image_path", (plugin_dir.path / "fake.png").string()}},
                                       artifact_dir.utf8());
    REQUIRE(result.is_error);
    CHECK(result.error_code == "plugin.image_rejected");
    CHECK(result.content.find("image_rejected") != std::string::npos);
    CHECK(result.content.find("魔数") != std::string::npos);
}

TEST_CASE("v2 拒收: 无 artifact 落盘地(单发路)整次 image_rejected") {
    if (!PythonAvailable()) {
        return;
    }
    TempDir plugin_dir("lubancode_plugin_img");
    auto holder = MakeAdapter(
        plugin_dir, std::string(R"py(def build_response(request):
    return {
        "protocol": 2,
        "call_id": request["call_id"],
        "ok": True,
        "content": [
            {"type": "text", "text": "图"},
            {"type": "image", "mime_type": "image/png", "data": ")py") +
                                  kTinyPngB64 + R"py("},
        ],
    })py");
    const auto result = holder.execute(nlohmann::json::object(), "");
    REQUIRE(result.is_error);
    CHECK(result.error_code == "plugin.image_rejected");
    CHECK(result.content.find("落盘地") != std::string::npos);
}
