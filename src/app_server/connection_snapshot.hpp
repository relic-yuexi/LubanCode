// 模型连接运行快照(应用Worker接入单 §八,本单切片:独立 Worker 启动
// 快照与配置来源检查;热切换/会话级覆盖归"AppServer模型连接解耦"单,
// 这里不做)。三个纯函数件,全部 metadata-only:
//
//   FreezeConnectionSnapshot   启动时从 Config/ConfigSources 冻结一份连接
//                              快照(wire/model/provider/脱敏端点/密钥引用/
//                              配置版本/逐字段来源/角色路由状态)。单 Worker
//                              连接冻结的合同载体:RunAppServerMode 进程启
//                              动读一次配置、冻一份,进程期内逐场 thread 复
//                              用同一份(§八 178:运行时更换配置只影响新
//                              Worker——配置不重读,环境变量改了对本进程无效)。
//   FormatEffectiveConfigDiagnostics
//                              §四.111 的脱敏 effective-config 诊断(stderr
//                              启动时打一次):根路径/来源清单/逐字段取值来
//                              源/部署档与指纹/功能状态。零密钥。
//
// 红线(§八 176/177):密钥正文、密钥的内容哈希(可离线猜测)一概不进;
// 端点只留 scheme://host[:port],路径/查询/userinfo 剥掉(有些服务的 URL
// 路径段带 token,整段不安全);密钥引用只说"钥匙在哪"(环境变量名/
// provider 声明的 key_env/inline),不说钥匙是什么。连接配置版本是
// 非敏感连接事实的 sha256——换钥匙不改版本(版本跟"连到哪、用哪个模型",
// 不跟"哪把钥匙";钥匙轮换的追溯归模型连接 owner 单)。
//
// 快照形状沿 v3 请求账 request_snapshot_ref 一路的"metadata-only JSON +
// 指纹"风格(PR #55 的预算快照先例);落账两处:thread/started 回执的
// additive connection 字段(客户端可见),与 v3 请求账 model.request.prepared
// 的 connection 块(经 TrajectoryTurnBridge::Identity 递入,见
// trajectory_session.hpp)。本件不落账、不碰盘——纯函数。
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "app_server/harness_profile.hpp"
#include "config/config.hpp"

namespace lubancode::app_server {

// 端点脱敏:只留 scheme://host[:port]。没有 "://" 的写法没法安全判段,
// 返回空串(快照里 endpoint 落 null,不冒险记原文)。
std::string MaskEndpointUrl(const std::string& base_url);

// 密钥引用:只说钥匙在哪,不说钥匙是什么。
//   环境变量优先级压过 provider 时 → env:LUBAN_API_KEY / env:LUBANCODE_API_KEY;
//   provider 生效 → provider:<名>:env:<key_env> / provider:<名>:inline /
//                   provider:<名>:none;
//   配置文件兜底 → config-file:inline;
//   都没有 → none。
std::string DescribeSecretReference(const config::Config& config, const config::ConfigSources& sources);

// 连接配置版本:非敏感连接事实(wire/base_url/model/provider/鉴权模式/
// 角色路由)canonical JSON 的 sha256(64 位十六进制)。密钥不进料——换钥
// 匙版本不动,这是刻意语义,不是漏项。
std::string ConnectionConfigVersion(const config::Config& config);

// 启动冻结的连接快照。字段合同(冻结后进程期内不变):
//   wire/model/provider/endpoint(脱敏)/secretRef/configVersion
//   sources.{wire,baseUrl,model,apiKey}(config::ToString(Source) 的标签)
//   roles.{routing,normal,cheap,lao,compact}(routing: off/shorthand/advanced;
//     各角色 {model, source},未配置的角色落 null)
nlohmann::json FreezeConnectionSnapshot(const config::Config& config, const config::ConfigSources& sources);

// §四.111 诊断的取材件。根路径与配置文件路径由调用方解析好递进(个人
// 布局与应用根语义各走各的口,本件不重读环境)。
struct EffectiveConfigInput {
    const config::Config* config = nullptr;
    const config::ConfigSources* sources = nullptr;
    std::string config_root;  // 参数根(个人布局=个人 .lubancode 目录)
    std::string data_root;    // 状态写入根
    bool managed = false;
    bool app_root_active = false;
    std::optional<std::string> project_config_path;  // 本次合并吃到的项目级档
    std::optional<std::string> global_config_path;   // 全局档(应用根语义=参数根 config.json)
    std::string profile_path;      // 部署档路径(空 = 无档)
    std::string profile_sha256;    // 部署档内容指纹(空 = 无档)
    const HarnessProfile* harness = nullptr;  // 档在场:功能状态从档面报
};

// 脱敏 effective-config 的多行诊断文本(每行带 [app-server] 前缀,stderr
// 打印;stdout 是协议口,诊断一律 stderr)。零密钥:凭据只以
// DescribeSecretReference 的引用形态出现。
std::string FormatEffectiveConfigDiagnostics(const EffectiveConfigInput& input);

}  // namespace lubancode::app_server
