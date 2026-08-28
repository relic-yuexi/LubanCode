// 统一 Package 封装单阶段 1:SemVer 解析/比较/范围 与 package.yaml 严格
// 解析的单测。全离线,纯文本进纯结果出。
//
// 测试账对齐单子 §十六"Package 清单与盘点":最小/完整清单、未知字段、
// 错类型、错 schema、id 非法、version 非 SemVer、版本范围非法与边界。

#include <doctest/doctest.h>

#include "package/manifest.hpp"
#include "package/semver.hpp"

using namespace lubancode::package;

// ---------------------------------------------------------------------------
// SemVer 解析
// ---------------------------------------------------------------------------

TEST_CASE("SemVer.合法版本") {
    CHECK(ParseSemVer("0.1.0").has_value());
    CHECK(ParseSemVer("1.2.3").has_value());
    CHECK(ParseSemVer("10.20.30").has_value());
    CHECK(ParseSemVer("1.0.0-alpha").has_value());
    CHECK(ParseSemVer("1.0.0-alpha.1").has_value());
    CHECK(ParseSemVer("1.0.0-0.3.7").has_value());
    CHECK(ParseSemVer("1.0.0-x.7.z.92").has_value());
    CHECK(ParseSemVer("1.0.0+build.1").has_value());
    CHECK(ParseSemVer("1.0.0-alpha+001").has_value());
    CHECK(ParseSemVer(" 0.1.0 ").has_value());  // 两端空白剥掉

    const auto v = ParseSemVer("1.2.3-rc.1+build.5");
    REQUIRE(v.has_value());
    CHECK(v->major == 1);
    CHECK(v->minor == 2);
    CHECK(v->patch == 3);
    CHECK(v->prerelease == "rc.1");
    CHECK(v->build == "build.5");
    CHECK(v->text == "1.2.3-rc.1+build.5");
}

TEST_CASE("SemVer.非法版本") {
    CHECK_FALSE(ParseSemVer("").has_value());
    CHECK_FALSE(ParseSemVer("1").has_value());
    CHECK_FALSE(ParseSemVer("1.2").has_value());
    CHECK_FALSE(ParseSemVer("1.2.x").has_value());
    CHECK_FALSE(ParseSemVer("01.2.3").has_value());   // 前导零
    CHECK_FALSE(ParseSemVer("1.02.3").has_value());
    CHECK_FALSE(ParseSemVer("1.2.03").has_value());
    CHECK_FALSE(ParseSemVer("v1.2.3").has_value());   // v 前缀不认
    CHECK_FALSE(ParseSemVer("1.2.3-").has_value());   // 空预发布
    CHECK_FALSE(ParseSemVer("1.2.3+").has_value());   // 空构建
    CHECK_FALSE(ParseSemVer("1.2.3-01").has_value());  // 预发布数字段前导零
    CHECK_FALSE(ParseSemVer("1.2.3-alpha..1").has_value());
    CHECK_FALSE(ParseSemVer("1.2.3-alpha+meta+more").has_value());
    CHECK_FALSE(ParseSemVer("1.2.3.4").has_value());
    CHECK_FALSE(ParseSemVer("1.2-rc").has_value());
}

TEST_CASE("SemVer.比较语义") {
    const auto parse = [](const char* s) {
        const auto v = ParseSemVer(s);
        REQUIRE(v.has_value());
        return *v;
    };
    // semver.org 的优先级示例逐条钉。
    CHECK(CompareSemVer(parse("1.0.0"), parse("2.0.0")) < 0);
    CHECK(CompareSemVer(parse("2.0.0"), parse("2.1.0")) < 0);
    CHECK(CompareSemVer(parse("2.1.0"), parse("2.1.1")) < 0);
    CHECK(CompareSemVer(parse("1.0.0-alpha"), parse("1.0.0")) < 0);
    CHECK(CompareSemVer(parse("1.0.0-alpha"), parse("1.0.0-alpha.1")) < 0);
    CHECK(CompareSemVer(parse("1.0.0-alpha.1"), parse("1.0.0-alpha.beta")) < 0);
    CHECK(CompareSemVer(parse("1.0.0-alpha.beta"), parse("1.0.0-beta")) < 0);
    CHECK(CompareSemVer(parse("1.0.0-beta"), parse("1.0.0-beta.2")) < 0);
    CHECK(CompareSemVer(parse("1.0.0-beta.2"), parse("1.0.0-beta.11")) < 0);
    CHECK(CompareSemVer(parse("1.0.0-beta.11"), parse("1.0.0-rc.1")) < 0);
    CHECK(CompareSemVer(parse("1.0.0-rc.1"), parse("1.0.0")) < 0);
    // 构建元数据不参与比较。
    CHECK(CompareSemVer(parse("1.0.0+one"), parse("1.0.0+two")) == 0);
    CHECK(CompareSemVer(parse("1.0.0-alpha.1+001"), parse("1.0.0-alpha.1+002")) == 0);
}

