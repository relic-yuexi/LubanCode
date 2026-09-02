// 派工任务书单 2.1(P1)/2.2(P2)的测试册:
//   - DetectSubagentEnvFacts:preset 挑法(已配置树优先 > 名叫 release >
//     名单第一个)、build 树在不在、_deps 齐不齐、非 CMake 工程零事实;
//   - ComposeSubagentEnvAppendix:首行 [宿主注入·本机环境附录] 标来源,
//     正文带离线 _deps 路/FETCHCONTENT_FULLY_DISCONNECTED/ctest 临时
//     USERPROFILE(Windows)/--clean-first 四要点;
//   - AgentTool 派工注入:prompt 尾部带附录、用户正文逐字节不动、探测
//     一次缓存(重复派工不重复探测)、不设探测口零注入;
//   - template: full 六件套套壳:原文居中逐字节保留、六件齐全、附录永远
//     在最尾、非法取值拒绝、schema 列参。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/agent_tool.hpp"
#include "tools/registry.hpp"
#include "tools/subagent_env_appendix.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// 与 test_agent_tool.cpp 同一套假后端:按脚本吐事件,记下收到的 Request。
class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::vector<api::Request> captured_requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        (void)cancel;
        captured_requests.push_back(request);
        const std::size_t idx = captured_requests.size() - 1;
        if (idx >= scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "FakeBackend: 脚本用完了", 0});
        }
        for (const auto& event : scripts[idx]) {
            on_event(event);
        }
        return {};
    }
};

std::vector<api::StreamEvent> TextOnlyScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

// 首条 user 消息的纯文本(RunTask 的 initial_input 只有一枚 TextBlock,
// 测试没灌记忆召回 provider,消息原样进请求)。
std::string FirstUserText(const api::Request& request) {
    for (const auto& message : request.messages) {
        if (message.role != api::Role::User) {
            continue;
        }
        std::string text;
        for (const auto& block : message.content) {
            if (const auto* text_block = std::get_if<api::TextBlock>(&block)) {
                text += text_block->text;
            }
        }
        return text;
    }
    return std::string();
}

// 临时目录夹具:构造即建,析构即清;Write 顺手把父目录建好。
struct TempDir {
    std::filesystem::path root;

    TempDir()
        : root(std::filesystem::temp_directory_path() /
               ("lubancode_envappendix_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(root);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    void Write(const std::string& relative, const std::string& content) {
        const std::filesystem::path path = root / relative;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream(path, std::ios::binary) << content;
    }
    void MakeDir(const std::string& relative) {
        std::error_code ec;
        std::filesystem::create_directories(root / relative, ec);
    }
};

// 与 LubanCode 仓库同款的 preset 名单:debug 在前、release 在后,
// binaryDir 用 ${sourceDir} 占位。
constexpr const char* kDebugReleasePresets = R"({
  "version": 6,
  "configurePresets": [
    {
      "name": "debug",
      "generator": "X",
      "binaryDir": "${sourceDir}/build/debug",
      "cacheVariables": { "CMAKE_CONFIGURATION_TYPES": "Debug" }
    },
    {
      "name": "release",
      "generator": "X",
      "binaryDir": "${sourceDir}/build/release",
      "cacheVariables": { "CMAKE_CONFIGURATION_TYPES": "Release" }
    }
  ]
})";

}  // namespace

TEST_CASE("环境探测:已配置树的 preset 优先,build 树与 _deps 两样账落准") {
    TempDir repo;
    repo.Write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\n");
    repo.Write("CMakePresets.json", kDebugReleasePresets);
    // release 树配置过(CMakeCache.txt 在),_deps 里有料;debug 树不存在。
    repo.Write("build/release/CMakeCache.txt", "# cache\n");
    repo.Write("build/release/_deps/fmt-src/CMakeLists.txt", "");
    const tools::SubagentEnvFacts facts = tools::DetectSubagentEnvFacts(repo.root);
    CHECK(facts.is_cmake_project);
    CHECK(facts.preset_name == "release");  // 名单里 debug 在前,但只有 release 有配置树
    CHECK(facts.build_dir_utf8 == "build/release");
    CHECK(facts.build_config == "Release");
    CHECK(facts.build_tree_present);
    CHECK(facts.offline_deps_ready);
}

