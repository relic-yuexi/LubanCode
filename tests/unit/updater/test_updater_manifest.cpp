// 更新助手契约层·清单册(批一第①单)。覆盖:
//   - CanonicalJsonDump 与 python `json.dumps(obj, ensure_ascii=True,
//     sort_keys=True, indent=2) + "\n"`(install_plan.py dump_json/
//     generate_manifest.py dump)的逐字节等价:嵌套/排序/中文转义/代理对/
//     0x7F/控制字符;
//   - ParseManifestText 的读/验/折:schema 1 字段口径对照
//     generate_manifest.py validate_manifest,坏 JSON/缺字段/字段不对/
//     路径不合法/大小写碰撞(按 FoldKey 平台口径)各拒;
//   - utf-8-sig 容错:剥 BOM。
#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "updater/manifest.hpp"
#include "updater/paths.hpp"

namespace {

using lubancode::updater::CanonicalJsonDump;
using lubancode::updater::ParseManifestText;

std::string Sha64(char fill) {
    return std::string(64, fill);
}

nlohmann::json MakeEntry(const std::string& path, std::int64_t size, const std::string& sha) {
    return {{"path", path}, {"size", size}, {"sha256", sha}};
}

nlohmann::json MakeManifest(const std::vector<nlohmann::json>& files,
                            std::optional<std::int64_t> file_count = std::nullopt) {
    nlohmann::json m = {{"schema", 1},
                        {"name", "lubancode"},
                        {"version", "1.2.3"},
                        {"platform", "test-x64"},
                        {"algo", "sha256"},
                        {"files", files}};
    m["file_count"] = file_count.has_value() ? *file_count
                                             : static_cast<std::int64_t>(files.size());
    return m;
}

std::optional<lubancode::updater::Manifest> ParseOk(const std::string& text) {
    std::vector<std::string> problems;
    auto manifest = ParseManifestText(text, &problems);
    if (problems.empty()) {
        CHECK(manifest.has_value());
    } else {
        // doctest 的 *_MESSAGE 变参只吃 const char* 一类(MessageBuilder* 技巧),
        // std::string 没有 operator* 重载,须先 .c_str()。
        const std::string why = "应通过却拒了:" + problems.front();
        CHECK_MESSAGE(false, why.c_str());
    }
    return manifest;
}

std::vector<std::string> ParseRejects(const std::string& text) {
    std::vector<std::string> problems;
    const auto manifest = ParseManifestText(text, &problems);
    CHECK_FALSE(manifest.has_value());
    CHECK_FALSE(problems.empty());
    return problems;
}

}  // namespace

// ---------------------------------------------------------------- dump ---

TEST_CASE("CanonicalJsonDump: 简单对象与键排序逐字节") {
    nlohmann::json j = {{"a", 1}};
    CHECK(CanonicalJsonDump(j) == "{\n  \"a\": 1\n}\n");

    // 输入乱序,输出必须按字典序(sort_keys=True)。
    j = {{"b", 1}, {"a", 2}, {"c", 3}};
    CHECK(CanonicalJsonDump(j) == "{\n  \"a\": 2,\n  \"b\": 1,\n  \"c\": 3\n}\n");
}

TEST_CASE("CanonicalJsonDump: 嵌套对象与数组缩进逐字节") {
    nlohmann::json j = {{"k", {{"n", {1, 2}}}}};
    CHECK(CanonicalJsonDump(j) == "{\n  \"k\": {\n    \"n\": [\n      1,\n      2\n    ]\n  }\n}\n");

    j = nlohmann::json::object();
    CHECK(CanonicalJsonDump(j) == "{}\n");

    j = {{"a", nlohmann::json::array()}};
    CHECK(CanonicalJsonDump(j) == "{\n  \"a\": []\n}\n");

    j = {{"a", nlohmann::json::object()}};
    CHECK(CanonicalJsonDump(j) == "{\n  \"a\": {}\n}\n");

    j = {{"t", true}, {"n", nullptr}};
    CHECK(CanonicalJsonDump(j) == "{\n  \"n\": null,\n  \"t\": true\n}\n");
}

TEST_CASE("CanonicalJsonDump: 中文与非 ASCII 转义(ensure_ascii 口径)") {
    // 中 U+4E2D / 文 U+6587:python json.dumps 输出 \u4e2d\u6587(小写)。
    nlohmann::json j = {{"k", "中文"}};
    CHECK(CanonicalJsonDump(j) == "{\n  \"k\": \"\\u4e2d\\u6587\"\n}\n");

    // BMP 外拆 UTF-16 代理对(python 同款:\ud83d\ude00)。
    j = {{"e", "\xF0\x9F\x98\x80"}};  // U+1F600
    CHECK(CanonicalJsonDump(j) == "{\n  \"e\": \"\\ud83d\\ude00\"\n}\n");

    // 0x7F 起 ensure_ascii 全转义(python ESCAPE_ASCII 的 [^\ -~] 口径)。
    j = {{"x", "\x7f"}};
    CHECK(CanonicalJsonDump(j) == "{\n  \"x\": \"\\u007f\"\n}\n");
}

TEST_CASE("CanonicalJsonDump: 控制字符与引号反斜杠转义") {
    // 八进制 \001 避免 \x 十六进制吞噬;期望:短转义 + \uXXXX 小写十六进制。
    const std::string payload = std::string("a\001") + "b\"c\\d\ne\tf";
    nlohmann::json j = {{"s", payload}};
    CHECK(CanonicalJsonDump(j) ==
          "{\n  \"s\": \"a\\u0001b\\\"c\\\\d\\ne\\tf\"\n}\n");

    // dump 产物是纯 ASCII(0x7F 以下可打印 + 转义),谁读都稳。
    const std::string dumped = CanonicalJsonDump(j);
    for (const char c : dumped) {
        CHECK(static_cast<unsigned char>(c) < 0x80);
    }
}

