// 应用根语义的解析与校验(应用Worker接入补齐单 §四,P1)。
//
// 三枚环境变量(冻结合同见 docs/reference/capability-contract.md §13):
//   LUBANCODE_HOME       参数根本身,不追加 .lubancode;未设=个人 CLI 旧布局
//   LUBANCODE_DATA_HOME  独立运行数据根;未设默认 <LUBANCODE_HOME>/data;
//                        仅在启用应用根语义时采用,孤立设置是配置错误
//   LUBANCODE_MANAGED    只认 1(开)与 0(关);=1 即多租户隔离单 Managed 档
//                        的进程级入口,必须显式给 LUBANCODE_HOME
//
// env 是进程级的:外部宿主给每个 Worker child 构造专属 env,不修改自己
// 的全局环境,也不重定义 HOME/USERPROFILE 冒充应用参数根(单 §4.1)。
// 与多租户隔离单"不改进程环境以切换租户"不冲突——那条管的是"同进程
// 轮流服务多人",本合同管的是"一进程一份 env、进程内单一身份"。
//
// 本文件是纯函数:解析只读入参,不碰盘、不起进程。文件系统侧的可达性
// 探测归 EnsureRuntimeRootsAccessible(启动门调用)。canonical/链接比较
// 复用 platform::PathComparisonKey(weakly_canonical 失败退
// lexically_normal),不另写一套规范化。
#pragma once

#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace lubancode::config {

// 一份环境快照。测试注入两套不同快照即可模拟两只 Worker 并行双根;
// 生产用 CaptureProcessEnv 抓当前进程环境。
struct RuntimeEnvSnapshot {
    // 键=变量名,值可为空串("设了但为空");缺席=变量未设。
    std::map<std::string, std::string> vars;
    // 主目录(Windows %USERPROFILE% / POSIX $HOME)。解析本身不消费它
    // (参数根语义与个人主目录无关),留在这份快照里供测试断言"个人家
    // 目录零副作用"时引用。
    std::optional<std::string> home_dir;

    // 变量未设返回 nullopt;设了(含空串)返回值。
    std::optional<std::string> Get(const char* name) const;
};

// 抓进程环境的默认视图:只取三枚应用根变量 + 主目录。
RuntimeEnvSnapshot CaptureProcessEnv();

// 解析结果。config_root/data_root 在 app_root_active=false 时为空
// (个人 CLI 旧布局由 HomeLubancodeDir()/StateRootDir() 兜底)。
struct RuntimePaths {
    bool app_root_active = false;  // LUBANCODE_HOME 有效设置
    bool managed = false;          // LUBANCODE_MANAGED=1
    std::optional<std::filesystem::path> config_root;  // 参数根(可只读)
    std::optional<std::filesystem::path> data_root;    // 状态写入根(可写)
};

// 解析 + 校验。失败返回人话(启动门据此拒启),判据(单 §4.1):
//   - LUBANCODE_HOME 空值/相对路径 → 配置错误
//   - LUBANCODE_DATA_HOME 孤立设置(无 HOME)/空值/相对路径 → 配置错误
//   - 数据根 == 参数根,或参数根落数据根之内 → 配置错误(读写不分)
//     数据根在参数根之内合法(默认 <HOME>/data 即如此)
//   - LUBANCODE_MANAGED 值不是 0/1 → 配置错误;=1 而 HOME 未设 → 配置错误
// 均不静默回个人目录。
std::expected<RuntimePaths, std::string> ResolveRuntimePaths(const RuntimeEnvSnapshot& env);

// 文件系统侧可达性(启动门在 Resolve 成功后调用):参数根与数据根缺失
// 合法——在获准父目录内创建;建不动(权限/磁盘)才报错。canonical/链接
// 处理遵多租户隔离单合同:OS 挂载与权限承担硬拒绝,这里只做前置探测。
std::expected<void, std::string> EnsureRuntimeRootsAccessible(const RuntimePaths& paths);

}  // namespace lubancode::config