TEST_CASE("环境探测:没有配置树时退到 release 名,再退名单第一个") {
    TempDir repo;
    repo.Write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\n");
    repo.Write("CMakePresets.json", kDebugReleasePresets);
    const tools::SubagentEnvFacts facts = tools::DetectSubagentEnvFacts(repo.root);
    CHECK(facts.is_cmake_project);
    CHECK(facts.preset_name == "release");  // 一棵树都没有:挑 release 档
    CHECK(facts.build_dir_utf8 == "build/release");
    CHECK_FALSE(facts.build_tree_present);
    CHECK_FALSE(facts.offline_deps_ready);

    TempDir single;
    single.Write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\n");
    single.Write("CMakePresets.json", R"({"version":6,"configurePresets":[
        {"name":"msvc","binaryDir":"${sourceDir}/build/msvc"}]})");
    const tools::SubagentEnvFacts lone = tools::DetectSubagentEnvFacts(single.root);
    CHECK(lone.preset_name == "msvc");  // 名单没有 release:退第一个
}

TEST_CASE("环境探测:没有 preset 文件时扫 build/ 下带 CMakeCache 的子目录") {
    TempDir repo;
    repo.Write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\n");
    repo.Write("build/odd-tree/CMakeCache.txt", "# cache\n");
    repo.Write("build/odd-tree/_deps/one-subbuild/CMakeLists.txt", "");
    const tools::SubagentEnvFacts facts = tools::DetectSubagentEnvFacts(repo.root);
    CHECK(facts.is_cmake_project);
    CHECK(facts.preset_name.empty());
    CHECK(facts.build_dir_utf8 == "build/odd-tree");
    CHECK(facts.build_tree_present);
    CHECK(facts.offline_deps_ready);
}

TEST_CASE("环境探测:非 CMake 工程零事实;空 _deps 不算离线料齐") {
    TempDir plain;
    plain.Write("package.json", "{}\n");
    const tools::SubagentEnvFacts plain_facts = tools::DetectSubagentEnvFacts(plain.root);
    CHECK_FALSE(plain_facts.is_cmake_project);
    CHECK(plain_facts.preset_name.empty());
    CHECK(plain_facts.build_dir_utf8.empty());

    TempDir repo;
    repo.Write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\n");
    repo.Write("CMakePresets.json", kDebugReleasePresets);
    repo.Write("build/release/CMakeCache.txt", "# cache\n");
    repo.MakeDir("build/release/_deps");  // _deps 在但空着:离线路走不通
    const tools::SubagentEnvFacts facts = tools::DetectSubagentEnvFacts(repo.root);
    CHECK(facts.build_tree_present);
    CHECK_FALSE(facts.offline_deps_ready);
}

TEST_CASE("附录成文:来源标注与构建四要点,非 CMake 工程不注入") {
    tools::SubagentEnvFacts facts;
    facts.is_cmake_project = true;
    facts.preset_name = "release";
    facts.build_config = "Release";
    facts.build_dir_utf8 = "build/release";
    facts.build_tree_present = true;
    facts.offline_deps_ready = true;
    const std::string appendix = tools::ComposeSubagentEnvAppendix(facts);
    CHECK(appendix.find("[宿主注入·本机环境附录]") == 0);  // 首行标来源
    CHECK(appendix.find("release") != std::string::npos);
    CHECK(appendix.find("build/release") != std::string::npos);
    CHECK(appendix.find("_deps") != std::string::npos);
    CHECK(appendix.find("FETCHCONTENT_FULLY_DISCONNECTED=ON") != std::string::npos);
    CHECK(appendix.find("--clean-first") != std::string::npos);
#ifdef _WIN32
    CHECK(appendix.find("USERPROFILE") != std::string::npos);
#endif

    facts.offline_deps_ready = false;
    CHECK(tools::ComposeSubagentEnvAppendix(facts).find("离线路走不通") != std::string::npos);
    facts.build_tree_present = false;
    CHECK(tools::ComposeSubagentEnvAppendix(facts).find("还没起构建树") != std::string::npos);

    CHECK(tools::ComposeSubagentEnvAppendix(tools::SubagentEnvFacts{}).empty());
}

