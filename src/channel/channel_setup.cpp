// channel_setup.hpp 的实现。channels 子树的定点更新在原始 JSON 上做
//(不经 ChannelUserConfig 结构往返——未知字段要在,其他账号要在,解析
// 端只用来"先验旧配置合法 + 提交后复验")。
#include "channel/channel_setup.hpp"

#include <algorithm>
#include <system_error>

#include "channel/credential_store.hpp"
#include "channel/credentials.hpp"
#include "config/config.hpp"
#include "platform/paths.hpp"
#include "platform/secure_file.hpp"
#include "platform/text_encoding.hpp"

namespace lubancode::channel {

namespace {

ChannelSetupError Fail(std::string reason, std::string detail) {
    return ChannelSetupError{std::move(reason), std::move(detail)};
}

// QQ 模板账号 → 原始 JSON(新账号的骨架;逐字段显式,与 MakeQqTemplateAccount
// 同源,不在第二处手抄取值)。
nlohmann::json TemplateAccountToJson() {
    const ChannelAccountUserConfig template_account = MakeQqTemplateAccount();
    nlohmann::json account;
    account["enabled"] = false;  // Commit 里按 ensure_enabled 显式置位
    account["transport"] = template_account.transport;
    account["dm_policy"] = DmPolicyName(template_account.dm_policy);
    account["group_policy"] = GroupPolicyName(template_account.group_policy);
    account["allow_bots"] = template_account.allow_bots;
    account["require_mention"] = template_account.require_mention;
    account["reply"] = nlohmann::json{{"mode", ReplyModeName(template_account.reply.mode)},
                                      {"tool_progress", template_account.reply.tool_progress}};
    // 保存档位名，解析时展开同源名单，用户不必追着新增工具改 JSON。
    account["tools"] = nlohmann::json{{"preset", template_account.tools.preset}};
    return account;
}

// 单行输入的校验(§5.1:非空、编码、控制字符;错误说明不引用用户输入)。
std::optional<ChannelSetupError> ValidateSingleLine(const std::string& value,
                                                    const char* what, std::size_t max_bytes,
                                                    const std::string& reason_code) {
    if (value.empty()) {
        return Fail(reason_code, std::string(what) + " 不能为空");
    }
    if (!platform::IsValidUtf8(value)) {
        return Fail(reason_code, std::string(what) + " 不是合法 UTF-8 文本");
    }
    if (value.size() > max_bytes) {
        return Fail(reason_code,
                    std::string(what) + " 超过长度上限 " + std::to_string(max_bytes) + " 字节");
    }
    for (const char c : value) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) {
            return Fail(reason_code, std::string(what) + " 含控制字符");
        }
    }
    return std::nullopt;
}

// channels 子树里这条账号现在指向哪些受管 secret_file(孤儿回收的保留
// 名单:新配置里所有账号的 secret_file 都要保住,不只本账号)。
std::vector<std::filesystem::path> CollectSecretFileReferences(
    const std::map<std::string, ChannelUserConfig>& channels) {
    std::vector<std::filesystem::path> refs;
    for (const auto& [channel_id, channel] : channels) {
        for (const auto& [account_id, account] : channel.accounts) {
            if (account.secret_file.has_value() && !account.secret_file->empty()) {
                refs.push_back(platform::Utf8ToPath(*account.secret_file));
            }
        }
    }
    return refs;
}

std::string DescribeBool(bool value) { return value ? "true" : "false"; }

}  // namespace

// ---------------------------------------------------------------------------
// ChannelSetupRegistry
// ---------------------------------------------------------------------------

const std::vector<ChannelSetupPlatform>& ChannelSetupPlatforms() {
    static const std::vector<ChannelSetupPlatform> platforms = {
        ChannelSetupPlatform{
            "qqbot",
            "QQ",
            true,  // Q1 起有进程内适配器(channel_gateway_wiring 只认 qqbot)
            {ChannelSetupField{"app_id", "AppID", false},
             ChannelSetupField{"app_secret", "AppSecret", true}},
        },
        ChannelSetupPlatform{
            "feishu",
            "飞书",
            false,  // 适配器未实现:可显示"尚未支持",不可进假成功配置
            {},
        },
    };
    return platforms;
}

