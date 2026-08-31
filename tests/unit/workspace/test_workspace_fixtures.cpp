// workspace 统一存储 P0-0:夹具防漂移。九种旧数据场景的夹具住
// tests/fixtures/workspace/,本册用**现行** parser 逐件校验形状——夹具与
// 生产格式任何一边漂了,这里当场红。P0-6 旧 parser 退场时,本册随迁移器
// 测试改吃 tools/legacy-storage-migrator/ 的隔离副本,不陪葬。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "memory/frontmatter.hpp"
#include "sessions/session_store.hpp"

using namespace lubancode;

namespace {

std::filesystem::path FixtureRoot() {
    return std::filesystem::path(LUBANCODE_TEST_FIXTURES_DIR) / "workspace";
}

std::optional<std::string> ReadFileText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return std::nullopt;
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::vector<std::string> ReadLines(const std::filesystem::path& path) {
    std::vector<std::string> lines;
    const auto text = ReadFileText(path);
    if (!text.has_value()) return lines;
    std::istringstream stream(*text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

// 一场旧档的标准读法:ParseSessionFile 能吃下、meta 合法、有消息。
std::optional<sessions::LoadedSession> LoadFixture(const std::string& name) {
    const auto path = FixtureRoot() / "legacy" / (name + ".jsonl");
    const auto text = ReadFileText(path);
    if (!text.has_value()) return std::nullopt;
    return sessions::ParseSessionFile(*text);
}

}  // namespace

TEST_CASE("workspace 夹具:manifest 列的每一件都在盘上") {
    const auto manifest_path = FixtureRoot() / "manifest.json";
    const auto text = ReadFileText(manifest_path);
    REQUIRE(text.has_value());
    const auto manifest = nlohmann::json::parse(*text, nullptr, false);
    REQUIRE_FALSE(manifest.is_discarded());
    REQUIRE(manifest.contains("scenarios"));
    int checked = 0;
    for (const auto& scenario : manifest["scenarios"]) {
        const std::string file = scenario.value("file", std::string());
        REQUIRE_FALSE(file.empty());
        std::error_code ec;
        CHECK(std::filesystem::exists(FixtureRoot() / file, ec));
        ++checked;
    }
    // 九种场景:八份旧档场景 + memory/job 三件(项目/全局/job 分列)。
    CHECK(checked >= 9);
}

TEST_CASE("workspace 夹具:普通会话与工具结果都能整场解析") {
    const auto plain = LoadFixture("plain-conversation");
    REQUIRE(plain.has_value());
    CHECK(plain->meta.version == 1);
    CHECK(plain->meta.cwd == "C:/Users/sandbox/work/demo-repo");
    CHECK(plain->messages.size() == 4);

    const auto tools = LoadFixture("tool-roundtrip");
    REQUIRE(tools.has_value());
    CHECK(tools->messages.size() == 8);
    CHECK(tools->repaired == 0);  // tool_use/tool_result 全配对,无孤儿
}

TEST_CASE("workspace 夹具:MCP rich result 的块账无损") {
    const auto rich = LoadFixture("mcp-rich-result");
    REQUIRE(rich.has_value());
    // 富块留在全量流水里:找带 blocks 的 tool_result。
    int rich_results = 0;
    int image_blocks = 0;
    int audio_blocks = 0;
    for (const auto& message : rich->all_messages) {
        for (const auto& block : message.content) {
            const auto* result = std::get_if<api::ToolResultBlock>(&block);
            if (result == nullptr) continue;
            rich_results += 1;
        }
    }
    CHECK(rich_results == 1);
    // 块细节直接读原始行:parser 投影不携 rich 块时,形状由 JSON 行守。
    const auto lines = ReadLines(FixtureRoot() / "legacy" / "mcp-rich-result.jsonl");
    const std::string* result_line = nullptr;
    for (const auto& line : lines) {
        if (line.find("\"blocks\"") != std::string::npos) result_line = &line;
    }
    REQUIRE(result_line != nullptr);
    const auto json = nlohmann::json::parse(*result_line, nullptr, false);
    REQUIRE_FALSE(json.is_discarded());
    const auto& blocks = json["content"][0]["blocks"];
    CHECK(blocks.size() == 5);
    for (const auto& block : blocks) {
        const std::string type = block.value("type", std::string());
        if (type == "image") image_blocks += 1;
        if (type == "audio") {
            audio_blocks += 1;
            CHECK(block["artifact"].value("stored", false) == false);  // 故意留的缺口样本
        }
    }
    CHECK(image_blocks == 1);
    CHECK(audio_blocks == 1);
    CHECK(json["content"][0].contains("structured_content"));
    // 落盘的 artifact 字节在(mcp-artifacts/ 旁挂目录)。
    std::error_code ec;
    CHECK(std::filesystem::exists(
        FixtureRoot() / "legacy" / "mcp-rich-result.mcp-artifacts" / "art-3f2a1b9c.png", ec));
}

TEST_CASE("workspace 夹具:前后台子代理只有最终回话,无子账") {
    const auto foreground = LoadFixture("subagent-foreground");
    REQUIRE(foreground.has_value());
    bool has_agent_call = false;
    for (const auto& message : foreground->all_messages) {
        for (const auto& block : message.content) {
            const auto* use = std::get_if<api::ToolUseBlock>(&block);
            if (use != nullptr && use->name == "agent") has_agent_call = true;
        }
    }
    CHECK(has_agent_call);
    // 旧格式没有 subagents/ 子账——这正是迁移须标 unavailable_legacy 的场景。

    const auto background = LoadFixture("subagent-background");
    REQUIRE(background.has_value());
    CHECK(background->queued_messages.size() == 1);
    CHECK(background->queued_messages[0].subagent);
    CHECK(background->queued_messages[0].task_id == 3);
}

TEST_CASE("workspace 夹具:compact_v2 与 resume/title 事件") {
    const auto compact = LoadFixture("compact");
    REQUIRE(compact.has_value());
    CHECK(compact->compact_count == 1);
    CHECK(compact->compact_epoch == 1);
    CHECK(compact->last_compact_manifest.contains("goal"));

    const auto resumed = LoadFixture("resume");
    REQUIRE(resumed.has_value());
    CHECK(resumed->title == "demo-repo 目录说明");
    CHECK(resumed->messages.size() == 4);
}

TEST_CASE("workspace 夹具:linked worktree 的 cwd 事件序列") {
    const auto lines = ReadLines(FixtureRoot() / "legacy" / "linked-worktree.jsonl");
    std::vector<std::string> cwds;
    for (const auto& line : lines) {
        if (auto cwd = sessions::ParseCwdEvent(line); cwd.has_value()) {
            cwds.push_back(*cwd);
        }
    }
    REQUIRE(cwds.size() == 2);
    CHECK(cwds[0] == "C:/Users/sandbox/work/.wt/demo-repo-feature");  // 进 worktree 房
    CHECK(cwds[1] == "C:/Users/sandbox/work/demo-repo");              // 回主树
    // 会话本体照旧可整场解析(事件行不算坏行)。
    const auto loaded = LoadFixture("linked-worktree");
    REQUIRE(loaded.has_value());
    CHECK(loaded->messages.size() == 6);  // 三轮往返;两行 cwd 事件不计入
}

TEST_CASE("workspace 夹具:项目/全局 Memory 的主题与 catalog 对得上") {
    const auto project_catalog_path =
        FixtureRoot() / "memory" / "project" / ".state" / "catalog.json";
    const auto project_text = ReadFileText(project_catalog_path);
    REQUIRE(project_text.has_value());
    const auto project_catalog = nlohmann::json::parse(*project_text, nullptr, false);
    REQUIRE_FALSE(project_catalog.is_discarded());
    REQUIRE(project_catalog["entries"].size() == 3);

    // catalog 列的每一件主题文件都在,front matter 能被现行 parser 吃下,
    // id/scope 与目录层一致(facts/preferences/feedback)。
    for (const auto& entry : project_catalog["entries"]) {
        const std::string file = entry.value("file", std::string());
        const auto topic_path = FixtureRoot() / "memory" / "project" / file;
        const auto topic_text = ReadFileText(topic_path);
        REQUIRE(topic_text.has_value());
        const auto parsed = memory::frontmatter::Parse(*topic_text);
        REQUIRE(parsed.has_value());
        CHECK(parsed->entry.id == entry.value("id", std::string()));
        CHECK(parsed->entry.scope.level == "project");
        CHECK(parsed->entry.schema == 3);
    }

    const auto user_text =
        ReadFileText(FixtureRoot() / "memory" / "user" / ".state" / "catalog.json");
    REQUIRE(user_text.has_value());
    const auto user_catalog = nlohmann::json::parse(*user_text, nullptr, false);
    REQUIRE_FALSE(user_catalog.is_discarded());
    REQUIRE(user_catalog["entries"].size() == 2);
    for (const auto& entry : user_catalog["entries"]) {
        const auto topic_text =
            ReadFileText(FixtureRoot() / "memory" / "user" / entry.value("file", std::string()));
        REQUIRE(topic_text.has_value());
        const auto parsed = memory::frontmatter::Parse(*topic_text);
        REQUIRE(parsed.has_value());
        CHECK(parsed->entry.scope.level == "user");  // 全局层不带项目 key
        CHECK(parsed->entry.evidence.empty());       // 也不假借项目路径作证据
    }
}

TEST_CASE("workspace 夹具:recall trace、候选与 pending job 的形状") {
    const auto trace_text =
        ReadFileText(FixtureRoot() / "memory" / "project" / ".state" / "recall-trace.json");
    REQUIRE(trace_text.has_value());
    const auto trace = nlohmann::json::parse(*trace_text, nullptr, false);
    REQUIRE_FALSE(trace.is_discarded());
    CHECK(trace.value("schema", 0) == 2);
    CHECK(trace.contains("project_key"));  // 旧钥匙名:P0-3 起改 workspace_key
    CHECK(trace["entries"].size() == 3);

    const auto candidate_text = ReadFileText(FixtureRoot() / "memory" / "project" /
                                             "memory-candidates" / "cand.fact.hatch-path-001.json");
    REQUIRE(candidate_text.has_value());
    const auto candidate = nlohmann::json::parse(*candidate_text, nullptr, false);
    REQUIRE_FALSE(candidate.is_discarded());
    CHECK(candidate.value("schema", 0) == 1);
    CHECK(candidate.value("confidence", std::string()) == "inferred");

    const auto job_dir = FixtureRoot() / "memory-jobs" / "pending";
    std::error_code ec;
    int jobs = 0;
    for (const auto& item : std::filesystem::directory_iterator(job_dir, ec)) {
        const auto job_text = ReadFileText(item.path());
        REQUIRE(job_text.has_value());
        const auto job = nlohmann::json::parse(*job_text, nullptr, false);
        REQUIRE_FALSE(job.is_discarded());
        CHECK(job.value("operation", std::string()) == "upsert");
        CHECK(job.contains("project_key"));  // P0-3 起按它路由进 workspace
        jobs += 1;
    }
    CHECK(jobs == 1);
}
