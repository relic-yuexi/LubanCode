// schema 3 front matter 的格式测试:往返字节稳定、坑人字符(中文/冒号/
// 引号/Windows 路径/空数组/null)、双格式 reader 混读、正文水平线不误判。

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "memory/frontmatter.hpp"

using namespace lubancode;
namespace fm = memory::frontmatter;

namespace {

memory::MemoryEntry SampleEntry() {
    memory::MemoryEntry entry;
    entry.id = "preference.package-manager";
    entry.name = "package-manager";
    entry.kind = memory::MemoryKind::Preference;
    entry.title = "包管理器";
    entry.summary = "本项目一律用 pnpm";
    entry.keywords = {"pnpm", "install"};
    entry.paths = {"package.json"};
    entry.status = "active";
    entry.updated_at = "2026-08-16T05:24:16Z";
    entry.created_at = "2026-08-01T00:00:00Z";
    entry.source_sessions = {"0591791e-4362-4df1-be31-b92561e8a644"};
    entry.confidence = "user-stated";
    entry.scope.kind = "project";
    entry.scope.value = "";
    entry.last_verified_at = "2026-08-16T05:24:16Z";
    entry.evidence.push_back(memory::MemoryEvidence{"package.json", "packageManager"});
    entry.schema = 3;
    return entry;
}

}  // namespace

TEST_CASE("frontmatter: 往返字节稳定,两次 parse/write 一致") {
    memory::MemoryEntry entry = SampleEntry();
    const std::string body = "本项目一律用 pnpm,不跑 npm install。\n\n## Why\n\n用户明说,避免锁文件混用。\n";
    const std::string first = fm::BuildTopicText(entry, nlohmann::json::object(), body);
    auto parsed = fm::Parse(first);
    REQUIRE(parsed.has_value());
    const std::string second = fm::BuildTopicText(parsed->entry, parsed->fingerprints, parsed->body);
    CHECK(second == first);
}

TEST_CASE("frontmatter: 坑人字符与空集合的往返") {
    memory::MemoryEntry entry = SampleEntry();
    entry.summary = "路径 D:\\repo\\lubancode 与冒号: 引号\"单'、null、true、3.14";
    entry.keywords = {"含: 冒号", "#hash", "-dash", "中文关键词", "2026-08-16"};
    entry.paths.clear();
    entry.evidence.clear();
    entry.expires_at.clear();  // 空 = expires: null
    entry.source_sessions.clear();

    const std::string first = fm::BuildTopicText(entry, nlohmann::json::object(), "正文一段。");
    auto parsed_first = fm::Parse(first);
    REQUIRE(parsed_first.has_value());
    const std::string second =
        fm::BuildTopicText(parsed_first->entry, parsed_first->fingerprints, parsed_first->body);
    CHECK(second == first);
    auto parsed_second = fm::Parse(second);
    REQUIRE(parsed_second.has_value());
    const std::string third =
        fm::BuildTopicText(parsed_second->entry, parsed_second->fingerprints, parsed_second->body);
    CHECK(third == second);
}

TEST_CASE("frontmatter: 字段读写对位") {
    memory::MemoryEntry entry = SampleEntry();
    nlohmann::json fingerprints;
    fingerprints["src/app/version.hpp"] = "fnv1a64:0123456789abcdef";
    fingerprints["CMakeLists.txt"] = "fnv1a64:fedcba9876543210";
    const std::string text = fm::BuildTopicText(entry, fingerprints, "每合并一笔进 main，patch 位加一。\n\n## Why\n\n小步走，任一提交都可发版。\n");

    auto parsed = fm::Parse(text);
    REQUIRE(parsed.has_value());
    const memory::MemoryEntry& back = parsed->entry;
    CHECK(back.schema == 3);
    CHECK(back.id == entry.id);
    CHECK(back.name == entry.name);
    CHECK(back.kind == memory::MemoryKind::Preference);
    CHECK(back.title == "包管理器");
    CHECK(back.summary == entry.summary);
    CHECK(back.confidence == "user-stated");
    CHECK(back.status == "active");
    CHECK(back.scope.level == "project");
    CHECK(back.scope.kind == "project");
    CHECK(back.created_at == "2026-08-01T00:00:00Z");
    CHECK(back.updated_at == "2026-08-16T05:24:16Z");
    CHECK(back.last_verified_at == "2026-08-16T05:24:16Z");
    CHECK(back.expires_at.empty());
    CHECK(back.keywords == entry.keywords);
    CHECK(back.source_sessions == entry.source_sessions);
    REQUIRE(back.evidence.size() == 1);
    CHECK(back.evidence[0].path == "package.json");
    CHECK(back.evidence[0].symbol == "packageManager");
    CHECK(back.paths == std::vector<std::string>{"package.json"});
    CHECK(parsed->fingerprints == fingerprints);
    CHECK(parsed->body.find("## Why") != std::string::npos);
    CHECK(parsed->body.find("小步走") != std::string::npos);

    // 两次往返字节稳定。
    const std::string again = fm::BuildTopicText(back, parsed->fingerprints, parsed->body);
    CHECK(again == text);
}

