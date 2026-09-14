// 统一凭据写入服务(QQBot Windows 修复单 §5.2)。
//
// 合同:
//   - 向导与迁移入口的唯一写入路;网络适配器只读凭据,不碰本件。
//   - Windows 创建目录/文件即带安全描述符(owner=当前用户、DACL 关继承、
//     默认仅当前用户);POSIX 目录 0700、文件 0600 并核对 owner。不存在
//     "先写明文再收紧"的窗口。
//   - 新密钥存为独立版本文件(<channel>-<account>.<nonce>.secret);配置
//     原子指向新文件由调用方(ChannelConfigService)完成;本件提供孤儿
//     受管件回收——只清 secrets 根下 *.secret 命名、且不在保留名单里的
//     文件,不碰任何外部路径。
//   - 密钥值只在参数与目标文件之间过路:不进日志、不进错误文案、不进
//     任何副本容器。错误只带路径与稳定码。
//   - 收紧(§5.3)只改既有文件的权限描述符,不重写内容;owner 不符时
//     准确报错——不提权、不夺所有权、不改父目录。
#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lubancode::channel {

// 稳定码:
//   credential_store_bad_id          渠道/账号 id 不能进文件名
//   credential_store_dir_failed      受管目录建不成/被非目录占着/是链接
//   credential_store_write_failed    版本文件创建或写入失败
//   credential_store_verify_failed   写后生产读取器复验不过(理论上不可达;
//                                    真发生即如实报,不谎报保存完成)
//   credential_store_tighten_failed  收紧失败(owner 不符/无 WRITE_DAC/IO 错)
//   credential_store_classify_failed 分类本身失败
//   credential_store_cleanup_failed  孤儿回收失败(不推翻已完成的提交)
struct CredentialStoreError {
    std::string reason;
    std::string detail;  // 脱敏:可带路径,绝不带内容
};

class CredentialStore {
public:
    // secrets_root:受管凭据根(生产 = <配置根>/secrets,见 DefaultSecretsRoot)。
    explicit CredentialStore(std::filesystem::path secrets_root);

    // 当前用户的默认受管根:<HomeLubancodeDir>/secrets。拿不到主目录返回
    // nullopt(调用方明报,不硬编码别的路径)。
    static std::optional<std::filesystem::path> DefaultSecretsRoot();

    // 受管版本文件的完整路径(只拼名,不落盘)。
    std::filesystem::path ManagedSecretPath(const std::string& channel_id,
                                            const std::string& account_id,
                                            const std::string& nonce) const;

    // 写一枚新版本受管密钥:建目录(幂等、带描述符)→ 以严格权限创建全新
    // 文件(CREATE_NEW/O_EXCL,不跟随链接)→ fsync → 用生产检查
    // (CheckCredentialFileSecurity)复验。secret 内容须已过校验(非空、
    // UTF-8、无控制字符、≤ kCredentialFileMaxBytes——本件再守一道)。
    std::expected<std::filesystem::path, CredentialStoreError> WriteNewManagedSecret(
        const std::string& channel_id, const std::string& account_id,
        const std::string& secret) const;

    // 收紧一枚既有文件的权限(§5.3):Windows 重设 owner+DACL(关继承,
    // 仅当前用户;owner 不符时不动手报错),POSIX chmod 0600。收完用生产
    // 读取器复验。不重写内容,不递归改父目录。
    std::expected<void, CredentialStoreError> TightenFilePermissions(
        const std::filesystem::path& path) const;

    // 孤儿回收:secrets 根下 *.secret 且不在 keep(绝对路径集合)里的,
    // 判为程序自己的孤儿受管件并删除。只动 *.secret 命名,其余文件一概
    // 不碰。返回实际删除的路径(记账用)。
    std::expected<std::vector<std::filesystem::path>, CredentialStoreError>
    RemoveUnreferencedManagedSecrets(const std::vector<std::filesystem::path>& keep) const;

    const std::filesystem::path& secrets_root() const { return secrets_root_; }

private:
    std::filesystem::path secrets_root_;
};

}  // namespace lubancode::channel
