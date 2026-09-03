// 路径比较键共享向量(src 收口审计 P2 候选:isolation/worktree/
// session_utils 的 NormalizeKey 各养一份,先证"三实现输出一致且合同相同",
// 一致才抽 platform::PathComparisonKey,不同则具名策略不强并)。
//
// 向量覆盖:不存在路径、已存在路径、盘符/目录名大小写变体、混合斜杠、
// 尾斜杠、`..` 折叠、根路径、UNC、junction/目录符号链接(建不成就软跳过
// ——Windows 无特权建不了链接是环境事,不是合同事)。
//
// 断言分两层:
//   1. 三实现逐向量相等(抽象的前置证据);
//   2. 合同形状:出口无反斜杠、无尾斜杠(根除外)、ASCII 大写折小写、
//      大小写/斜杠变体同键。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "platform/paths.hpp"              // PathComparisonKey:比较键公共件(收编后的唯一实现)
#include "runtime/worktree.hpp"            // cli::NormalizeKey(worktree 房务)
#include "tools/isolation.hpp"             // tools::NormalizeKey(隔离闸)
#include "tools/session_utils.hpp"         // tools::NormalizePathForCompare(会话引用)

using lubancode::tools::NormalizePathForCompare;

namespace {

// 三只实现:
std::filesystem::path Utf8Path(const std::string& utf8) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string KeyIsolation(const std::string& utf8_path) {
    return lubancode::tools::NormalizeKey(Utf8Path(utf8_path));
}

std::string KeyWorktree(const std::string& utf8_path) {
    return lubancode::cli::NormalizeKey(Utf8Path(utf8_path));
}

std::string KeySessionUtils(const std::string& utf8_path) {
    return NormalizePathForCompare(utf8_path);
}

// 逐向量三实现一致,并与公共件 platform::PathComparisonKey 相等;顺带钉
// 合同形状(无反斜杠、无尾斜杠)。
void CheckThreeAgree(const std::string& label, const std::string& utf8_path) {
    const std::string a = KeyIsolation(utf8_path);
    const std::string b = KeyWorktree(utf8_path);
    const std::string c = KeySessionUtils(utf8_path);
    const std::string p = lubancode::platform::PathComparisonKey(Utf8Path(utf8_path));
    INFO(label << " -> isolation=" << a << " worktree=" << b << " session=" << c << " platform=" << p);
    CHECK(a == b);
    CHECK(a == c);
    CHECK(a == p);
    CHECK(a.find('\\') == std::string::npos);
    if (a.size() > 1) {
        CHECK(a.back() != '/');
    }
}

std::filesystem::path MakeTempRoot() {
    const auto dir = std::filesystem::temp_directory_path() / "lubancode-path-key-vectors";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

}  // namespace

TEST_CASE("路径比较键:三实现在共享向量上输出一致") {
    const auto root = MakeTempRoot();
    std::error_code ec;
    std::filesystem::create_directories(root / "Sub" / "Deeper", ec);
    std::ofstream touch(root / "Sub" / "file.txt", std::ios::binary);
    touch << "x";
    touch.close();

    const std::string root_utf8 = PathToUtf8(root);

    // 不存在路径(weakly_canonical 不报错,前缀归一 + 尾段照拼)。
    CheckThreeAgree("不存在·深路径", root_utf8 + "/no/such/path.json");
    CheckThreeAgree("不存在·带 ..", root_utf8 + "/Sub/../Ghost/../Gone");

    // 已存在路径:正斜杠 / 反斜杠 / 混合。
    CheckThreeAgree("存在·正斜杠", root_utf8 + "/Sub/file.txt");
    CheckThreeAgree("存在·反斜杠", root_utf8 + "\\Sub\\file.txt");
    CheckThreeAgree("存在·混合斜杠", root_utf8 + "/Sub\\Deeper");

    // 尾斜杠(目录)与多根尾斜杠。
    CheckThreeAgree("目录·尾斜杠", root_utf8 + "/Sub/");
    CheckThreeAgree("目录·双尾斜杠", root_utf8 + "/Sub//");

    // 大小写变体(Windows 上 weakly_canonical 会把已存在段还原成盘上真
    // 身;POSIX 原样保留——三实现怎么折都该同键)。
    CheckThreeAgree("大小写·上写目录", root_utf8 + "/SUB/FILE.TXT");
    CheckThreeAgree("大小写·下写目录", root_utf8 + "/sub/file.txt");

    // `..` 折叠与 `.`。
    CheckThreeAgree(".. 折叠", root_utf8 + "/Sub/Deeper/../../Sub/file.txt");
    CheckThreeAgree(". 当前", root_utf8 + "/./Sub/./file.txt");

    // 根路径。
#ifdef _WIN32
    CheckThreeAgree("根·盘符", "C:\\");
    CheckThreeAgree("根·盘符小写", "c:/");
    CheckThreeAgree("UNC", "\\\\server\\share\\dir\\file.txt");
#else
    CheckThreeAgree("根·斜杠", "/");
#endif
    CheckThreeAgree("根·临时根", root_utf8);

    // 相对路径(不带根;weakly_canonical 按进程 cwd 归一——三实现同样处
    // 置,一致性不赖 cwd 值)。
    CheckThreeAgree("相对路径", "Sub/../Plain/Name.TXT");
}

TEST_CASE("路径比较键:大小写与斜杠变体同键(合同本身)") {
    const auto root = MakeTempRoot();
    std::error_code ec;
    std::filesystem::create_directories(root / "Case", ec);
    const std::string root_utf8 = PathToUtf8(root);

    // 同一目录的四种写法必须同键——这是比较键存在的意义;三实现都得守。
    const std::string variants[] = {
        root_utf8 + "/Case", root_utf8 + "\\Case", root_utf8 + "/case/", root_utf8 + "\\CASE\\\\",
    };
    const std::string key0 = KeyIsolation(variants[0]);
    for (const std::string& variant : variants) {
        CHECK(KeyIsolation(variant) == key0);
        CHECK(KeyWorktree(variant) == key0);
        CHECK(KeySessionUtils(variant) == key0);
    }
}

#ifdef _WIN32
TEST_CASE("路径比较键:junction/目录符号链接解析一致(建不成则软跳过)") {
    const auto root = MakeTempRoot();
    std::error_code ec;
    std::filesystem::create_directories(root / "Real", ec);
    // 目录符号链接:Windows 要开发者模式/特权;建不成是环境不给,软跳过。
    const auto link = root / "Linked";
    std::filesystem::create_directory_symlink(root / "Real", link, ec);
    if (ec) {
        return;
    }
    // 链接解析后应与目标同键(weakly_canonical 穿链接)——三实现都得穿。
    const std::string real_key = KeyIsolation(PathToUtf8(root / "Real"));
    CHECK(KeyIsolation(PathToUtf8(link)) == real_key);
    CHECK(KeyWorktree(PathToUtf8(link)) == real_key);
    CHECK(KeySessionUtils(PathToUtf8(link)) == real_key);
}
#endif
