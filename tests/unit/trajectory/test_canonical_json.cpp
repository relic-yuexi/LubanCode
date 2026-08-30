// Canonical JSON(§8.1)字节钉板测试:键序、无空白、UTF-8、NaN/Inf 拒绝。
// 这些字节就是 hash chain 的输入,改一个字节等于改链——本册存在的意义
// 就是让任何序列化改动立刻炸出来。
#include <doctest/doctest.h>

#include <limits>
#include <string>

#include <nlohmann/json.hpp>

#include "trajectory/canonical_json.hpp"

using lubancode::trajectory::CanonicalJsonDump;
using lubancode::trajectory::IsValidUtf8;

TEST_CASE("canonical: 键按 UTF-8 字节序递归排序,与输入序无关") {
    const nlohmann::json a = nlohmann::json::parse(R"({"b":1,"a":2})");
    const nlohmann::json b = nlohmann::json::parse(R"({"a":2,"b":1})");
    const auto da = CanonicalJsonDump(a);
    const auto db = CanonicalJsonDump(b);
    REQUIRE(da.has_value());
    REQUIRE(db.has_value());
    CHECK(*da == R"({"a":2,"b":1})");
    CHECK(*da == *db);  // 语义同即字节同(§8.1)

    const nlohmann::json nested = nlohmann::json::parse(
        R"({"z":{"y":2,"x":1},"a":[{"q":0,"b":1}]})");
    const auto dn = CanonicalJsonDump(nested);
    REQUIRE(dn.has_value());
    CHECK(*dn == R"({"a":[{"b":1,"q":0}],"z":{"x":1,"y":2}})");
}

TEST_CASE("canonical: 中文键按 UTF-8 字节序,不是码点序也不是本地序") {
    // "一" U+4E00 -> E4 B8 80;"万" U+4E07 -> E4 B8 87。字节序与码点序
    // 在 BMP 内一致;这里钉的是"按字节、跨平台一致"。
    const nlohmann::json json = nlohmann::json::parse(R"({"一":1,"万":2})");
    const auto dumped = CanonicalJsonDump(json);
    REQUIRE(dumped.has_value());
    CHECK(*dumped == "{\"一\":1,\"万\":2}");
    CHECK(IsValidUtf8(*dumped));
}

TEST_CASE("canonical: 无空白;数值与布尔照常") {
    const nlohmann::json json = nlohmann::json::parse(
        R"( {"i": -3, "u": 5, "f": 1.5, "t": true, "n": null, "s": "x"} )");
    const auto dumped = CanonicalJsonDump(json);
    REQUIRE(dumped.has_value());
    CHECK(*dumped == R"({"f":1.5,"i":-3,"n":null,"s":"x","t":true,"u":5})");
}

TEST_CASE("canonical: 字符串转义钉字节") {
    const nlohmann::json json = nlohmann::json::parse(
        R"({"quote":"a\"b","ctrl":"\u0000\u001f","nl":"\n"})");
    const auto dumped = CanonicalJsonDump(json);
    REQUIRE(dumped.has_value());
    CHECK(*dumped == "{\"ctrl\":\"\\u0000\\u001f\",\"nl\":\"\\n\",\"quote\":\"a\\\"b\"}");
}

TEST_CASE("canonical: 拒绝 NaN/Inf") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    CHECK_FALSE(CanonicalJsonDump(nlohmann::json(nan)).has_value());
    CHECK_FALSE(CanonicalJsonDump(nlohmann::json(inf)).has_value());
    CHECK_FALSE(CanonicalJsonDump(nlohmann::json(-inf)).has_value());
}

TEST_CASE("canonical: 拒绝非法 UTF-8") {
    const std::string bad = std::string("a") + static_cast<char>(0xFF) + "b";
    CHECK(IsValidUtf8("hello 你好"));
    CHECK_FALSE(IsValidUtf8(bad));
    CHECK_FALSE(IsValidUtf8(std::string("\xE4\xB8")));       // 截断的三字节序列
    CHECK_FALSE(IsValidUtf8(std::string("\xC0\x80")));       // 过长编码 NUL
    CHECK_FALSE(IsValidUtf8(std::string("\xED\xA0\x80")));   // 代理区半枚
    nlohmann::json json = nlohmann::json::array();
    json.push_back(nlohmann::json(bad));
    CHECK_FALSE(CanonicalJsonDump(json).has_value());
}

TEST_CASE("canonical: 浮点走确定性表示,同一值两次 dump 同字节") {
    const nlohmann::json json = nlohmann::json::parse(R"({"v":0.1,"w":1e30})");
    const auto first = CanonicalJsonDump(json);
    const auto second = CanonicalJsonDump(nlohmann::json::parse(R"({"w":1e30,"v":0.1})"));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == *second);
}

TEST_CASE("canonical: 空容器与空串") {
    const auto obj = CanonicalJsonDump(nlohmann::json::parse("{}"));
    const auto arr = CanonicalJsonDump(nlohmann::json::parse("[]"));
    REQUIRE(obj.has_value());
    REQUIRE(arr.has_value());
    CHECK(*obj == "{}");
    CHECK(*arr == "[]");
}
