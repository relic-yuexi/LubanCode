// im 最近选择偏好册(§6.2):只存标识、原子保存、坏档回空、悬空由调用方
// 回退(裁决册在 tests/unit/app)。
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "config/im_preferences.hpp"
#include "platform/paths.hpp"

using namespace lubancode::config;

namespace {

std::filesystem::path MakeTempFile(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode_impref_" + std::string(tag) + "_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir / "im-preferences.json";
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("往返:保存后读回,只存标识") {
    const auto file = MakeTempFile("roundtrip");
    ImPreferenceStore store(file);
    ImPreferences prefs;
    prefs.last = ImRecentSelection{"qqbot", "main"};
    prefs.recent_account["qqbot"] = "main";
    REQUIRE(store.Save(prefs).has_value());

    const ImPreferences loaded = store.Load();
    REQUIRE(loaded.last.has_value());
    CHECK(loaded.last->channel_id == "qqbot");
    CHECK(loaded.last->account_id == "main");
    REQUIRE(loaded.recent_account.count("qqbot") == 1);
    CHECK(loaded.recent_account.at("qqbot") == "main");

    // 落盘内容只有标识:不带 AppSecret/token 一类(扫一遍已知敏感字样)。
    const std::string text = ReadFile(file);
    CHECK(text.find("AppSecret") == std::string::npos);
    CHECK(text.find("token") == std::string::npos);
    CHECK(text.find("schema") != std::string::npos);  // 可版本化
}

TEST_CASE("无档/坏档/版本不认:回空偏好,不拦启动") {
    const auto file = MakeTempFile("empty");
    ImPreferenceStore store(file);
    CHECK_FALSE(store.Load().last.has_value());  // 无档

    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "{ not json !!!";
    }
    CHECK_FALSE(store.Load().last.has_value());  // 坏档回空

    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << R"json({"schema": 99, "last": {"channel": "qqbot", "account": "main"}})json";
    }
    CHECK_FALSE(store.Load().last.has_value());  // 未来版本不猜
}

TEST_CASE("保存后无半截文件:原子写不留 tmp 尾巴") {
    const auto file = MakeTempFile("atomic");
    ImPreferenceStore store(file);
    ImPreferences prefs;
    prefs.last = ImRecentSelection{"qqbot", "main"};
    REQUIRE(store.Save(prefs).has_value());
    std::error_code ec;
    std::size_t entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(file.parent_path(), ec)) {
        ++entries;
        CHECK(entry.path().extension() != ".tmp");  // 临时件已收走
    }
    CHECK(entries == 1);
}
