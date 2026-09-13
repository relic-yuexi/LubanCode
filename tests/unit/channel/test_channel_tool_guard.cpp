// 受保护路径闸册(QQ 机器人接入单 Q0)。
// 真源:docs/architecture/channels/security.md §3。纯谓词:护的路径表由
// 测试注入临时目录,不碰真实 home(DefaultChannelProtectedPaths 只验形状)。
//
// 钉的合同:
//   - read_file/search 的本地路径入参落进受保护根/文件即拦,canonical
//     比对(../ 回绕、斜杠与大小写变体绕不过);
//   - 检查在实际执行入口(Tool 包装),不只过滤工具名——名单外工具
//     不带路径入参的不受影响;
//   - search 不带 path:按 default_search_root(会话 cwd)判。
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include "channel/tool_guard.hpp"
#include "config/config.hpp"  // HomeLubancodeDir:默认表的形状验证

using namespace lubancode::channel;

namespace {

std::filesystem::path MakeTempDir(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode_guard_" + std::string(tag) + "_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// 路径的 UTF-8 写法,反斜杠统一正斜杠:既要进 JSON 入参(合法转义),
// 也与路径闸的 canonical 比对同口径(闸自己会归一,这里只是测试侧稳态)。
std::string ToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::string Blocked(const std::string& tool, const std::string& json_text,
                    const ChannelProtectedPaths& paths,
                    const std::string& default_root = std::string()) {
    const nlohmann::json input = nlohmann::json::parse(json_text, nullptr,
                                                        /*allow_exceptions=*/false);
    return ChannelToolPathBlocked(tool, input, paths, default_root);
}

}  // namespace

TEST_CASE("受保护根:直读/子文件/回绕/斜杠变体都拦") {
    const auto dir = MakeTempDir("root");
    std::error_code ec;
    std::filesystem::create_directories(dir / "channels" / "qqbot" / "main", ec);
    { std::ofstream(dir / "channels" / "qqbot" / "main" / "credentials", std::ios::trunc); }

    ChannelProtectedPaths paths;
    paths.roots.push_back(ToUtf8(dir / "channels"));
    paths.files.push_back(ToUtf8(dir / "config.json"));
    { std::ofstream(dir / "config.json", std::ios::trunc); }

    SUBCASE("根内子文件直读") {
        CHECK_FALSE(Blocked("read_file",
                            R"({"path": ")" + ToUtf8(dir / "channels" / "qqbot" / "main" / "credentials") + R"("})",
                            paths)
                        .empty());
    }
    SUBCASE("受保护单文件") {
        CHECK_FALSE(Blocked("read_file", R"({"path": ")" + ToUtf8(dir / "config.json") + R"("})",
                            paths)
                        .empty());
    }
    SUBCASE("../ 回绕进受保护根") {
        const std::string sneaky =
            ToUtf8(dir / "channels" / "qqbot" / "elsewhere" / ".." / ".." / "main" / "credentials");
        CHECK_FALSE(Blocked("read_file", R"({"path": ")" + sneaky + R"("})", paths).empty());
    }
    SUBCASE("根本体也拦") {
        CHECK_FALSE(Blocked("search", R"({"path": ")" + ToUtf8(dir / "channels") + R"("})", paths)
                        .empty());
    }
    SUBCASE("根外文件放行") {
        const auto outside = dir / "workspace" / "note.txt";
        { std::ofstream(outside, std::ios::trunc); }
        CHECK(Blocked("read_file", R"({"path": ")" + ToUtf8(outside) + R"("})", paths).empty());
        CHECK(Blocked("search", R"({"path": ")" + ToUtf8(dir / "workspace") + R"("})", paths)
                  .empty());
    }
}

TEST_CASE("符号链接绕过:canonical 解析到真实目标再判") {
    const auto dir = MakeTempDir("symlink");
    std::error_code ec;
    std::filesystem::create_directories(dir / "protected", ec);
    { std::ofstream(dir / "protected" / "secret", std::ios::trunc); }
    std::filesystem::create_directories(dir / "workspace", ec);

    // workspace/alias -> protected/secret(符号链接指进受保护根)。
    const auto link = dir / "workspace" / "alias";
    std::filesystem::create_directory_symlink(dir / "protected", link, ec);
    if (ec) {
        // 平台限制(Windows 无特权建不了链接):该案如实标未验,不冒充。
        MESSAGE("跳过符号链接案: create_directory_symlink 失败 (" << ec.message() << ")");
        return;
    }

    ChannelProtectedPaths paths;
    paths.roots.push_back(ToUtf8(dir / "protected"));
    CHECK_FALSE(Blocked("read_file",
                        R"({"path": ")" + ToUtf8(dir / "workspace" / "alias" / "secret") + R"("})",
                        paths)
                    .empty());
    CHECK_FALSE(Blocked("search", R"({"path": ")" + ToUtf8(link) + R"("})", paths).empty());
}

TEST_CASE("只认路径入参:不带路径的工具与路径外参数不受影响") {
    const auto dir = MakeTempDir("others");
    ChannelProtectedPaths paths;
    paths.roots.push_back(ToUtf8(dir));

    // 名单外工具(如 todo_write/web_fetch):没有本地路径入参,不查。
    CHECK(Blocked("todo_write", R"({"todos": []})", paths).empty());
    CHECK(Blocked("web_fetch", R"({"url": "https://example.com"})", paths).empty());
    // read_file 缺 path 参数:无路径可查(schema 层会先拒),放行到工具。
    CHECK(Blocked("read_file", R"({})", paths).empty());
    // path 不是字符串:不查(schema 层先拒)。
    CHECK(Blocked("read_file", R"({"path": 123})", paths).empty());
}

TEST_CASE("search 不带 path:按 default_search_root 判") {
    const auto dir = MakeTempDir("default_root");
    ChannelProtectedPaths paths;
    paths.roots.push_back(ToUtf8(dir / "vault"));

    SUBCASE("默认根在受保护根内:拦") {
        CHECK_FALSE(Blocked("search", R"({"pattern": "secret"})", paths,
                            ToUtf8(dir / "vault" / "sub"))
                        .empty());
    }
    SUBCASE("默认根在外面:放行") {
        CHECK(Blocked("search", R"({"pattern": "note"})", paths, ToUtf8(dir / "workspace"))
                  .empty());
    }
    SUBCASE("没递默认根:不查") {
        CHECK(Blocked("search", R"({"pattern": "note"})", paths).empty());
    }
}

TEST_CASE("空护表:闸恒放行;默认表形状正常") {
    ChannelProtectedPaths empty;
    CHECK(Blocked("read_file", R"({"path": "/etc/passwd"})", empty).empty());

    // 默认表:正常环境(home 拿得到)非空;每条都是绝对路径形状。只验
    // 形状,不碰真实文件。
    const auto defaults = DefaultChannelProtectedPaths();
    if (const auto home = lubancode::config::HomeLubancodeDir(); home.has_value()) {
        REQUIRE_FALSE(defaults.roots.empty());
        REQUIRE_FALSE(defaults.files.empty());
        for (const std::string& root : defaults.roots) {
            CHECK(root.find(*home) == 0);
        }
        for (const std::string& file : defaults.files) {
            CHECK(file.find(*home) == 0);
        }
        bool has_global_config = false;
        for (const std::string& file : defaults.files) {
            if (file.find("config.json") != std::string::npos) has_global_config = true;
        }
        CHECK(has_global_config);
    }
}
