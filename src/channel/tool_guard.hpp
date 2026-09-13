// 渠道工具受保护路径闸(QQ 机器人接入单 Q0;security.md §3)。
//
// 读文件/搜索类工具统一拦账号凭据与全局密钥配置:检查在实际工具执行
// 入口做(runtime 侧包装 Tool::execute,见 agent_channel_engine),这里
// 是纯谓词——按 canonical 路径比对(platform::PathComparisonKey:符号
// 链接/重解析点解析到真实目标、斜杠归一、ASCII 折小写),不只过滤
// 工具名。绕道(../ 回绕、大小写变体、符号链接指进来)同样拦下。
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::channel {

struct ChannelProtectedPaths {
    std::vector<std::string> roots;  // 受保护目录(整棵树)
    std::vector<std::string> files;  // 受保护单文件
};

// 默认受保护路径(security.md §3):全局 config.json(模型密钥+渠道
// secret 引用)、渠道状态根(凭据/锁/账)、Package/插件信任账。home 拿
// 不到时回空表——装配层照常注入自己的根,空表只意味着"没有默认项",
// 不是"全放行"的依据(闸对空表恒放行,但默认表在正常环境非空)。
ChannelProtectedPaths DefaultChannelProtectedPaths();

// 路径闸判定:tool_name 是注册工具名;input 是模型给的原始入参;
// default_search_root 是 search 未带 path 时的搜索起点(会话 cwd,由
// 装配层递进来)。
//
// 返回空串 = 放行;非空 = 拒绝文案(进 tool_result,给模型看,不带密钥)。
// 认得的路径入参:read_file.path、search.path;其余工具不查路径
// (首版渠道上限本来就只放只读件;名单外扩时这里同步)。
std::string ChannelToolPathBlocked(const std::string& tool_name, const nlohmann::json& input,
                                   const ChannelProtectedPaths& protected_paths,
                                   const std::string& default_search_root);

}  // namespace lubancode::channel