TEST_CASE("frontmatter: expires 与 Windows 路径往返") {
    memory::MemoryEntry entry = SampleEntry();
    entry.expires_at = "2026-12-31";
    entry.evidence.push_back(memory::MemoryEvidence{"docs\\子目录 手册.md", ""});
    const std::string text = fm::BuildTopicText(entry, nlohmann::json::object(), "正文");
    auto parsed = fm::Parse(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->entry.expires_at == "2026-12-31");
    REQUIRE(parsed->entry.evidence.size() == 2);
    CHECK(parsed->entry.evidence[1].path == "docs\\子目录 手册.md");
    const std::string again = fm::BuildTopicText(parsed->entry, parsed->fingerprints, parsed->body);
    CHECK(again == text);
}

TEST_CASE("frontmatter: 正文水平线不算分隔线,只认开头第一对 ---") {
    memory::MemoryEntry entry = SampleEntry();
    const std::string body = "正文第一行。\n\n---\n\n这段在水平线之后,不该被当成 front matter 边界。\n";
    const std::string text = fm::BuildTopicText(entry, nlohmann::json::object(), body);
    auto parsed = fm::Parse(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->body.find("这段在水平线之后") != std::string::npos);
    CHECK(fm::StripTopicMetadata(text).find("这段在水平线之后") != std::string::npos);
}

TEST_CASE("frontmatter: 坏 YAML 与缺字段的报错") {
    CHECK_FALSE(fm::Parse("没有分隔线的正文").has_value());
    CHECK_FALSE(fm::Parse("---\nname: 只有一行没有闭合\n").has_value());
    CHECK_FALSE(fm::Parse("---\nname: bad\nmetadata:\n  schema: 2\n---\n").has_value());
    CHECK_FALSE(fm::Parse("---\n\tbad: [\n---\n").has_value());
    // 手写变体:合法 YAML 但缺必填字段。
    CHECK_FALSE(fm::Parse("---\nname: x\nmetadata:\n  schema: 3\n  type: 不认得\n---\n").has_value());
}

TEST_CASE("frontmatter: 手写 YAML 变体也能读(时间按字符串,不受隐式类型牵扯)") {
    const std::string text =
        "---\n"
        "name: hand-written\n"
        "description: \"手写的主题,带 引号 与冒号:\"\n"
        "metadata:\n"
        "  schema: 3\n"
        "  node_type: memory\n"
        "  type: fact\n"
        "  id: fact.hand-written\n"
        "  confidence: verified\n"
        "  status: active\n"
        "  scope: {level: project, kind: path, value: src/loop.cpp}\n"
        "  origin_session_ids: []\n"
        "  created: 2026-08-16\n"
        "  modified: 2026-08-16T05:24:16Z\n"
        "  last_verified: 2026-08-16T05:24:16Z\n"
        "  expires:\n"
        "  keywords: []\n"
        "  evidence:\n"
        "    - {path: src/loop.cpp, symbol: kVersion}\n"
        "---\n\n# 手写主题\n\n正文。\n";
    auto parsed = fm::Parse(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->entry.id == "fact.hand-written");
    CHECK(parsed->entry.title == "手写主题");
    CHECK(parsed->entry.summary == "手写的主题,带 引号 与冒号:");
    CHECK(parsed->entry.created_at == "2026-08-16");
    CHECK(parsed->entry.expires_at.empty());
    CHECK(parsed->entry.scope.kind == "path");
    CHECK(parsed->entry.scope.value == "src/loop.cpp");
    REQUIRE(parsed->entry.evidence.size() == 1);
    CHECK(parsed->entry.evidence[0].symbol == "kVersion");
    // 标题已从正文剥掉,正文只剩内容。
    CHECK(parsed->body == "正文。");
}

