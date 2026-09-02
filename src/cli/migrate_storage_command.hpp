// `lubancode migrate-storage <verb>`(存储 v2 P0-5 的命令面,单子 §8.1):
//   plan    扫旧源写 intent.json(只读旧档,不动任何源字节)。
//   run     执行迁移(幂等,可续跑;自动续跑最近一只未 committed 的 operation)。
//   status  只读列进度与全机未迁清单(§7.3"列出未迁项目")。
// 修饰词:
//   --operation <id>      run 续跑指认。
//   --project-root <路径> 旧项目库缺 project.json 时的显式映射(可多枚)。
//   --delete-source --yes 删源(单独二次确认;只删已 committed 且复验过的源)。
// 退出码:0 成;1 用法错;2 迁移有失败项或被中断(可续跑);3 删源未获核验。
#pragma once

#include <map>
#include <string>
#include <vector>

namespace lubancode::cli {

struct MigrateStorageCommandArgs {
    std::string verb;  // plan | run | status
    std::string operation_id;
    std::vector<std::string> project_roots;  // --project-root(可多枚)
    bool delete_source = false;
    bool confirm_delete = false;  // --yes
};

int RunMigrateStorageCommand(const MigrateStorageCommandArgs& args);

}  // namespace lubancode::cli
