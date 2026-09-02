// workspace 统一存储 P0-0:夹具防漂移。九种旧数据场景的夹具住
// tests/fixtures/workspace/,本册守形状——夹具与迁移器输入格式任何一边
// 漂了,这里当场红。
//
// P0-6 口径:旧 JSONL 的生产 parser(SessionStore/ParseSessionFile)已删,
// 这里不再借它校验——形状按 JSON 行本身守(逐行可解析、事件行带 type、
// 消息行带 role);"整场能被迁移器吃下并过 verify+replay"的端到端守门
// 在 tests/unit/workspace/test_storage_migrator.cpp(全件真导入)。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "memory/frontmatter.hpp"

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
        if (line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

// 一份 legacy 夹具的逐行解析账:每行都是合法 JSON 对象;首行是 meta
// (带整型 version),其后每行带 type(事件行)或 role(消息行)。
struct LegacyShape {
    nlohmann::json meta;
    std::vector<nlohmann::json> rows;
    std::size_t bad_lines = 0;
};

std::optional<LegacyShape> ShapeOf(const std::string& name) {
    const auto lines = ReadLines(FixtureRoot() / "legacy" / (name + ".jsonl"));
    if (lines.empty()) return std::nullopt;
    LegacyShape shape;
    shape.meta = nlohmann::json::parse(lines.front(), nullptr, false);
    if (shape.meta.is_discarded() || !shape.meta.is_object() ||
        !shape.meta.contains("version")) {
        return std::nullopt;
    }
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const auto json = nlohmann::json::parse(lines[i], nullptr, false);
        if (json.is_discarded() || !json.is_object() ||
            (!json.contains("type") && !json.contains("role"))) {
            shape.bad_lines += 1;
            continue;
        }
        shape.rows.push_back(std::move(json));
    }
    return shape;
}

std::size_t CountRows(const LegacyShape& shape, const std::string& key, const std::string& value) {
    std::size_t n = 0;
    for (const auto& row : shape.rows) {
        if (row.value(key, std::string()) == value) n += 1;
    }
    return n;
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

TEST_CASE("workspace 夹具:普通会话与工具往返逐行可解析") {
    const auto plain = ShapeOf("plain-conversation");
    REQUIRE(plain.has_value());
    CHECK(plain->meta.value("version", 0) == 1);
    CHECK(plain->meta.value("cwd", std::string()) == "C:/Users/sandbox/work/demo-repo");
    CHECK(plain->bad_lines == 0);
    CHECK(plain->rows.size() == 4);  // 两轮往返
    CHECK(CountRows(*plain, "role", "user") == 2);
    CHECK(CountRows(*plain, "role", "assistant") == 2);

    const auto tools = ShapeOf("tool-roundtrip");
    REQUIRE(tools.has_value());
    CHECK(tools->bad_lines == 0);
    // 两次工具往返:4 条消息行 + tool_trace_v1 事件行若干。
    std::size_t tool_calls = 0;
    std::size_t tool_results = 0;
    for (const auto& row : tools->rows) {
        if (!row.contains("content") || !row["content"].is_array()) continue;
        for (const auto& block : row["content"]) {
            const std::string type = block.value("type", std::string());
            tool_calls += type == "tool_use" ? 1 : 0;
            tool_results += type == "tool_result" ? 1 : 0;
        }
    }
    CHECK(tool_calls == 2);
    CHECK(tool_results == 2);
}

TEST_CASE("workspace 夹具:MCP rich result 的块账无损") {
    const auto rich = ShapeOf("mcp-rich-result");
    REQUIRE(rich.has_value());
    CHECK(rich->bad_lines == 0);
    // 富块留在流水里:找带 blocks 的 tool_result 行。
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
    int image_blocks = 0;
    int audio_blocks = 0;
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
    const auto foreground = ShapeOf("subagent-foreground");
    REQUIRE(foreground.has_value());
    bool has_agent_call = false;
    for (const auto& row : foreground->rows) {
        if (!row.contains("content") || !row["content"].is_array()) continue;
        for (const auto& block : row["content"]) {
            if (block.value("type", std::string()) == "tool_use" &&
                block.value("name", std::string()) == "agent") {
                has_agent_call = true;
            }
        }
    }
    CHECK(has_agent_call);
    // 旧格式没有 subagents/ 子账——这正是迁移须标 unavailable_legacy 的场景。

    const auto background = ShapeOf("subagent-background");
    REQUIRE(background.has_value());
    // queue 事件快照:一条 subagent 目标的排队账。
    std::size_t queue_events = 0;
    for (const auto& row : background->rows) {
        if (row.value("type", std::string()) != "queue" || !row.contains("items")) continue;
        queue_events += 1;
        REQUIRE(row["items"].is_array());
        REQUIRE(row["items"].size() == 1);
        CHECK(row["items"][0].value("target", std::string()) == "#3");
    }
    CHECK(queue_events == 1);
}

TEST_CASE("workspace 夹具:compact_v2 与 resume/title 事件") {
    const auto compact = ShapeOf("compact");
    REQUIRE(compact.has_value());
    CHECK(CountRows(*compact, "type", "compact_v2") == 1);
    // compact_v2 的 manifest 带 goal 字段(压缩守恒面)。
    for (const auto& row : compact->rows) {
        if (row.value("type", std::string()) != "compact_v2") continue;
        REQUIRE(row.contains("manifest"));
        CHECK(row["manifest"].contains("goal"));
        CHECK(row.value("epoch", 0) == 1);
    }

    const auto resumed = ShapeOf("resume");
    REQUIRE(resumed.has_value());
    CHECK(CountRows(*resumed, "type", "title") == 1);
    for (const auto& row : resumed->rows) {
        if (row.value("type", std::string()) == "title") {
            CHECK(row.value("title", std::string()) == "demo-repo 目录说明");
        }
    }
    CHECK(CountRows(*resumed, "role", "user") + CountRows(*resumed, "role", "assistant") == 4);
}

TEST_CASE("workspace 夹具:linked worktree 的 cwd 事件序列") {
    const auto linked = ShapeOf("linked-worktree");
    REQUIRE(linked.has_value());
    std::vector<std::string> cwds;
    for (const auto& row : linked->rows) {
        if (row.value("type", std::string()) == "cwd") {
            cwds.push_back(row.value("cwd", std::string()));
        }
    }
    REQUIRE(cwds.size() == 2);
    CHECK(cwds[0] == "C:/Users/sandbox/work/.wt/demo-repo-feature");  // 进 worktree 房
    CHECK(cwds[1] == "C:/Users/sandbox/work/demo-repo");              // 回主树
    // 消息行照旧可数(事件行不是坏行):三轮往返。
    CHECK(CountRows(*linked, "role", "user") + CountRows(*linked, "role", "assistant") == 6);
    CHECK(linked->bad_lines == 0);
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