std::optional<ChannelSetupPlatform> FindChannelSetupPlatform(const std::string& id) {
    for (const ChannelSetupPlatform& platform : ChannelSetupPlatforms()) {
        if (platform.id == id) {
            return platform;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// InspectChannelAccount
// ---------------------------------------------------------------------------

ChannelAccountStatus InspectChannelAccount(const std::map<std::string, ChannelUserConfig>& channels,
                                           const std::filesystem::path& secrets_root,
                                           const std::string& channel_id,
                                           const std::string& account_id) {
    ChannelAccountStatus status;
    const auto channel_it = channels.find(channel_id);
    if (channel_it == channels.end()) {
        return status;
    }
    status.channel_enabled = channel_it->second.enabled;
    const auto account_it = channel_it->second.accounts.find(account_id);
    if (account_it == channel_it->second.accounts.end()) {
        return status;
    }
    const ChannelAccountUserConfig& account = account_it->second;
    status.exists = true;
    status.enabled = account.enabled;
    status.has_app_id = !account.app_id.empty();
    if (account.secret_file.has_value() && !account.secret_file->empty()) {
        status.has_secret_file = true;
        status.secret_file = *account.secret_file;
        std::error_code ec;
        const std::filesystem::path path = platform::Utf8ToPath(status.secret_file);
        const auto canonical = std::filesystem::weakly_canonical(path, ec);
        const std::filesystem::path effective = ec ? path : canonical;
        // 受管判定 = secrets 根前缀,且恰在根上或后跟一段分隔(防"secrets2"
        // 这类前缀撞车)。
        const std::string effective_key = effective.string();
        const std::string root_key = secrets_root.string();
        status.secret_file_managed =
            effective_key.rfind(root_key, 0) == 0 &&
            (effective_key.size() == root_key.size() ||
             effective_key[root_key.size()] == std::filesystem::path::preferred_separator);
        if (std::filesystem::is_regular_file(effective, ec)) {
            const auto report = InspectCredentialFileSecurity(effective);
            status.secret_file_secure = report.status == CredentialFileSecurityReport::Status::Ok;
            if (!status.secret_file_secure) {
                status.security_detail = report.detail;
            }
        }
    }
    return status;
}

// ---------------------------------------------------------------------------
// ChannelConfigService
// ---------------------------------------------------------------------------

std::expected<ChannelConfigService::Options, ChannelSetupError> ChannelConfigService::DefaultOptions() {
    const auto config_file = config::GlobalConfigFilePath();
    if (!config_file.has_value()) {
        return std::unexpected(
            Fail("setup_config_unreadable",
                 "找不到用户主目录(Windows 下是 %USERPROFILE%),也没设 LUBANCODE_HOME,没法定位全局配置"));
    }
    Options options;
    options.config_file = *config_file;
    const auto secrets_root = CredentialStore::DefaultSecretsRoot();
    if (!secrets_root.has_value()) {
        return std::unexpected(
            Fail("setup_config_unreadable",
                 "找不到用户主目录,没法定位受管凭据根(<配置根>/secrets)"));
    }
    options.secrets_root = *secrets_root;
    return options;
}

std::expected<ChannelSetupCommitResult, ChannelSetupError> ChannelConfigService::Commit(
    const Options& options, const ChannelSetupCommitRequest& request) {
    // 0) 平台与 id 的守门。
    const auto platform = FindChannelSetupPlatform(request.channel_id);
    if (!platform.has_value()) {
        return std::unexpected(Fail("setup_bad_platform",
                                    "未知平台: " + request.channel_id + "(认得: qqbot)"));
    }
    if (!platform->implemented) {
        return std::unexpected(
            Fail("setup_bad_platform", "平台 " + platform->display_name + "(" + request.channel_id +
                                          ")尚未支持,不能进配置向导"));
    }
    if (!IsValidChannelId(request.channel_id) || !IsValidChannelAccountId(request.account_id)) {
        return std::unexpected(Fail(
            "setup_bad_id",
            "渠道/账号 id 须是单段名(无路径分隔符/控制字符,长度 ≤ 64): " + request.account_id));
    }
    if (request.app_id.has_value()) {
        if (const auto invalid = ValidateSingleLine(*request.app_id, "AppID", 128, "setup_app_id_invalid")) {
            return std::unexpected(*invalid);
        }
    }
    if (request.new_secret.has_value()) {
        if (const auto invalid =
                ValidateSingleLine(*request.new_secret, "AppSecret", kCredentialFileMaxBytes, "setup_secret_invalid")) {
            return std::unexpected(*invalid);
        }
    }

    ChannelSetupCommitResult result;
    result.config_file = options.config_file;

    // 1) 进程锁(不覆盖并发向导)。锁文件落 secrets 根(那棵树是程序自己的)。
    //    dry_run 不写盘,不抢锁。
    std::optional<platform::InterProcessFileLock> lock;
    if (!request.dry_run) {
        if (const auto dir = platform::CreateSecureDirectory(options.secrets_root);
            !dir.has_value()) {
            return std::unexpected(Fail("setup_secret_write_failed", dir.error().message));
        }
        lock.emplace(options.secrets_root / ".setup.lock", options.lock_timeout_ms);
        if (!lock->holds()) {
            return std::unexpected(
                Fail("setup_busy", "另一个配置向导正在写(锁 " +
                                       platform::PathToUtf8(options.secrets_root / ".setup.lock") +
                                       "),稍后再试"));
        }
    }

    // 2) 锁内读整份配置 + 验既有 channels 段合法(不合法不动手)。
    auto root = config::ReadConfigObjectForTargetedUpdate(options.config_file);
    if (!root.has_value()) {
        return std::unexpected(Fail("setup_config_unreadable", root.error()));
    }
    nlohmann::json channels_json = nlohmann::json::object();
    if (root->contains("channels")) {
        if (!(*root)["channels"].is_object()) {
            return std::unexpected(
                Fail("setup_channels_invalid",
                     "配置文件 " + options.config_file + " 里的 channels 字段必须是一个 JSON object"));
        }
        channels_json = (*root)["channels"];
        std::string channels_error;
        const auto parsed = ParseChannelsUserConfig(channels_json, options.config_file,
                                                    &channels_error);
        if (!parsed.has_value()) {
            return std::unexpected(Fail("setup_channels_invalid", channels_error));
        }
    }

    nlohmann::json* channel_json = nullptr;
    if (channels_json.contains(request.channel_id)) {
        if (!channels_json[request.channel_id].is_object()) {
            return std::unexpected(Fail("setup_internal",
                                        "channels." + request.channel_id + " 必须是一个 JSON object"));
        }
        channel_json = &channels_json[request.channel_id];
    }
    nlohmann::json fresh_channel = nlohmann::json::object();
    if (channel_json == nullptr) {
        channel_json = &fresh_channel;
    }
    nlohmann::json* accounts_json = nullptr;
    if (channel_json->contains("accounts")) {
        if (!(*channel_json)["accounts"].is_object()) {
            return std::unexpected(Fail("setup_internal",
                                        "channels." + request.channel_id + ".accounts 必须是一个 JSON object"));
        }
        accounts_json = &(*channel_json)["accounts"];
    }
    nlohmann::json fresh_accounts = nlohmann::json::object();
    if (accounts_json == nullptr) {
        accounts_json = &fresh_accounts;
    }

    if (request.tools_preset && !ChannelToolsPreset(*request.tools_preset)) {
        return std::unexpected(Fail("setup_tools_invalid", "工具模式只认 readonly/ask/auto"));
    }
    const bool created_account = !accounts_json->contains(request.account_id);
    result.created_account = created_account;

    // 必填前置守门:更新后仍无 AppID(本次没给、旧值也没有)就明报——
    // 必须先于密钥写入,不然半截流程会留下没人引用的密钥文件。
    const nlohmann::json* existing_account_json = nullptr;
    if (!created_account) {
        existing_account_json = &(*accounts_json)[request.account_id];
    }
    const bool existing_has_app_id =
        existing_account_json != nullptr && existing_account_json->contains("app_id") &&
        (*existing_account_json)["app_id"].is_string() &&
        !(*existing_account_json)["app_id"].get<std::string>().empty();
    if (!request.app_id.has_value() && !existing_has_app_id) {
        return std::unexpected(
            Fail("setup_app_id_invalid", "AppID 缺失:请输入 AppID(已有账号可直接回车保留旧值)"));
    }

    // 3) 新密钥先落独立版本文件(先有合格文件,配置才允许指向它)。
    //    dry_run 不写盘:差异预览不落密钥。
    std::optional<std::filesystem::path> new_secret_file;
    if (request.new_secret.has_value() && !request.dry_run) {
        const CredentialStore store(options.secrets_root);
        auto written = store.WriteNewManagedSecret(request.channel_id, request.account_id,
                                                   *request.new_secret);
        if (!written.has_value()) {
            return std::unexpected(Fail("setup_secret_write_failed",
                                        written.error().reason + ": " + written.error().detail));
        }
        new_secret_file = *written;
    }

    // 4) 定点改账号(已有账号只动选定字段;新账号走模板骨架)。差异
    //    (changes)与判定都基于"改前"快照,改完再装回。
    const bool channel_enabled_before =
        channel_json->contains("enabled") && (*channel_json)["enabled"].is_boolean() &&
        (*channel_json)["enabled"].get<bool>();
    const bool set_default_account =
        !channel_json->contains("default_account") ||
        !(*channel_json)["default_account"].is_string() ||
        (*channel_json)["default_account"].get<std::string>().empty();
    const nlohmann::json* old_account = nullptr;
    bool account_enabled_before = false;
    if (!created_account) {
        old_account = &(*accounts_json)[request.account_id];
        account_enabled_before = old_account->contains("enabled") &&
                                 (*old_account)["enabled"].is_boolean() &&
                                 (*old_account)["enabled"].get<bool>();
    }

    nlohmann::json account_json =
        created_account ? TemplateAccountToJson() : (*accounts_json)[request.account_id];
    if (request.tools_preset) {
        auto& tools = account_json["tools"];
        if (tools.is_null()) tools = nlohmann::json::object();
        tools.erase("allow");
        tools.erase("approve");
        tools["preset"] = *request.tools_preset;
    }
    if (request.app_id.has_value()) {
        account_json["app_id"] = *request.app_id;
    }
    if (new_secret_file.has_value()) {
        account_json["secret_file"] = platform::PathToUtf8(*new_secret_file);
        // 受管文件接管凭据来源:inline 明文与 env 引用一并清掉,不给旧
        // 明文留第二落点(文件优先级本来就压过它们,留着只是隐患)。
        account_json.erase("secret");
        account_json.erase("secret_env");
    }
    if (request.ensure_enabled) {
        account_json["enabled"] = true;
    }

    auto note_change = [&](const std::string& text) { result.changes.push_back(text); };
    if (created_account) {
        note_change("新建账号 " + request.channel_id + "/" + request.account_id +
                    "(QQ 模板:websocket、私聊配对、群聊禁用、final 回复、操作前询问)");
    }
    if (request.tools_preset) {
        const auto& preset = *request.tools_preset;
        note_change("工具模式: " + std::string(preset == "ask" ? "操作前询问" :
                    preset == "auto" ? "自动执行常用工具" : "只读查询") +
                    "；保留禁止项与渠道/路由限制，重启 Gateway 后生效");
    }
    if (request.app_id.has_value() &&
        (old_account == nullptr || !old_account->contains("app_id") ||
         !(*old_account)["app_id"].is_string() ||
         (*old_account)["app_id"].get<std::string>() != *request.app_id)) {
        note_change("app_id 已更新");
    }
    if (new_secret_file.has_value()) {
        note_change("AppSecret 已保存到受管文件: " + platform::PathToUtf8(*new_secret_file));
        result.secret_file = platform::PathToUtf8(*new_secret_file);
    } else if (request.new_secret.has_value()) {
        // dry_run:不落盘,只预告去处。
        note_change("AppSecret 将保存到受管位置(" + platform::PathToUtf8(options.secrets_root) +
                    " 下,提交后生成)");
    } else if (old_account != nullptr && old_account->contains("secret_file") &&
               (*old_account)["secret_file"].is_string()) {
        result.secret_file = (*old_account)["secret_file"].get<std::string>();
    }
    if (request.ensure_enabled) {
        if (!account_enabled_before) {
            note_change("账号 enabled: " + DescribeBool(account_enabled_before) + " -> true");
        }
        if (!channel_enabled_before) {
            note_change("channels." + request.channel_id +
                        ".enabled: " + DescribeBool(channel_enabled_before) + " -> true");
        }
    }
    if (set_default_account) {
        note_change("default_account = " + request.account_id);
    }

    if (request.dry_run) {
        return result;  // 差异到手,一页未写
    }

    // 5) 装回子树(纯值语义,不玩指针别名)→ 原子写回。渠道层默认账号
    //    只在原本缺位时立本次账号,不偷别人的默认位。
    nlohmann::json accounts_value = std::move(*accounts_json);
    accounts_value[request.account_id] = std::move(account_json);
    nlohmann::json channel_value = std::move(*channel_json);
    channel_value["accounts"] = std::move(accounts_value);
    if (request.ensure_enabled && !channel_enabled_before) {
        channel_value["enabled"] = true;
    }
    if (set_default_account) {
        channel_value["default_account"] = request.account_id;
    }
    channels_json[request.channel_id] = std::move(channel_value);
    (*root)["channels"] = std::move(channels_json);

    const auto written = config::WriteConfigObjectAtomic(options.config_file, *root);
    if (!written.has_value()) {
        // 配置写失败:旧配置在(原子写失败目标保持原样),删本次新件,
        // 旧密钥文件一个不动。
        if (new_secret_file.has_value()) {
            std::error_code ignored;
            std::filesystem::remove(*new_secret_file, ignored);
        }
        return std::unexpected(Fail("setup_config_write_failed", written.error()));
    }

    // 6) 复验:重读配置 + 生产凭据读取器真的能读出密钥,才算保存完成。
    {
        auto verify_root = config::ReadConfigObjectForTargetedUpdate(options.config_file);
        if (!verify_root.has_value() || !verify_root->contains("channels") ||
            !(*verify_root)["channels"].is_object()) {
            return std::unexpected(Fail("setup_verify_failed", verify_root.has_value()
                                                                    ? "channels 段写入后读不回"
                                                                    : verify_root.error()));
        }
        std::string verify_error;
        const auto parsed = ParseChannelsUserConfig((*verify_root)["channels"],
                                                    options.config_file, &verify_error);
        if (!parsed.has_value()) {
            return std::unexpected(Fail("setup_verify_failed",
                                        "写入结果解析不过: " + verify_error));
        }
        const auto channel_it = parsed->find(request.channel_id);
        if (channel_it == parsed->end()) {
            return std::unexpected(Fail("setup_verify_failed",
                                        "写入结果里找不到渠道 " + request.channel_id));
        }
        const auto account_it = channel_it->second.accounts.find(request.account_id);
        if (account_it == channel_it->second.accounts.end()) {
            return std::unexpected(Fail("setup_verify_failed", "写入结果里找不到目标账号"));
        }
        const auto resolved = ResolveChannelCredential(account_it->second);
        if (!resolved.has_value()) {
            return std::unexpected(
                Fail("setup_verify_failed",
                     "生产凭据读取器复验失败(" + resolved.error().reason + "): " +
                         resolved.error().detail));
        }
        // 全部提交成功:回收不再被引用的孤儿受管件(保留名单 = 新配置里
        // 所有账号引用的 secret_file;外部路径不在 secrets 根下,自然删不着)。
        const CredentialStore store(options.secrets_root);
        auto removed = store.RemoveUnreferencedManagedSecrets(CollectSecretFileReferences(*parsed));
        if (!removed.has_value()) {
            // 回收失败不推翻提交:如实报失败原因,配置与密钥已可用。
            return std::unexpected(Fail("setup_verify_failed",
                                        "配置已保存,但孤儿受管件回收失败: " +
                                            removed.error().detail));
        }
    }
    return result;
}

}  // namespace lubancode::channel