// ---------------------------------------------------------------------------
// 版本范围
// ---------------------------------------------------------------------------

TEST_CASE("范围.解析与满足") {
    const auto range = ParseVersionRange(">=0.27.0 <0.28.0");
    REQUIRE(range.has_value());
    REQUIRE(range->parts.size() == 2);

    const auto satisfies = [range](const char* v) {
        const auto version = ParseSemVer(v);
        REQUIRE(version.has_value());
        return VersionSatisfies(*version, *range);
    };
    CHECK(satisfies("0.27.0"));    // 下界含
    CHECK(satisfies("0.27.5"));
    CHECK_FALSE(satisfies("0.26.9"));
    CHECK_FALSE(satisfies("0.28.0"));  // 上界不含
    CHECK_FALSE(satisfies("0.27.0-rc.1"));  // 预发布不冒充正式版
}

TEST_CASE("范围.算子与裸版本") {
    const auto check = [](const char* range_text, const char* version, bool expected) {
        const auto range = ParseVersionRange(range_text);
        REQUIRE(range.has_value());
        const auto v = ParseSemVer(version);
        REQUIRE(v.has_value());
        const std::string note = std::string(range_text) + " vs " + version +
                                 (expected ? " 应满足" : " 应不满足");
        CHECK_MESSAGE(VersionSatisfies(*v, *range) == expected, note.c_str());
    };
    check(">0.26.0", "0.26.0", false);
    check(">0.26.0", "0.26.1", true);
    check("<=0.26.76", "0.26.76", true);
    check("<=0.26.76", "0.26.77", false);
    check("=0.27.0", "0.27.0", true);
    check("=0.27.0", "0.27.1", false);
    check("0.27.0", "0.27.0", true);   // 裸版本 = 精确匹配
    check("0.27.0", "0.27.1", false);
    check(">=0.27.0", "0.27.0+build", true);  // 构建元数据不参与
}

TEST_CASE("范围.非法") {
    CHECK_FALSE(ParseVersionRange("").has_value());
    CHECK_FALSE(ParseVersionRange("  ").has_value());
    CHECK_FALSE(ParseVersionRange(">=junk").has_value());
    CHECK_FALSE(ParseVersionRange(">=1.0").has_value());
    CHECK_FALSE(ParseVersionRange("~=1.0.0").has_value());  // 首版不认的算子
}

// ---------------------------------------------------------------------------
// package.yaml 严格解析
// ---------------------------------------------------------------------------

TEST_CASE("清单.最小合法") {
    const auto parsed = ParsePackageManifest(R"yaml(
schema: 1
id: moontide.browser-suite
version: 0.1.0
name: Browser Suite
description: Browser inspection package.
)yaml");
    REQUIRE(parsed.has_value());
    CHECK(parsed->schema == 1);
    CHECK(parsed->id == "moontide.browser-suite");
    CHECK(parsed->version.text == "0.1.0");
    CHECK(parsed->name == "Browser Suite");
    CHECK(parsed->description == "Browser inspection package.");
    CHECK(parsed->authors.empty());
    CHECK(parsed->license.empty());
    CHECK_FALSE(parsed->compatibility_lubancode.has_value());
    CHECK(parsed->compatibility_platforms.empty());
}

TEST_CASE("清单.完整合法") {
    const auto parsed = ParsePackageManifest(R"yaml(
schema: 1
id: moontide.browser-suite
version: 0.1.0
name: Browser Suite
description: Browser inspection agents, skills, workflows, plugins, and MCP tools.

authors:
  - name: Moontide
    url: https://example.com

license: Apache-2.0
homepage: https://example.com/browser-suite
repository: https://example.com/browser-suite.git

compatibility:
  lubancode: ">=0.27.0 <0.28.0"
  platforms:
    - windows
    - linux
    - macos
)yaml");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->authors.size() == 1);
    CHECK(parsed->authors[0].name == "Moontide");
    CHECK(parsed->authors[0].url == "https://example.com");
    CHECK(parsed->license == "Apache-2.0");
    REQUIRE(parsed->compatibility_lubancode.has_value());
    CHECK(parsed->compatibility_lubancode->text == ">=0.27.0 <0.28.0");
    REQUIRE(parsed->compatibility_platforms.size() == 3);
}

