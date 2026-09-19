// 更新助手契约层·路径册(批一第①单)。用例对照三方真源:
//   - scripts/tests/updater_tests.py A 段(screen_member_name 的正反两排);
//   - scripts/install_plan.py valid_relpath(L71-89)九类拒绝;
//   - install_plan.py fold/CASE_FOLD(L63-68)的平台口径。
// 九类案:../ 穿越、绝对路径、盘符冒号、ADS 冒号、保留名(含大小写与
// stem 连扩展名)、结尾点/空格、反斜杠、保留名大小写变体、超长(>512)。
#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <string_view>

#include "updater/paths.hpp"

namespace {

bool Valid(std::string_view path) {
    return lubancode::updater::ValidRelpath(path);
}

std::optional<std::string> Member(std::string_view name) {
    return lubancode::updater::NormalizeMemberName(name);
}

bool UserData(std::string_view rel) {
    return lubancode::updater::IsUserDataPath(rel);
}

}  // namespace

TEST_CASE("ValidRelpath: 官方包内正常路径放行") {
    using lubancode::updater::ValidRelpath;
    CHECK(ValidRelpath(std::string_view("lubancode.exe")));
    CHECK(ValidRelpath(std::string_view("skills/lubancode-config/SKILL.md")));
    CHECK(ValidRelpath(std::string_view("docs/features/README.md")));
    CHECK(ValidRelpath(std::string_view("web/assistant/index.html")));
    CHECK(ValidRelpath(std::string_view("libexec/rg")));
    CHECK(ValidRelpath(std::string_view("updater/updater.py")));
    CHECK(ValidRelpath(std::string_view("README.en.md")));
    CHECK(ValidRelpath(std::string_view("LICENSE")));
    // UTF-8 段:字节 >= 0x80 不在非法字符域,放行(与 python codepoint 口径一致)。
    CHECK(ValidRelpath(std::string_view("docs/中文/说明.md")));
    // 中间空格合法:只有段尾空格才拒(install_plan.py 只查 endswith)。
    CHECK(ValidRelpath(std::string_view("docs/a b.md")));
    CHECK(Valid("a"));
    CHECK(Valid(std::string(512, 'a')));  // 恰 512 放行(>512 才拒)
}

TEST_CASE("ValidRelpath: ../ 与点段") {
    CHECK_FALSE(Valid("../evil.txt"));
    CHECK_FALSE(Valid("skills/../../evil"));
    CHECK_FALSE(Valid("a/./b"));
    CHECK_FALSE(Valid("./a"));
    CHECK_FALSE(Valid("a/.."));
    CHECK_FALSE(Valid("."));
    CHECK_FALSE(Valid(".."));
}

TEST_CASE("ValidRelpath: 绝对路径/反斜杠/冒号(盘符与 ADS)") {
    CHECK_FALSE(Valid("/abs.txt"));
    CHECK_FALSE(Valid("//a"));
    CHECK_FALSE(Valid("skills\\a.md"));
    CHECK_FALSE(Valid("\\\\server\\share"));
    CHECK_FALSE(Valid("C:/x"));
    CHECK_FALSE(Valid("c:"));
    CHECK_FALSE(Valid("a:b"));
    CHECK_FALSE(Valid("skills/a:b.txt"));
}

TEST_CASE("ValidRelpath: Windows 保留名(大小写不敏感,连扩展名)") {
    CHECK_FALSE(Valid("docs/CON"));
    CHECK_FALSE(Valid("con"));
    CHECK_FALSE(Valid("Con"));
    CHECK_FALSE(Valid("CON.txt"));
    CHECK_FALSE(Valid("com1.md"));
    CHECK_FALSE(Valid("LPT9.tar.gz"));
    CHECK_FALSE(Valid("aux"));
    CHECK_FALSE(Valid("nul.ini"));
    CHECK_FALSE(Valid("skills/prn.bin"));
    // 近似名不误伤:保留名是精确整段/stem,不是前缀。
    CHECK(Valid("console.log"));
    CHECK(Valid("contact.md"));
    CHECK(Valid("com10"));  // COM10 不是保留名(COM1-9 到头)
    CHECK(Valid("lpt"));
    CHECK(Valid("confidence/a.md"));
    CHECK(Valid("skills/con-x.md"));
}

TEST_CASE("ValidRelpath: 结尾点/空格") {
    CHECK_FALSE(Valid("skills/a.md."));
    CHECK_FALSE(Valid("a."));
    CHECK_FALSE(Valid("skills/a.md "));
    CHECK_FALSE(Valid("a/b c."));  // 段 "b c." 尾点
    CHECK(Valid("a/ b"));          // 段首空格合法,只有段尾才拒
}

