// 渠道设置的平台表单与定点配置服务(QQBot Windows 修复单 §5.1/§5.2,
//§六 6.0 的 ChannelSetupRegistry/ChannelConfigService 两件)。
//
// 分工:
//   - ChannelSetupRegistry:平台 ID → 显示名、实现状态、表单字段。与
//     运行时适配器注册对账:只有 qqbot 有进程内适配器,其余平台如实标
//     尚未支持,不进假成功配置。
//   - ChannelConfigService:目标账号的配置变更提交。定点改全局 config.json
//     的 channels 子树(其他账号/模型/未知字段原样保留),调凭据服务落
//     受管密钥;内存里的 secret 不进任何通用结果结构(changes 只记差异
//     摘要,不含值)。
//
// 提交次序(§5.2"中断不留启用但无可读密钥"):
//   1) 取 secrets 根的进程锁(两个向导不互相覆盖);
//   2) 新密钥先写独立版本文件并过生产读取器复验;
//   3) 锁内重读整份配置,定点改 channels,原子写回;
//   4) 重新解析写入结果 + 生产凭据读取器复验,成功才算保存完成;
//   5) 配置提交失败:删本次新件,保旧配置旧密钥;
//   6) 全部提交后回收该账号不再被引用的孤儿受管件。
#pragma once

#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "channel/channel_config.hpp"

namespace lubancode::channel {

// ---------------------------------------------------------------------------
// ChannelSetupRegistry(§6.3 平台接入合同)
// ---------------------------------------------------------------------------

struct ChannelSetupField {
    std::string id;      // app_id / app_secret
    std::string label;   // 展示名(AppID)
    bool sensitive = false;  // true = 隐藏输入
};

struct ChannelSetupPlatform {
    std::string id;            // qqbot
    std::string display_name;  // QQ
    bool implemented = false;  // 有可运行适配器才能进配置向导
    std::vector<ChannelSetupField> fields;
};

// 全部平台(含未实现的,选择界面如实列"尚未支持")。
const std::vector<ChannelSetupPlatform>& ChannelSetupPlatforms();
std::optional<ChannelSetupPlatform> FindChannelSetupPlatform(const std::string& id);

// ---------------------------------------------------------------------------
// ChannelConfigService
// ---------------------------------------------------------------------------

struct ChannelSetupCommitRequest {
    std::string channel_id;
    std::string account_id;
    // 空 = 保留旧值。AppID 非敏感,明文进配置(JSON 本来就存它)。
    std::optional<std::string> app_id;
    // 新密钥:只在服务内过路,落受管文件;不进结果结构。nullopt = 保留
    // 旧密钥(旧 secret_file 原样不动)。
    std::optional<std::string> new_secret;
    // 保存时显式启用渠道与账号(向导先问过用户"是否启用",这里只兑现
    // 明确选择,不暗改开关)。
    bool ensure_enabled = true;
    // 只算差异不落盘(向导保存前显示差异用)。
    bool dry_run = false;
    std::optional<std::string> tools_preset;  // nullopt 保留旧策略；选择后保留 deny
};

struct ChannelSetupCommitResult {
    std::string config_file;             // 实际写入的配置文件路径
    std::string secret_file;             // 提交后生效的 secret_file(绝对路径)
    bool created_account = false;        // 新建账号(走模板)还是已有账号
    std::vector<std::string> changes;    // 差异摘要(人话;不含密钥值)
};

// 稳定码:
//   setup_bad_platform         平台没在注册表/没有可运行适配器
//   setup_bad_id               渠道/账号 id 不合法
//   setup_config_unreadable    配置文件读不出/不是合法 JSON
//   setup_channels_invalid     既有 channels 段解析不过(不碰文件)
//   setup_app_id_invalid       AppID 空/编码/控制字符
//   setup_secret_invalid       密钥空/编码/控制字符/超限
//   setup_busy                 配置锁被别的向导占着
//   setup_secret_write_failed  受管密钥写入失败(凭据服务 detail)
//   setup_config_write_failed  配置原子写失败(旧配置旧密钥保留)
//   setup_verify_failed        写后复验失败(如实报,不谎称保存完成)
//   setup_internal             内部形状错误(channels 子树不是 object 等)
struct ChannelSetupError {
    std::string reason;
    std::string detail;  // 人话;不含密钥值
};

class ChannelConfigService {
public:
    struct Options {
        std::string config_file;             // 全局 config.json 路径
        std::filesystem::path secrets_root;  // <配置根>/secrets
        int lock_timeout_ms = 10000;
    };

    static std::expected<ChannelSetupCommitResult, ChannelSetupError> Commit(
        const Options& options, const ChannelSetupCommitRequest& request);

    // 生产默认 Options(全局 config.json + HomeLubancodeDir/secrets)。
    static std::expected<Options, ChannelSetupError> DefaultOptions();
};

// 既有账号的现况探察(向导显示用;不改任何东西)。channels 来自当前配置
// 解析结果(调用方 LoadFromEnv 后取 config.channels)。
struct ChannelAccountStatus {
    bool exists = false;
    bool enabled = false;
    bool channel_enabled = false;
    bool has_app_id = false;
    bool has_secret_file = false;
    std::string secret_file;      // 原样路径(可能是外部路径)
    bool secret_file_managed = false;  // 落在受管 secrets 根下
    bool secret_file_secure = false;   // 生产读取器的权限结论
    std::string security_detail;       // 不安全时的人话(权限分类)
};
ChannelAccountStatus InspectChannelAccount(const std::map<std::string, ChannelUserConfig>& channels,
                                           const std::filesystem::path& secrets_root,
                                           const std::string& channel_id,
                                           const std::string& account_id);

}  // namespace lubancode::channel