TEST_CASE("清单.缺必填指到字段") {
    const auto missing = [](const char* yaml, const char* field) {
        const auto parsed = ParsePackageManifest(yaml);
        if (parsed.has_value()) {
            FAIL("本该解析失败");
            return;
        }
        const std::string note = std::string("错误应指到字段 ") + field;
        CHECK_MESSAGE(parsed.error().field == field, note.c_str());
    };
    missing("id: a.b\nversion: 0.1.0\nname: n\ndescription: d", "schema");
    missing("schema: 1\nversion: 0.1.0\nname: n\ndescription: d", "id");
    missing("schema: 1\nid: a.b\nname: n\ndescription: d", "version");
    missing("schema: 1\nid: a.b\nversion: 0.1.0\ndescription: d", "name");
    missing("schema: 1\nid: a.b\nversion: 0.1.0\nname: n", "description");
}

TEST_CASE("清单.错schema与类型错") {
    auto parsed = ParsePackageManifest("schema: 2\nid: a.b\nversion: 0.1.0\nname: n\ndescription: d");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().field == "schema");

    parsed = ParsePackageManifest("schema: 1\nid: [a, b]\nversion: 0.1.0\nname: n\ndescription: d");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().field == "id");

    parsed = ParsePackageManifest("schema: 1\nid: a.b\nversion: [1]\nname: n\ndescription: d");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().field == "version");

    // version 类型对但不是 SemVer:指到 version。
    parsed = ParsePackageManifest("schema: 1\nid: a.b\nversion: 0.1\nname: n\ndescription: d");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().field == "version");

    // authors 给成标量:指到 authors。
    parsed = ParsePackageManifest(
        "schema: 1\nid: a.b\nversion: 0.1.0\nname: n\ndescription: d\nauthors: moontide");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().field == "authors");

    // author 元素缺 name:指到 authors[0].name。
    parsed = ParsePackageManifest(
        "schema: 1\nid: a.b\nversion: 0.1.0\nname: n\ndescription: d\nauthors:\n  - url: x");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().field == "authors[0].name");
}

TEST_CASE("清单.未知字段") {
    auto parsed = ParsePackageManifest(
        "schema: 1\nid: a.b\nversion: 0.1.0\nname: n\ndescription: d\npermissions: {network: true}");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().field == "permissions");  // 清单不许替组件发号施令

    parsed = ParsePackageManifest(
        "schema: 1\nid: a.b\nversion: 0.1.0\nname: n\ndescription: d\ncompatibility:\n  os: windows");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().field == "compatibility.os");

    parsed = ParsePackageManifest(
        "schema: 1\nid: a.b\nversion: 0.1.0\nname: n\ndescription: d\nskills:\n  - x");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().field == "skills");  // 逐文件清单不进根清单
}

TEST_CASE("清单.id字符规矩") {
    const auto ok = [](const char* id) {
        return ParsePackageManifest(std::string("schema: 1\nid: ") + id +
                                    "\nversion: 0.1.0\nname: n\ndescription: d")
            .has_value();
    };
    CHECK(ok("a"));
    CHECK(ok("moontide.browser-suite"));
    CHECK(ok("a1.b-2.c3"));
    CHECK_FALSE(ok(".a.b"));    // 首点
    CHECK_FALSE(ok("a.b."));    // 尾点
    CHECK_FALSE(ok("-a.b"));    // 首连字符
    CHECK_FALSE(ok("a.b-"));    // 尾连字符
    CHECK_FALSE(ok("A.b"));     // 大写
    CHECK_FALSE(ok("a b"));     // 空白
    CHECK_FALSE(ok("a_b"));     // 下划线
    CHECK_FALSE(ok("中文.包"));  // 非 ASCII
}

TEST_CASE("清单.平台与范围非法值") {
    auto parsed = ParsePackageManifest(
        "schema: 1\nid: a.b\nversion: 0.1.0\nname: n\ndescription: d\ncompatibility:\n  platforms:\n"
        "    - windows95");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().field == "compatibility.platforms[0]");

    parsed = ParsePackageManifest(
        "schema: 1\nid: a.b\nversion: 0.1.0\nname: n\ndescription: d\ncompatibility:\n  lubancode: "
        "\"~>0.27\"");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().field == "compatibility.lubancode");
}

TEST_CASE("清单.语法错与空文件") {
    auto parsed = ParsePackageManifest("id: [unclosed");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().field == "(yaml)");

    parsed = ParsePackageManifest("");
    REQUIRE_FALSE(parsed.has_value());

    parsed = ParsePackageManifest("- just\n- a\n- list\n");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().field == "(yaml)");
}

TEST_CASE("清单.错误格式带行号") {
    const auto parsed = ParsePackageManifest("schema: 1\nid: a.b\nversion: 0.1.0\nname: n\nextra: 1");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().line == 5);  // 未知字段在第 5 行
    const std::string text = parsed.error().Format();
    CHECK(text.find("package.yaml:5") != std::string::npos);
    CHECK(text.find("extra") != std::string::npos);
}