TEST_CASE("ValidRelpath: 非法字符/控制字符/空段/超长") {
    CHECK_FALSE(Valid("a<b"));
    CHECK_FALSE(Valid("a>b"));
    CHECK_FALSE(Valid("a|b"));
    CHECK_FALSE(Valid("a?b"));
    CHECK_FALSE(Valid("a*b"));
    CHECK_FALSE(Valid("a\"b"));
    CHECK_FALSE(Valid("a\001b"));  // 八进制:控制字符 U+0001
    CHECK_FALSE(Valid(""));
    CHECK_FALSE(Valid("a//b"));  // 空段
    CHECK_FALSE(Valid("a/"));    // 尾斜杠即尾空段
    CHECK_FALSE(Valid(std::string(513, 'a')));
}

TEST_CASE("FoldKey: 平台口径(install_plan.py CASE_FOLD)") {
    using lubancode::updater::FoldKey;
    using lubancode::updater::kCaseFoldingActive;
    // 编译期口径:Windows/macOS 折叠,Linux 不折(接口签名平台无关)。
    if constexpr (kCaseFoldingActive) {
        CHECK(FoldKey(std::string_view("Skills/A.Md")) == "skills/a.md");
        CHECK(FoldKey(std::string_view("中文/文件.MD")) == "中文/文件.md");
        CHECK(FoldKey(std::string_view("LUBANCODE.EXE")) == "lubancode.exe");
    } else {
        CHECK(FoldKey(std::string_view("Skills/A.Md")) == "Skills/A.Md");
        CHECK(FoldKey(std::string_view("中文/文件.MD")) == "中文/文件.MD");
    }
    // ASCII 折叠口径:非 ASCII 字节原样(python casefold 的 Unicode 全折叠
    // 不跟,清单路径实测 ASCII 域两边逐字节一致——见 paths.hpp 合同注释)。
    CHECK(FoldKey(std::string_view("")) == "");
    // 键自洽:同一路径两折必同键。
    CHECK(FoldKey(std::string_view("Docs/X.md")) == FoldKey(std::string_view("Docs/X.md")));
}

TEST_CASE("NormalizeMemberName: 折叠归一(updater_tests.py A 段正向)") {
    REQUIRE(Member("a\\b.txt").has_value());
    CHECK(*Member("a\\b.txt") == "a/b.txt");
    REQUIRE(Member("./skills/a.md").has_value());
    CHECK(*Member("./skills/a.md") == "skills/a.md");
    REQUIRE(Member("skills//a.md").has_value());
    CHECK(*Member("skills//a.md") == "skills/a.md");
    REQUIRE(Member("skills/./a.md").has_value());
    CHECK(*Member("skills/./a.md") == "skills/a.md");
    REQUIRE(Member("skills/a/b.md").has_value());
    CHECK(*Member("skills/a/b.md") == "skills/a/b.md");
    REQUIRE(Member("a//./b").has_value());
    CHECK(*Member("a//./b") == "a/b");
    REQUIRE(Member(".env").has_value());
    CHECK(*Member(".env") == ".env");
}

TEST_CASE("NormalizeMemberName: 九类拒绝(A 段负向 + 契约边角)") {
    CHECK_FALSE(Member("../evil.txt").has_value());
    CHECK_FALSE(Member("skills/../../evil").has_value());
    CHECK_FALSE(Member("/abs.txt").has_value());
    CHECK_FALSE(Member("\\escape").has_value());  // 折成 //escape,绝对路径
    CHECK_FALSE(Member("C:/x").has_value());
    CHECK_FALSE(Member("a:b").has_value());
    CHECK_FALSE(Member("docs/CON").has_value());
    CHECK_FALSE(Member("skills/a.md.").has_value());
    CHECK_FALSE(Member("a/.../b").has_value());  // "..." 段尾点
    CHECK_FALSE(Member("a/../b").has_value());
    CHECK_FALSE(Member(std::string(513, 'a')).has_value());
    CHECK_FALSE(Member("").has_value());
    CHECK_FALSE(Member(".").has_value());
    CHECK_FALSE(Member("./").has_value());
    CHECK_FALSE(Member("/").has_value());
}

TEST_CASE("IsUserDataPath: 用户数据边界") {
    CHECK(UserData("config.toml"));
    CHECK(UserData(".env"));
    CHECK(UserData(".lubancode"));
    CHECK(UserData(".lubancode/sessions/main.jsonl"));
    CHECK(UserData(".lubancode/rg-stage/rg"));
    CHECK(UserData(".agents"));
    CHECK(UserData(".agents/researcher.md"));
    CHECK_FALSE(UserData("config.toml.bak"));
    CHECK_FALSE(UserData(".envx"));
    CHECK_FALSE(UserData(".env.backup"));  // 精确名,不是前缀
    CHECK_FALSE(UserData("skills/config.toml"));
    CHECK_FALSE(UserData("docs/.env"));
    CHECK_FALSE(UserData("lubancode"));
    CHECK_FALSE(UserData(".agent/x"));
    CHECK_FALSE(UserData(".agentsconfig/y"));
    CHECK_FALSE(UserData("skills/a.md"));
    CHECK_FALSE(UserData(""));
}
