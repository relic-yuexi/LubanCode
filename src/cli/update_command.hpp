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

namespace lubancode::cli {

struct UpdateCommandArgs {
    std::string verb;          // "" = 一键更新 | check | dry-run | rollback
    bool prerelease = false;   // 显式选预发布通道
    bool json = false;         // check 的机器可读输出
    std::string from_archive;  // --from <发行包>;空 = GitHub 资产
};

// 退出码:0 成功/已是最新;1 失败(旧版继续可用);2 needs-review(冲突
// 未决);3 rolled-back(已恢复旧版)——后两码由更新助手原样透传。
int RunUpdateCommand(const UpdateCommandArgs& args);

}  // namespace lubancode::cli