TEST_CASE("派工注入:prompt 尾部带附录,正文逐字节不动,探测一次缓存") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("办完了"), TextOnlyScript("又办完了")};
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    const std::string appendix = "[宿主注入·本机环境附录] 测试附录:preset release,离线 _deps 齐。";
    int probe_calls = 0;
    agent_tool.SetEnvAppendixProbe([&probe_calls, appendix]() {
        ++probe_calls;
        return appendix;
    });

    const std::string prompt = "修构建脚本,把 preset 对齐。";
    CHECK_FALSE(agent_tool.execute(nlohmann::json{{"title", "对齐 preset"}, {"prompt", prompt}}).is_error);
    REQUIRE(backend.captured_requests.size() == 1);
    const std::string first = FirstUserText(backend.captured_requests[0]);
    // 正文在前(逐字节保留),附录在最尾,两段之间隔空行。
    CHECK(first.find(prompt) == 0);
    CHECK(first == prompt + "\n\n" + appendix);

    // 第二笔派工:附录照带(缓存),探测口不再被调。
    CHECK_FALSE(
        agent_tool.execute(nlohmann::json{{"title", "再对齐"}, {"prompt", prompt}}).is_error);
    REQUIRE(backend.captured_requests.size() == 2);
    CHECK(FirstUserText(backend.captured_requests[1]) == prompt + "\n\n" + appendix);
    CHECK(probe_calls == 1);
}

TEST_CASE("派工注入:不设探测口零注入,prompt 逐字节原样") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("办完了")};
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
    const std::string prompt = "只读摸排 build 目录,报告结论。";
    CHECK_FALSE(agent_tool.execute(nlohmann::json{{"title", "摸排 build"}, {"prompt", prompt}}).is_error);
    REQUIRE(backend.captured_requests.size() == 1);
    CHECK(FirstUserText(backend.captured_requests[0]) == prompt);
}

TEST_CASE("template: full 套壳:六件齐全,原文居中,附录永远在最尾") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("办完了"), TextOnlyScript("又办完了")};
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
    agent_tool.SetEnvAppendixProbe([]() { return "[宿主注入·本机环境附录] 测试附录。"; });

    const std::string prompt = "修 todos/某单.todo 的编号 1。";
    CHECK_FALSE(agent_tool.execute(nlohmann::json{{"title", "修单子"}, {"prompt", prompt}, {"template", "full"}})
                    .is_error);
    REQUIRE(backend.captured_requests.size() == 1);
    const std::string wrapped = FirstUserText(backend.captured_requests[0]);
    // 六件套齐全,原文逐字节居中。
    for (const std::string_view piece :
         {"单子路径", "范围红线", "环境实情", "纪律", "完工标准", "回报格式", "===== 任务原文 ====="}) {
        CHECK(wrapped.find(piece) != std::string::npos);
    }
    const std::size_t body = wrapped.find(prompt);
    const std::size_t original_marker = wrapped.find("===== 任务原文 =====");
    REQUIRE(body != std::string::npos);
    CHECK(body > original_marker);  // 原文在标记之后,不是被壳吃了
    // 附录在最尾:附录标记比"原文完"更靠后。
    CHECK(wrapped.rfind("[宿主注入·本机环境附录]") > wrapped.rfind("===== 原文完 ====="));

    // 不传 template:一个套壳标记都没有。
    CHECK_FALSE(agent_tool.execute(nlohmann::json{{"title", "再修"}, {"prompt", prompt}}).is_error);
    REQUIRE(backend.captured_requests.size() == 2);
    CHECK(FirstUserText(backend.captured_requests[1]).find("六件套") == std::string::npos);
}

TEST_CASE("template 参数校验:非法取值与类型拒绝,不发请求") {
    FakeBackend backend;
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    const tools::Tool::Result bad_value =
        agent_tool.execute(nlohmann::json{{"title", "坏模板"}, {"prompt", "干活"}, {"template", "mini"}});
    CHECK(bad_value.is_error);
    CHECK(bad_value.content.find("template") != std::string::npos);
    CHECK(bad_value.content.find("\"full\"") != std::string::npos);

    const tools::Tool::Result bad_type =
        agent_tool.execute(nlohmann::json{{"title", "坏类型"}, {"prompt", "干活"}, {"template", 5}});
    CHECK(bad_type.is_error);
    CHECK(bad_type.content.find("字符串") != std::string::npos);

    CHECK(backend.captured_requests.empty());  // 拒绝发生在发请求之前
}

TEST_CASE("schema:template 列入参数表,枚举只认 full,附录探测不进 schema") {
    FakeBackend backend;
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
    const nlohmann::json schema = agent_tool.input_schema();
    REQUIRE(schema.contains("properties"));
    REQUIRE(schema["properties"].contains("template"));
    const nlohmann::json& prop = schema["properties"]["template"];
    CHECK(prop["type"] == "string");
    REQUIRE(prop.contains("enum"));
    REQUIRE(prop["enum"].is_array());
    REQUIRE(prop["enum"].size() == 1);
    CHECK(prop["enum"][0] == "full");
    CHECK(prop["description"].is_string());
    CHECK(!prop["description"].get<std::string>().empty());
}
