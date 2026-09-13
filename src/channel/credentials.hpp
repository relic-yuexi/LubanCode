// 渠道凭据 resolver(QQ 机器人接入单 Q0;TODO §三)。
//
// 唯一真源 docs/architecture/channels/configuration.md §4(来源三种、
// 解析优先级、secret_file 读取规矩)与 security.md §6(泄露禁令)。
// 合同:
//   - 优先级 secret_file > secret_env > secret 明文;高优先级来源配置了
//     却无效时明报稳定错误,不静默降级——降级只会把"配置错了"藏成
//     "碰巧能用"。
//   - secret_file:绝对路径;canonical 解析(符号链接/重解析点到真实
//     目标);常规文件;归属当前用户;组/其他用户无读写位(POSIX)或
//     DACL 无其他账户读权(Windows);上限 kCredentialFileMaxBytes;内容
//     即 AppSecret 原值,至多剥末尾一处换行;拒绝空值与内部控制字符。
//   - 密钥值只在本进程内持有:不进 argv、不进模型输入、不进会话、不进
//     trace、不进错误与普通日志。诊断只报来源与状态(稳定码 + 路径,
//     绝不带值)。
//   - 凭据只进入获准的渠道执行载体(Q1 三选一定案:进程内直连则不出
//     宿主进程;受管子进程则走不落日志的专用启动管道)。子进程若存在,
//     环境按白名单继承(见 SidecarEnvAllowlist):宿主模型 API key、
//     其他账号的密钥环境变量一律不递。
//   - 首版不生成 credentials.json.enc(文件名带 enc 不代表加密)。
//
// 纯运行时件:读环境变量与用户明指的文件,不写任何文件。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "channel/channel_config.hpp"

namespace lubancode::channel {

// secret_file 的字节上限(§三"拒绝超限";AppSecret 是短串,8 KiB 已宽裕)。
inline constexpr std::uintmax_t kCredentialFileMaxBytes = 8 * 1024;

struct ResolvedChannelCredential {
    enum class Source { File, Env, InlinePlaintext };
    Source source = Source::File;
    // 密钥值:只在进程内持有(见文件头泄露禁令)。无 operator<<,不许落日志。
    std::string secret;
};

struct ChannelCredentialError {
    std::string reason;  // 稳定码(见 .cpp 顶部清单)
    std::string detail;  // 脱敏:可带路径,绝不带密钥值
};

// 稳定码清单:
//   credential_missing     三种来源都没配
//   secret_file_relative   secret_file 不是绝对路径
//   secret_file_missing    文件不存在
//   secret_file_not_regular 不是常规文件(目录/设备……)
//   secret_file_insecure   归属/权限不安全(他人可读、非当前用户所有、
//                          Windows DACL 放行了别的账户;含无 DACL=全员)
//   secret_file_too_large  超过 kCredentialFileMaxBytes
//   secret_file_read_fail  读失败(IO 错误)
//   secret_file_invalid_utf8 内容不是合法 UTF-8
//   secret_file_bad_content 含内部控制字符(换行只许末尾那一处)
//   secret_file_empty      剥掉末尾换行后为空
//   secret_env_missing     环境变量不存在
//   secret_env_empty       环境变量为空串
//   secret_inline_empty    明文 secret 为空串

// 解析账号凭据。纯同步调用,启动路径与 doctor 都可用。
std::expected<ResolvedChannelCredential, ChannelCredentialError> ResolveChannelCredential(
    const ChannelAccountUserConfig& account);

// 单独暴露的文件安全检查(诊断/测试用):canonical_path 须已是解析后的
// 真实目标。错误 reason 同上(secret_file_insecure/...)。
std::expected<void, ChannelCredentialError> CheckCredentialFileSecurity(
    const std::filesystem::path& canonical_path);

// 渠道子进程环境白名单(Q0 定形;Q1 定案走子进程时 spawn 实装照此继承,
// 定案进程内直连则本表不消费)。凭据不走环境。
const std::vector<std::string>& SidecarEnvAllowlist();

// 防御性脱敏:把 text 里出现的已知密钥值换成 <redacted>。密钥为空原样
// 返回。任何"疑似要进日志/错误"的字符串都该先过一遍。
std::string RedactSecret(std::string text, const std::string& secret);

}  // namespace lubancode::channel
