// update 子命令实现(GitHubRelease自动更新单 P1,§三)。合同见头注释。
#include "cli/update_command.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "app/launcher.hpp"
#include "app/version.hpp"
#include "cli/frame_notice.hpp"  // 批 7:check 人看分支走 frame 键值对框
#include "config/config.hpp"
#include "config/update_checker.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"

namespace lubancode::cli {

namespace {

// 布局识别在 app::launcher(cli 命名空间里裸名找不到,起个别名)
namespace launcher = ::lubancode::app::launcher;

constexpr const char* kUpdateRepo = "relic-yuexi/LubanCode";

std::string PlatformSlug() {
#ifdef _WIN32
    return "windows-x64";
#elif defined(__APPLE__)
    return "macos-arm64";
#else
    return "linux-x64";
#endif
}

std::string MiB(std::uint64_t bytes) {
    return std::to_string(bytes >> 20) + " MiB";
}

// 装发行包文件名抠版本:lubancode-v0.26.300-windows-x64.zip -> 0.26.300。
// 抠不出合法 SemVer 明说,不猜。
std::optional<std::string> VersionFromArchiveName(const std::string& path_utf8) {
    const std::filesystem::path path = platform::Utf8ToPath(path_utf8);
    std::string name = platform::PathToUtf8(path.filename());
    constexpr const char* kPrefix = "lubancode-v";
    if (name.rfind(kPrefix, 0) != 0) return std::nullopt;
    name.erase(0, std::char_traits<char>::length(kPrefix));
    const std::size_t end = name.find_first_of("-.");
    if (end != std::string::npos) name.resize(end);
    const auto valid = config::CompareSemanticVersions(name, name);
    if (!valid.has_value() || *valid != 0) return std::nullopt;
    return name;
}

struct UpdateHelper {
    std::vector<std::string> interpreter;  // python 解释器 argv 前缀
    std::filesystem::path script;          // updater.py
};

std::optional<UpdateHelper> FindUpdateHelper(const std::filesystem::path& install_root) {
    std::error_code ec;
    const std::filesystem::path script = install_root / "updater" / "updater.py";
    if (!std::filesystem::is_regular_file(script, ec) || ec) return std::nullopt;

    const std::vector<std::vector<std::string>> candidates = {
        {"python3"}, {"python"}, {"py", "-3"}};
    for (const auto& candidate : candidates) {
        std::vector<std::string> probe = candidate;
        probe.emplace_back("-c");
        probe.emplace_back("import sys");
        // 探针超时 8 秒:Windows 商店假桩会弹窗/退出码 49,别等它
        const auto result = platform::RunProcess(probe, 8000);
        if (!result.spawn_failed && result.exit_code == 0) {
            UpdateHelper helper;
            helper.interpreter = candidate;
            helper.script = script;
            return helper;
        }
    }
    // 脚本在、python 不在:区分两种缺失,调用方要分别给指引
    UpdateHelper missing_python;
    missing_python.script = script;
    return missing_python;  // interpreter 为空 = 没有可用 python
}

int SpawnHelperAttached(const UpdateHelper& helper, const std::vector<std::string>& tail) {
    std::vector<std::string> argv = helper.interpreter;
    argv.push_back(platform::PathToUtf8(helper.script));
    for (const std::string& arg : tail) argv.push_back(arg);
    std::string error;
    const int code = platform::RunAttachedProcess(argv, &error);
    if (code < 0) {
        std::fprintf(stderr, "[update] 更新助手没跑起来: %s\n", error.c_str());
        return 1;
    }
    return code;
}

struct Target {
    config::ReleaseInfo release;
    config::ReleaseAssetInfo asset;
    std::string channel;
};

// 按通道挑 Release,按平台挑资产;版本比较复用 config 的语义版本比较。
std::optional<Target> ResolveTarget(bool prerelease, std::string* error_out) {
    const int connect_ms = config::kDefaultConnectTimeoutMs;
    const int timeout_secs = config::kDefaultRequestTimeoutSecs;
    auto release = prerelease ? config::FetchNewestPrerelease(connect_ms, timeout_secs)
                              : config::FetchLatestRelease(connect_ms, timeout_secs);
    if (!release.has_value()) {
        *error_out = release.error();
        return std::nullopt;
    }
    auto asset = config::PickAssetForPlatform(*release, PlatformSlug());
    if (!asset.has_value()) {
        *error_out = asset.error();
        return std::nullopt;
    }
    Target target;
    target.release = std::move(*release);
    target.asset = std::move(*asset);
    target.channel = prerelease ? "prerelease" : "stable";
    return target;
}

std::string LayoutLine(const launcher::LayoutInfo& layout) {
    const char* name = "便携/源码构建";
    if (layout.layout == launcher::Layout::VersionedEntry) name = "版本化(固定入口)";
    if (layout.layout == launcher::Layout::FlatInstall) name = "平铺安装";
    if (layout.layout == launcher::Layout::RootLauncher) name = "安装根启动器";
    std::string line = "安装布局: ";
    line += name;
    if (!layout.install_root.empty()) {
        line += "(" + platform::PathToUtf8(layout.install_root) + ")";
    }
    return line;
}

int RunCheck(bool prerelease, bool json) {
    const auto exe = platform::ExecutablePath();
    const launcher::LayoutInfo layout =
        exe.has_value() ? launcher::ClassifyExecutionLayout(*exe) : launcher::LayoutInfo{};

    std::string error;
    const auto target = ResolveTarget(prerelease, &error);
    if (!target.has_value()) {
        std::fprintf(stderr, "[update] 检查失败: %s\n", error.c_str());
        return 1;
    }

    const auto comparison =
        config::CompareSemanticVersions(target->release.version, std::string(app::kVersion));
    if (!comparison.has_value()) {
        std::fprintf(stderr, "[update] %s\n", comparison.error().c_str());
        return 1;
    }
    const bool newer = *comparison > 0;

    if (json) {
        nlohmann::json out;
        out["current_version"] = std::string(app::kVersion);
        out["latest_version"] = target->release.version;
        out["tag"] = target->release.tag_name;
        out["channel"] = target->channel;
        out["prerelease"] = target->release.prerelease;
        out["update_available"] = newer;
        out["release_url"] = target->release.html_url;
        nlohmann::json asset;
        asset["name"] = target->asset.name;
        asset["id"] = target->asset.id;
        asset["size"] = target->asset.size;
        asset["digest"] = target->asset.digest;
        out["asset"] = asset;
        out["layout"] = layout.install_root.empty()
                            ? std::string("standalone")
                            : (layout.layout == launcher::Layout::VersionedEntry
                                   ? std::string("versioned")
                                   : std::string("flat"));
        std::printf("%s\n", out.dump().c_str());
        return 0;
    }

    // 批 7:人看分支走 frame 键值对框(RenderUpdateCheckView 纯函数渲染,
    // 形状册直调);printf 散打收口 TermOut(同一 stdout 目的地)。--json
    // 分支(上面)字节级不动。
    EmitFrameLines(RenderUpdateCheckView(std::string(app::kVersion), target->release,
                                         target->asset, target->channel, LayoutLine(layout),
                                         newer, CliTheme(), CliFrameWidth()));
    TermOut().flush();
    return 0;
}

// §四:资产未带 digest 时,从 Release 挂的 update-meta.json(集中发布任务
// 汇总的资产摘要账)取该平台的可信摘要。取不到返回空——调用方按
// "没有可信摘要不自动安装"拒绝,不静默降级为不校验。顺带回填最低更新器
// 版本门槛(空 = 元数据里没写,不设这道门)。
std::string DigestFromUpdateMeta(const config::ReleaseInfo& release, const std::string& platform,
                                 std::string* minimum_updater_version) {
    const auto meta_asset = std::find_if(
        release.assets.begin(), release.assets.end(),
        [](const config::ReleaseAssetInfo& asset) { return asset.name == "update-meta.json"; });
    if (meta_asset == release.assets.end() || meta_asset->id == 0) return std::string();

    // 资产内容经 API 按 asset id 拉(Accept: octet-stream 走 302 到 CDN),
    // 钉死同一 Release 的同一资产,发布中途换资产拼不成套。
    const std::string url = "https://api.github.com/repos/" + std::string(kUpdateRepo) +
                            "/releases/assets/" + std::to_string(meta_asset->id);
    const cpr::Header headers{{"User-Agent", "lubancode-update-check"},
                              {"Accept", "application/octet-stream"}};
    const auto response = cpr::Get(cpr::Url{url}, headers,
                                   cpr::ConnectTimeout{std::chrono::milliseconds(
                                       config::kDefaultConnectTimeoutMs)},
                                   cpr::Timeout{std::chrono::seconds(
                                       config::kDefaultRequestTimeoutSecs)});
    if (response.error || response.status_code < 200 || response.status_code >= 300) {
        return std::string();
    }
    if (response.text.size() > 1024 * 1024) return std::string();
    const nlohmann::json meta = nlohmann::json::parse(response.text, nullptr, false);
    if (meta.is_discarded() || !meta.is_object()) return std::string();
    if (minimum_updater_version != nullptr && meta.contains("minimum_updater_version") &&
        meta["minimum_updater_version"].is_string()) {
        *minimum_updater_version = meta["minimum_updater_version"].get<std::string>();
    }
    if (!meta.contains("platforms") || !meta["platforms"].is_array()) return std::string();
    for (const auto& entry : meta["platforms"]) {
        if (!entry.is_object() || !entry.contains("platform") ||
            !entry["platform"].is_string() ||
            entry["platform"].get<std::string>() != platform) {
            continue;
        }
        if (entry.contains("sha256") && entry["sha256"].is_string()) {
            const std::string sha = entry["sha256"].get<std::string>();
            return sha.size() == 64 ? "sha256:" + sha : std::string();
        }
        return std::string();
    }
    return std::string();
}

std::vector<std::string> HelperTailFromTarget(const std::filesystem::path& install_root,
                                              const Target& target, bool dry_run) {
    std::vector<std::string> tail;
    tail.push_back(dry_run ? "plan" : "update");
    tail.push_back("--install-root");
    tail.push_back(platform::PathToUtf8(install_root));
    tail.push_back("--repo");
    tail.push_back(kUpdateRepo);
    tail.push_back("--version");
    tail.push_back(target.release.version);
    tail.push_back("--tag");
    tail.push_back(target.release.tag_name);
    tail.push_back("--exe-version");
    tail.push_back(config::ExeVersionFromTag(target.release.tag_name));
    tail.push_back("--release-id");
    tail.push_back(std::to_string(target.release.id));
    tail.push_back("--asset-id");
    tail.push_back(std::to_string(target.asset.id));
    tail.push_back("--asset-name");
    tail.push_back(target.asset.name);
    if (target.asset.size > 0) {
        tail.push_back("--asset-size");
        tail.push_back(std::to_string(target.asset.size));
    }
    tail.push_back("--digest");
    tail.push_back(target.asset.digest);
    tail.push_back("--channel");
    tail.push_back(target.channel);
    return tail;
}

}  // namespace

std::vector<std::string> RenderUpdateCheckView(const std::string& current_version,
                                               const config::ReleaseInfo& release,
                                               const config::ReleaseAssetInfo& asset,
                                               const std::string& channel,
                                               const std::string& layout_line, bool newer,
                                               const Theme& theme, int width) {
    // 六句既有文案原样进框(逐句按冒号拆列,一字不添不改);结论句按有无
    // 新版上语义色(Pass=table_pass,批 4 裁量:Theme 无 tool_accent)。
    std::vector<frame::Field> fields;
    fields.push_back(SentenceField("当前版本: " + current_version));
    fields.push_back(SentenceField(layout_line));
    fields.push_back(SentenceField("远端 " + channel + " 通道: " + release.tag_name + "(" +
                                   release.version + ")" +
                                   (release.prerelease ? " [预发布]" : "")));
    if (!release.html_url.empty()) {
        fields.push_back(SentenceField("发布页: " + release.html_url));
    }
    fields.push_back(SentenceField("资产: " + asset.name + "(" +
                                   (asset.size > 0 ? MiB(asset.size) : std::string("大小未知")) +
                                   ")" +
                                   (asset.digest.empty() ? " [未带摘要:不能自动安装,见下]"
                                                         : "")));
    if (newer) {
        fields.push_back(SentenceField("结论: 有新版可装。执行 lubancode update 一键更新"
                                       "(技能保护预检、整包校验、指针切换、健康检查都在事务里)。",
                                       frame::FieldAccent::Pass));
    } else {
        fields.push_back(SentenceField("结论: 已是最新(本地 " + current_version + ",远端 " +
                                       release.version + ")。本地不低于 Latest,不降级;"
                                       "回退用 lubancode update --rollback。"));
    }
    return frame::RenderKeyValues("update check", fields, theme, frame::Light(), width);
}

int RunUpdateCommand(const UpdateCommandArgs& args) {
    if (args.verb == "check") {
        return RunCheck(args.prerelease, args.json);
    }

    const auto exe = platform::ExecutablePath();
    if (!exe.has_value() || exe->empty()) {
        std::fprintf(stderr, "[update] 定位不到当前可执行文件。\n");
        return 1;
    }
    const launcher::LayoutInfo layout = launcher::ClassifyExecutionLayout(*exe);

    if (args.verb == "rollback") {
        if (layout.layout != launcher::Layout::VersionedEntry) {
            std::fprintf(stderr,
                         "[update] --rollback 只适用于固定入口布局(做过一键更新的安装)。"
                         "平铺安装请用包内安装脚本的备份。\n");
            return 1;
        }
        const auto helper = FindUpdateHelper(layout.install_root);
        if (!helper.has_value() || helper->interpreter.empty()) {
            std::fprintf(stderr,
                          "[update] 安装根里没有受管更新助手(%s)。"
                          "回滚请下载任一发行包重装,或手工把 current.json 指回旧版本。\n",
                          platform::PathToUtf8(layout.install_root / "updater" / "updater.py")
                              .c_str());
            return 1;
        }
        return SpawnHelperAttached(*helper, {"rollback", "--install-root",
                                             platform::PathToUtf8(layout.install_root)});
    }

    // ---- 一键更新 / 预演 ----
    if (layout.install_root.empty()) {
        std::fprintf(stderr,
                     "[update] 便携/源码构建的 exe 没有安装根,一键更新不适用"
                     "(不覆盖工作区,§六)。请用官方发行包安装后再更新。\n");
        return 1;
    }
    const auto helper = FindUpdateHelper(layout.install_root);
    if (!helper.has_value()) {
        std::fprintf(stderr,
                     "[update] 这份安装还没带受管更新助手(旧版发行包装的)。"
                     "一次性引导:下载最新发行包,运行包内 install 脚本一次,"
                     "之后 lubancode update 即可一键更新。\n");
        return 1;
    }
    if (helper->interpreter.empty()) {
        std::fprintf(stderr,
                     "[update] 找不到可用的 python3(python3 / python / py -3 都没探到)。"
                     "更新助手需要它跑事务。装好 python3 后重试;"
                     "或下载发行包用包内 install 脚本升级(同为受保护流程)。\n");
        return 1;
    }

    if (!args.from_archive.empty()) {
        // GitHub 不通时的手动路径(§八):本地官方包走同一校验/保护/事务;
        // 摘要由助手对本地包自算并钉进事务。
        const auto version = VersionFromArchiveName(args.from_archive);
        if (!version.has_value()) {
            std::fprintf(stderr,
                         "[update] --from 的文件名里抠不出版本号(形如 "
                         "lubancode-v0.26.300-windows-x64.zip)。\n");
            return 1;
        }
        std::vector<std::string> tail;
        tail.push_back(args.verb == "dry-run" ? "plan" : "update");
        tail.push_back("--install-root");
        tail.push_back(platform::PathToUtf8(layout.install_root));
        tail.push_back("--version");
        tail.push_back(*version);
        tail.push_back("--tag");
        tail.push_back("v" + *version);
        tail.push_back("--exe-version");
        tail.push_back(config::ExeVersionFromTag("v" + *version));
        tail.push_back("--archive");
        tail.push_back(args.from_archive);
        tail.push_back("--channel");
        tail.push_back(args.prerelease ? "prerelease" : "stable");
        return SpawnHelperAttached(*helper, tail);
    }

    std::string error;
    auto target = ResolveTarget(args.prerelease, &error);
    if (!target.has_value()) {
        std::fprintf(stderr, "[update] 检查失败: %s\n离线时可 lubancode update --from "
                             "<发行包> 走本地包,同一校验与事务。\n",
                     error.c_str());
        return 1;
    }
    const auto comparison =
        config::CompareSemanticVersions(target->release.version, std::string(app::kVersion));
    if (comparison.has_value() && *comparison <= 0) {
        // 批 7:人看回执进键值对框;printf 散打收口 TermOut。
        const Theme theme = CliTheme();
        PrintNotice(theme, {"已是最新(本地 " + std::string(app::kVersion) + ",远端 " +
                                       target->release.version +
                                       ")。本地不低于远端,不降级;"
                                       "回退用 lubancode update --rollback。"});
        TermOut().flush();
        return 0;
    }
    // 最低更新器版本门槛:update-meta.json 里声明了就得过,过不了先走包内
    // 安装脚本一次性升级(更新助手自升级的分阶段交接另设实现项)。
    std::string minimum_updater_version;
    if (target->asset.digest.empty()) {
        target->asset.digest =
            DigestFromUpdateMeta(target->release, PlatformSlug(), &minimum_updater_version);
    }
    if (!minimum_updater_version.empty()) {
        const auto floor =
            config::CompareSemanticVersions(std::string(app::kVersion), minimum_updater_version);
        if (floor.has_value() && *floor < 0) {
            std::fprintf(stderr,
                         "[update] 本机 %s 低于该 Release 要求的最低更新器版本 %s。"
                         "先下载发行包运行包内安装脚本升级一次,"
                         "之后 lubancode update 恢复一键更新。\n",
                         std::string(app::kVersion).c_str(), minimum_updater_version.c_str());
            return 1;
        }
    }
    if (target->asset.digest.empty()) {
        // §四:资产没带摘要,又没有可信更新元数据摘要,停止自动安装,
        // 不静默降级为不校验。
        std::fprintf(stderr,
                     "[update] Release %s 的资产 %s 未带 sha256 摘要,发布侧也没提供"
                     "可信更新元数据(update-meta.json),不做自动安装。手动路径:"
                     "下载该包后 lubancode update --from <包路径>"
                     "(本地包自算摘要并钉进事务)。\n",
                     target->release.tag_name.c_str(), target->asset.name.c_str());
        return 1;
    }
    return SpawnHelperAttached(*helper,
                               HelperTailFromTarget(layout.install_root, *target,
                                                    args.verb == "dry-run"));
}

}  // namespace lubancode::cli