TEST_CASE("frontmatter: occurred_at 往返与旧条目兼容") {
    // 带日期:写出成 occurred_at: <date>,读回一致,两次往返字节稳定。
    memory::MemoryEntry entry = SampleEntry();
    entry.occurred_at = "2023-05-08";
    const std::string text = fm::BuildTopicText(entry, nlohmann::json::object(), "正文。");
    CHECK(text.find("occurred_at: 2023-05-08\n") != std::string::npos);
    auto parsed = fm::Parse(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->entry.occurred_at == "2023-05-08");
    const std::string again = fm::BuildTopicText(parsed->entry, parsed->fingerprints, parsed->body);
    CHECK(again == text);

    // 空 = occurred_at: ~(yaml-cpp 的 Null 定型),读回为空。
    memory::MemoryEntry bare = SampleEntry();
    const std::string bare_text = fm::BuildTopicText(bare, nlohmann::json::object(), "正文。");
    CHECK(bare_text.find("occurred_at: ~\n") != std::string::npos);
    auto bare_parsed = fm::Parse(bare_text);
    REQUIRE(bare_parsed.has_value());
    CHECK(bare_parsed->entry.occurred_at.empty());

    // 旧条目手写 YAML 没有 occurred_at 键:照常可读,读入为空不炸。
    const std::string legacy =
        "---\n"
        "name: old-timeline\n"
        "description: 老主题\n"
        "metadata:\n"
        "  schema: 3\n"
        "  node_type: memory\n"
        "  type: fact\n"
        "  id: fact.old-timeline\n"
        "  confidence: verified\n"
        "  status: active\n"
        "  scope: {level: project, kind: project, value: \"\"}\n"
        "  origin_session_ids: []\n"
        "  created: 2026-08-16\n"
        "  modified: 2026-08-16\n"
        "  last_verified: 2026-08-16\n"
        "  expires: null\n"
        "  keywords: []\n"
        "  evidence: []\n"
        "---\n\n# 老主题\n\n老正文。\n";
    auto old = fm::Parse(legacy);
    REQUIRE(old.has_value());
    CHECK(old->entry.occurred_at.empty());

    // 形状不像日期的值按空处理(不造假):相对时间、口语时间都进不来。
    const std::string sloppy =
        "---\n"
        "name: sloppy-date\n"
        "description: 草率日期\n"
        "metadata:\n"
        "  schema: 3\n"
        "  node_type: memory\n"
        "  type: fact\n"
        "  id: fact.sloppy-date\n"
        "  confidence: verified\n"
        "  status: active\n"
        "  scope: {level: project, kind: project, value: \"\"}\n"
        "  origin_session_ids: []\n"
        "  created: 2026-08-16\n"
        "  modified: 2026-08-16\n"
        "  last_verified: 2026-08-16\n"
        "  expires: null\n"
        "  occurred_at: 上周三\n"
        "  keywords: []\n"
        "  evidence: []\n"
        "---\n\n# 草率日期\n\n正文。\n";
    auto bad = fm::Parse(sloppy);
    REQUIRE(bad.has_value());
    CHECK(bad->entry.occurred_at.empty());
}

TEST_CASE("frontmatter: 老格式(schema 1/2)HTML 注释的剥壳") {
    const std::string legacy =
        "<!-- lubancode-memory\n"
        "{\"schema\":2,\"id\":\"fact.a\",\"kind\":\"fact\",\"title\":\"A\"}\n"
        "-->\n\n# A\n\n正文。\n";
    const std::string stripped = fm::StripTopicMetadata(legacy);
    CHECK(stripped.find("# A") == 0);
    CHECK(stripped.find(" lubancode-memory") == std::string::npos);
}
