// update 子命令(GitHubRelease自动更新单 P1,§三):`lubancode update`
// [--check | --dry-run | --rollback] [--prerelease] [--from <发行包>]。
//
// 分工:版本比较与目标挑选在这里(复用 config::update_checker 的语义版本
// 比较与 Release 解析,不重写);下载校验、隔离解包、技能保护预检、指针
// 激活、健康检查、回滚全在受管更新助手 scripts/updater.py(包内 updater/
// 树)——本命令把钉死的 Release ID / asset ID / 摘要递给它,发布中途更新
// 拼不成一套。`--check-update` 与交互 `/update check` 保留,检查走同一份
// config::CheckForUpdate。
#pragma once

#include <string>
#include <vector>

#include "cli/theme.hpp"             // Theme:渲染纯函数吃主题
#include "config/update_checker.hpp" // ReleaseInfo/ReleaseAssetInfo

namespace lubancode::cli {

struct UpdateCommandArgs {
    std::string verb;          // "" = 一键更新 | check | dry-run | rollback
    bool prerelease = false;   // 显式选预发布通道
    bool json = false;         // check 的机器可读输出
    std::string from_archive;  // --from <发行包>;空 = GitHub 资产
};

// TUI 排版批 7:check 人看分支的 frame 渲染(纯函数,形状册直调;check
// 主路径要发网,CI 里测不到 RunCheck 的落盘)。六句既有文案逐句按冒号拆
// 进键值对框,标题用命令名 update check(schema 标识符)。layout_line 是
// cpp 侧拼好的 "安装布局" 句值(cli 不反引 app,LayoutInfo 不进头文件)。
std::vector<std::string> RenderUpdateCheckView(const std::string& current_version,
                                               const config::ReleaseInfo& release,
                                               const config::ReleaseAssetInfo& asset,
                                               const std::string& channel,
                                               const std::string& layout_line, bool newer,
                                               const Theme& theme, int width);

// 退出码:0 成功/已是最新;1 失败(旧版继续可用);2 needs-review(冲突
// 未决);3 rolled-back(已恢复旧版)——后两码由更新助手原样透传。
int RunUpdateCommand(const UpdateCommandArgs& args);

}  // namespace lubancode::cli
