// TUI 排版批 7:`lubancode update --check` 人看分支的输出形状册。
//   - RenderUpdateCheckView 纯函数直调:六句既有文案(当前版本/安装布局/
//     远端通道/发布页/资产/结论)逐句按冒号拆进键值对框,标题
//     update check 嵌上边框;结论句语义色(有新版=Pass 即 table_pass);
//   - plain 主题(--no-color/T3 降级路径)零转义字节、无框字形——合同
//     第 3 条;
//   - --json 分支(机器面)与 RunCheck 落盘路径要发网,CI 里测不到,
//     真机未验;check 的 printf 散打收口 TermOut 属端口合并(同一 stdout),
//     落盘文字全数进框。
//   - LayoutInfo 在 app 层,cli 不反引 app——渲染函数吃拼好的布局句,
//     这里喂代表值。

#include <doctest/doctest.h>

#include <sstream>
#include <string>
#include <vector>

#include "cli/theme.hpp"
#include "cli/update_command.hpp"
#include "config/update_checker.hpp"

using namespace lubancode;

namespace {

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string Join(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        out += line;
        out += "\n";
    }
    return out;
}

const cli::Theme dark = cli::BuiltinTheme("dark");
const cli::Theme plain = cli::BuiltinTheme("plain");

constexpr const char* kBoxTopLeft = "\xe2\x94\x8c";  // ┌

config::ReleaseInfo MakeRelease() {
    config::ReleaseInfo release;
    release.tag_name = "v0.26.300";
    release.version = "0.26.300";
    release.html_url = "https://github.com/relic-yuexi/LubanCode/releases/v0.26.300";
    release.id = 12345;
    release.prerelease = false;
    return release;
}

config::ReleaseAssetInfo MakeAsset() {
    config::ReleaseAssetInfo asset;
    asset.name = "lubancode-v0.26.300-windows-x64.zip";
    asset.id = 67890;
    asset.size = 21ull * 1024 * 1024;  // 21 MiB
    asset.digest = "sha256:0123456789abcdef";
    return asset;
}

}  // namespace

TEST_CASE("update check:键值对框,标题 update check,六句拆列") {
    const std::vector<std::string> lines =
        cli::RenderUpdateCheckView("0.26.281", MakeRelease(), MakeAsset(), "stable",
                                   "安装布局: 平铺安装(D:/lubancode)", /*newer=*/true, dark,
                                   /*width=*/0);
    const std::string out = Join(lines);
    CHECK(!lines.empty());

    CHECK(Contains(out, "update check"));  // 标题嵌上边框
    CHECK(Contains(out, kBoxTopLeft));
    CHECK(Contains(out, "当前版本"));
    CHECK(Contains(out, "0.26.281"));
    CHECK(Contains(out, "安装布局"));
    CHECK(Contains(out, "平铺安装"));
    CHECK(Contains(out, "远端 stable 通道"));
    CHECK(Contains(out, "v0.26.300"));
    CHECK(Contains(out, "发布页"));
    CHECK(Contains(out, "资产"));
    CHECK(Contains(out, "21 MiB"));
    CHECK(Contains(out, "结论"));
    CHECK(Contains(out, "有新版可装"));
}

TEST_CASE("update check:结论句语义色——有新版走 table_pass(批 4 裁量)") {
    const std::vector<std::string> lines =
        cli::RenderUpdateCheckView("0.26.281", MakeRelease(), MakeAsset(), "stable",
                                   "安装布局: 便携/源码构建", /*newer=*/true, dark,
                                   /*width=*/0);
    const std::string out = Join(lines);
    // Theme 无 tool_accent,一律 table_pass(批 4 裁量先例)。
    CHECK(Contains(out, dark.table_pass));
    CHECK(Contains(out, "有新版可装"));
}

TEST_CASE("update check:已是最新档不上 Pass 色,尾句指 rollback") {
    const std::vector<std::string> lines =
        cli::RenderUpdateCheckView("0.26.300", MakeRelease(), MakeAsset(), "stable",
                                   "安装布局: 平铺安装", /*newer=*/false, dark,
                                   /*width=*/0);
    const std::string out = Join(lines);
    CHECK(Contains(out, "已是最新"));
    CHECK(Contains(out, "--rollback"));
    CHECK(out.find(dark.table_pass) == std::string::npos);
}

TEST_CASE("update check:plain 零转义、无框、六键都在") {
    const std::vector<std::string> lines =
        cli::RenderUpdateCheckView("0.26.281", MakeRelease(), MakeAsset(), "prerelease",
                                   "安装布局: 版本化(固定入口)", /*newer=*/true, plain,
                                   /*width=*/0);
    const std::string out = Join(lines);

    CHECK(out.find("\x1b") == std::string::npos);
    CHECK(out.find(kBoxTopLeft) == std::string::npos);
    CHECK(Contains(out, "update check"));  // 标题独立成行
    CHECK(Contains(out, "当前版本"));
    CHECK(Contains(out, "远端 prerelease 通道"));
    CHECK(Contains(out, "结论"));
}