// -------------------------------------------------------------- parse ---

TEST_CASE("ParseManifestText: 合格清单读验折成 map") {
    const std::string text = CanonicalJsonDump(MakeManifest(
        {MakeEntry("skills/lubancode-config/SKILL.md", 123, Sha64('a')),
         MakeEntry("docs/README.md", 45, Sha64('0'))}));

    const auto manifest = ParseOk(text);
    REQUIRE(manifest.has_value());
    REQUIRE(manifest->files.size() == 2);
    CHECK(manifest->file_count == 2);

    // 键是 FoldKey(path),值里的 path 留原始大小写(给人看与落盘)。
    const auto it = manifest->files.find(lubancode::updater::FoldKey("skills/lubancode-config/SKILL.md"));
    REQUIRE(it != manifest->files.end());
    CHECK(it->second.path == "skills/lubancode-config/SKILL.md");
    CHECK(it->second.size == 123);
    CHECK(it->second.sha256 == Sha64('a'));
    CHECK(manifest->files.count(lubancode::updater::FoldKey("docs/README.md")) == 1);
}

TEST_CASE("ParseManifestText: utf-8-sig 容错剥 BOM") {
    const std::string body = CanonicalJsonDump(MakeManifest({MakeEntry("LICENSE", 4, Sha64('f'))}));
    const auto plain = ParseOk(body);
    const auto with_bom = ParseOk("\xEF\xBB\xBF" + body);
    REQUIRE(plain.has_value());
    REQUIRE(with_bom.has_value());
    CHECK(with_bom->files.size() == plain->files.size());
    CHECK(with_bom->file_count == 1);
}

TEST_CASE("ParseManifestText: 坏 JSON 与非对象拒") {
    ParseRejects("{ not json");
    ParseRejects("[1, 2]");
    ParseRejects("\"just a string\"");
    ParseRejects("");
}

TEST_CASE("ParseManifestText: schema/algo/files 缺或不对拒") {
    nlohmann::json m = MakeManifest({MakeEntry("LICENSE", 4, Sha64('f'))});

    nlohmann::json bad = m;
    bad["schema"] = 2;
    ParseRejects(CanonicalJsonDump(bad));

    bad = m;
    bad.erase("schema");
    ParseRejects(CanonicalJsonDump(bad));

    bad = m;
    bad["algo"] = "md5";
    ParseRejects(CanonicalJsonDump(bad));

    bad = m;
    bad.erase("algo");
    ParseRejects(CanonicalJsonDump(bad));

    bad = m;
    bad.erase("files");
    ParseRejects(CanonicalJsonDump(bad));

    bad = m;
    bad["files"] = nlohmann::json::object();
    ParseRejects(CanonicalJsonDump(bad));
}

TEST_CASE("ParseManifestText: 条目字段不合格拒") {
    const auto with_files = [](const std::vector<nlohmann::json>& files) {
        return CanonicalJsonDump(MakeManifest(files));
    };

    nlohmann::json entry = MakeEntry("LICENSE", 4, Sha64('f'));

    nlohmann::json bad = entry;
    bad["path"] = 42;
    ParseRejects(with_files({bad}));

    bad = entry;
    bad.erase("path");
    ParseRejects(with_files({bad}));

    ParseRejects(with_files({MakeEntry("../evil.txt", 4, Sha64('f'))}));

    bad = entry;
    bad.erase("size");
    ParseRejects(with_files({bad}));

    bad = entry;
    bad["size"] = -1;
    ParseRejects(with_files({bad}));

    bad = entry;
    bad["size"] = "4";
    ParseRejects(with_files({bad}));

    bad = entry;
    bad.erase("sha256");
    ParseRejects(with_files({bad}));

    bad = entry;
    bad["sha256"] = Sha64('A');  // 大写拒
    ParseRejects(with_files({bad}));

    bad = entry;
    bad["sha256"] = Sha64('f').substr(1);  // 63 位拒
    ParseRejects(with_files({bad}));

    bad = entry;
    bad["sha256"] = Sha64('g');  // 非十六进制拒
    ParseRejects(with_files({bad}));

    ParseRejects(with_files({42}));  // 成员不是对象
}

TEST_CASE("ParseManifestText: file_count 与 files 长度不合拒") {
    const std::string ok_text =
        CanonicalJsonDump(MakeManifest({MakeEntry("LICENSE", 4, Sha64('f'))}));
    REQUIRE(ParseOk(ok_text).has_value());

    nlohmann::json m = MakeManifest({MakeEntry("LICENSE", 4, Sha64('f'))});
    m["file_count"] = 3;
    ParseRejects(CanonicalJsonDump(m));

    m.erase("file_count");
    ParseRejects(CanonicalJsonDump(m));
}

TEST_CASE("ParseManifestText: 大小写碰撞按 FoldKey 平台口径") {
    const std::string text = CanonicalJsonDump(MakeManifest(
        {MakeEntry("docs/A.md", 1, Sha64('a')), MakeEntry("docs/a.md", 2, Sha64('0'))}));
    if constexpr (lubancode::updater::kCaseFoldingActive) {
        // Windows/macOS 盘面大小写不敏感:折叠后撞键,拒(install_plan.py 读侧)。
        ParseRejects(text);
    } else {
        // Linux 盘面大小写敏感:两个键各安其位,不算撞。
        const auto manifest = ParseOk(text);
        REQUIRE(manifest.has_value());
        CHECK(manifest->files.size() == 2);
    }
}
