// assistant_channels.hpp 的实现。
#include "app/assistant_channels.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "app/channel_connection_reporter.hpp"
#include "app_server/dispatcher.hpp"
#include "app_server/protocol.hpp"
#include "channel/pairing.hpp"
#include "cli/channel_pairing_command.hpp"  // JudgePairingGate(锁探测门,与 CLI 同尺)
#include "cli/channel_status_command.hpp"   // 四态投影与配置探针(判据单一真源)
#include "gateway/pairing_command.hpp"
#include "gateway/process.hpp"
#include "gateway/service.hpp"
#include "platform/process.hpp"
#include "trajectory/session_lock.hpp"

namespace lubancode::app {

namespace {

std::int64_t DefaultNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 入参读字(缺键/类型不对给缺省;严格校验由各 handler 自管)。
std::string ReadJsonString(const nlohmann::json& params, const char* key) {
    if (params.is_object() && params.contains(key) && params[key].is_string()) {
        return params[key].get<std::string>();
    }
    return std::string();
}

// 连接快照文件 → JSON(不在/读不懂回 nullopt;零建目录零写盘)。
std::optional<nlohmann::json> ReadConnectionSnapshot(
    const std::filesystem::path& channels_root, const std::string& channel_id,
    const std::string& account_id) {
    const std::filesystem::path file =
        ChannelConnectionReporter::SnapshotFilePath(channels_root, channel_id, account_id);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec) || ec) {
        return std::nullopt;
    }
    std::ifstream stream(file, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const auto parsed = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded()) {
        return std::nullopt;
    }
    return parsed;
}

// 对号串(命令/回执文件名;单段 [A-Za-z0-9-],与 CLI 同式)。
std::string NewPairingCommandId() {
    std::random_device device;
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%llx-%lx-%lx",
                  static_cast<unsigned long long>(DefaultNowMs()),
                  static_cast<unsigned long>(device()), static_cast<unsigned long>(device()));
    return buffer;
}

}  // namespace

AssistantChannelFace::AssistantChannelFace(Options options) : options_(std::move(options)) {
    if (!options_.now_ms) {
        options_.now_ms = [] { return DefaultNowMs(); };
    }
    if (!options_.is_alive) {
        options_.is_alive = [](unsigned long pid) { return platform::IsProcessAlive(pid); };
    }
}

nlohmann::json AssistantChannelFace::BuildAccountProjection(
    const std::string& channel_id, const std::string& account_id,
    const channel::ChannelUserConfig* channel_config, bool include_detail) const {
    // 四态输入(与 CLI `channel status` 同一份探针与判据)。
    cli::ChannelFourStateInput four_state;
    if (channel_config != nullptr && channel_config->accounts.count(account_id) > 0) {
        const auto& account = channel_config->accounts.at(account_id);
        if (!channel_config->enabled || !account.enabled) {
            four_state.config_detail = "渠道或账号未启用";
        } else if (account.app_id.empty()) {
            four_state.config_detail = "AppID 未填";
        } else if (channel::DescribeCredentialSource(account) ==
                   channel::CredentialSource::Missing) {
            four_state.config_detail = "凭据来源未配(secret_file/secret_env)";
        } else {
            four_state.account_configured = true;
        }
    } else {
        four_state.config_detail = "账号不在配置里";
    }
    // 连接快照裁决(#81 的合同:connected 且进程活且新鲜才在线)。
    const auto snapshot =
        ReadConnectionSnapshot(options_.channels_state_root, channel_id, account_id);
    const cli::ChannelStatusVerdict verdict = cli::JudgeChannelStatus(
        snapshot.has_value() ? &*snapshot : nullptr, channel_id, account_id,
        options_.now_ms(), options_.is_alive);
    four_state.online = verdict.online;
    if (!verdict.online && !verdict.lines.empty()) {
        four_state.online_detail = verdict.lines.front();
    } else if (verdict.online && snapshot.has_value() && snapshot->contains("boot_id") &&
               snapshot->at("boot_id").is_string()) {
        four_state.online_detail = "boot " + snapshot->at("boot_id").get<std::string>();
    }
    // pairing 账(计数投影 + 待审清单;坏账如实)。
    const std::filesystem::path account_dir =
        options_.channels_state_root / channel_id / account_id;
    const auto pairing = channel::PairingStore::ReadProjection(account_dir);
    four_state.pairing_present = pairing.present;
    four_state.pairing_parse_ok = pairing.parse_ok;
    four_state.pairing_approved = pairing.approved;
    four_state.pairing_pending = pairing.pending;
    // 模型(第四步;probe 未注入 = 该步如实"未探",不冒充)。
    if (options_.model_probe) {
        const auto model = options_.model_probe();
        four_state.model_configured = model.first;
        four_state.model_detail = model.second;
    } else {
        four_state.model_detail = "本实例未提供模型探针";
    }
    // 最近真实执行结局(P1:与 CLI channel status 同一份判据——ingress 账
    // 只读投影,不拿"配置齐全"冒充"能回复")。
    cli::DeriveRecentTurnOutcome(channel::ReadChannelIngressRecentChain(account_dir, 32),
                                 &four_state);

    const cli::ChannelFourStateView four =
        cli::BuildChannelFourState(channel_id, account_id, four_state);

    nlohmann::json item;
    item["channelId"] = channel_id;
    item["accountId"] = account_id;
    item["fourState"] = four.report;
    // 待批准清单(#90 ReadPendingList:sender + 过期时刻;code_hash 不出账)。
    nlohmann::json pending_list = nlohmann::json::array();
    if (pairing.present && pairing.parse_ok) {
        const auto pending = channel::PairingStore::ReadPendingList(account_dir);
        for (const auto& entry : pending.pending) {
            pending_list.push_back(nlohmann::json{{"senderId", entry.sender_id},
                                                  {"expiresAtMs", entry.expires_at_ms}});
        }
    }
    item["pendingPairings"] = std::move(pending_list);
    item["pairingParseOk"] = pairing.parse_ok;
    if (include_detail) {
        nlohmann::json lines = nlohmann::json::array();
        for (const std::string& line : four.lines) {
            lines.push_back(line);
        }
        item["fourStateLines"] = std::move(lines);
        nlohmann::json detail_lines = nlohmann::json::array();
        for (const std::string& line : verdict.lines) {
            detail_lines.push_back(line);
        }
        item["connectionDetail"] = std::move(detail_lines);
        // 快照的脱敏投影(它本身脱敏——#81 合同:不落 AppSecret/token)。
        if (snapshot.has_value()) {
            item["connectionSnapshot"] = *snapshot;
        }
        item["online"] = verdict.online;
    }
    return item;
}

nlohmann::json AssistantChannelFace::HandleChannelList(const nlohmann::json& params,
                                                        int& out_error_code,
                                                        std::string& out_error_message) {
    (void)params;
    if (options_.channels_state_root.empty()) {
        out_error_code = app_server::kErrInternalError;
        out_error_message = "channel/list: 渠道状态根不可用";
        return nlohmann::json();
    }
    nlohmann::json result;
    // 配置探针:全局 config.json 的 channels 段(与 CLI channel status 同源)。
    if (options_.config_path.has_value()) {
        const auto probe = cli::LoadChannelsUserConfigFromFile(*options_.config_path);
        if (!probe.ok) {
            result["channelsError"] = probe.detail;
        }
        nlohmann::json accounts = nlohmann::json::array();
        if (probe.ok) {
            for (const auto& [channel_id, channel_config] : probe.channels) {
                for (const auto& account_id : channel_config.accounts) {
                    accounts.push_back(BuildAccountProjection(channel_id, account_id.first,
                                                              &channel_config,
                                                              /*include_detail=*/false));
                }
            }
        }
        result["accounts"] = std::move(accounts);
    } else {
        result["channelsError"] = "全局配置路径不可用";
        result["accounts"] = nlohmann::json::array();
    }
    // 安全边界文案:凭据不在网页录入(单 §五;前端也写死同一条)。
    result["credentialHint"] =
        "渠道凭据(AppSecret)不在网页录入——用 lubancode im 或 lubancode channel setup "
        "配置,这里只读状态。";
    return result;
}

nlohmann::json AssistantChannelFace::HandleChannelStatus(const nlohmann::json& params,
                                                          int& out_error_code,
                                                          std::string& out_error_message) {
    const std::string channel_id = ReadJsonString(params, "channelId");
    const std::string account_id = ReadJsonString(params, "accountId");
    if (channel_id.empty() || account_id.empty()) {
        out_error_code = app_server::kErrInvalidParams;
        out_error_message = "channel/status: channelId 与 accountId 都要有";
        return nlohmann::json();
    }
    if (!channel::IsValidChannelId(channel_id) || !channel::IsValidChannelAccountId(account_id)) {
        out_error_code = app_server::kErrInvalidParams;
        out_error_message = "channel/status: 渠道/账号 id 不合法(非空、无路径段)";
        return nlohmann::json();
    }
    if (options_.channels_state_root.empty()) {
        out_error_code = app_server::kErrInternalError;
        out_error_message = "channel/status: 渠道状态根不可用";
        return nlohmann::json();
    }
    // 配置探针(单账号):不在册也要如实给四态(卡第一步)。channels 段
    // 读不懂 → result 里如实报(channelsError),四态的第一步照实卡。
    // probe 整体存活到投影用完(取 &found->second 进的是它的 map——局部
    // 拷贝出了函数就悬垂,UB 不能碰)。
    std::optional<cli::ChannelsConfigProbe> probe;
    if (options_.config_path.has_value()) {
        probe = cli::LoadChannelsUserConfigFromFile(*options_.config_path);
    }
    const channel::ChannelUserConfig* account_config = nullptr;
    if (probe.has_value() && probe->ok) {
        const auto found = probe->channels.find(channel_id);
        if (found != probe->channels.end()) {
            account_config = &found->second;
        }
    }
    nlohmann::json result =
        BuildAccountProjection(channel_id, account_id, account_config, /*include_detail=*/true);
    if (probe.has_value() && !probe->ok) {
        result["channelsError"] = probe->detail;
    }
    return result;
}

nlohmann::json AssistantChannelFace::HandleChannelPairingRespond(
    const nlohmann::json& params, int& out_error_code, std::string& out_error_message) {
    if (!params.is_object()) {
        out_error_code = app_server::kErrInvalidParams;
        out_error_message = "channel/pairing/respond: params 须是对象";
        return nlohmann::json();
    }
    const std::string channel_id = ReadJsonString(params, "channelId");
    const std::string account_id = ReadJsonString(params, "accountId");
    const std::string token = ReadJsonString(params, "token");
    const std::string action = ReadJsonString(params, "action");
    if (channel_id.empty() || account_id.empty() || token.empty() ||
        (action != "approve" && action != "reject")) {
        out_error_code = app_server::kErrInvalidParams;
        out_error_message =
            "channel/pairing/respond: channelId、accountId、token 必填,action 须是 "
            "approve 或 reject";
        return nlohmann::json();
    }
    if (!channel::IsValidChannelId(channel_id) || !channel::IsValidChannelAccountId(account_id)) {
        out_error_code = app_server::kErrInvalidParams;
        out_error_message = "channel/pairing/respond: 渠道/账号 id 不合法";
        return nlohmann::json();
    }
    // 投递门(与 CLI `channel pairing` 同一道):锁不在/持有者死透/锁读不懂
    // 都不投——不许拿空 manager 冒充批准成功。
    std::error_code ec;
    const bool lock_present =
        std::filesystem::exists(options_.gateway_paths.lock_file, ec) && !ec;
    std::string lock_error;
    const auto holder =
        lock_present ? gateway::ReadGatewayLockFile(options_.gateway_paths.lock_file, &lock_error)
                     : std::nullopt;
    bool holder_alive = false;
    if (holder.has_value()) {
        const trajectory::SessionLockOwner owner{holder->pid, holder->start_token, 0};
        holder_alive =
            trajectory::ProbeLockHolder(owner) != trajectory::LockHolderState::Dead;
    }
    const cli::ChannelPairingGate gate =
        cli::JudgePairingGate(lock_present, holder.has_value(), holder_alive,
                              holder.has_value() ? holder->boot_id : std::string(),
                              holder.has_value() ? holder->pid : 0);
    if (gate.status != cli::PairingGateStatus::Submit) {
        out_error_code = app_server::kErrInternalError;
        out_error_message = "channel/pairing/respond: " + gate.detail;
        return nlohmann::json();
    }
    // 投命令(带目标 boot_id;持锁 gateway 消费后写回执——同一控制面,
    // 与 CLI 同一份合同,不另开批准口)。
    gateway::GatewayPairingCommand command;
    command.boot_id = gate.boot_id;
    command.command_id = NewPairingCommandId();
    command.action = action;
    command.channel_id = channel_id;
    command.account_id = account_id;
    command.token = token;
    command.requested_at_ms = options_.now_ms();
    const std::string write_error =
        gateway::WritePairingCommand(options_.gateway_paths.control_dir, command);
    if (!write_error.empty()) {
        out_error_code = app_server::kErrInternalError;
        out_error_message = "channel/pairing/respond: 命令写不进控制面——" + write_error;
        return nlohmann::json();
    }
    // 等回执(有界轮询;读到即被 TakePairingCommandResult 删走)。
    const std::int64_t deadline = options_.now_ms() + options_.pairing_timeout_ms;
    while (options_.now_ms() < deadline) {
        std::string result_error;
        const auto receipt = gateway::TakePairingCommandResult(
            options_.gateway_paths.control_dir, command.command_id, &result_error);
        if (receipt.has_value()) {
            if (receipt->ok) {
                nlohmann::json result;
                result["resolved"] = true;
                result["action"] = action;
                result["senderId"] = receipt->sender_id;
                result["note"] = "批准不会自动补跑对方已发的消息——让 TA 重新发一遍。";
                return result;
            }
            nlohmann::json result;
            result["resolved"] = false;
            result["reason"] = receipt->error;
            result["detail"] = receipt->detail;
            return result;
        }
        if (!result_error.empty()) {
            out_error_code = app_server::kErrInternalError;
            out_error_message = "channel/pairing/respond: 回执读不懂——" + result_error;
            return nlohmann::json();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    out_error_code = app_server::kErrInternalError;
    out_error_message = "channel/pairing/respond: 命令已投出但没有回执(等了 " +
                        std::to_string(options_.pairing_timeout_ms / 1000) +
                        " 秒)——gateway 在跑却没消费,可能是渠道账号未装配;如需重试请再点一次。";
    return nlohmann::json();
}

nlohmann::json AssistantChannelFace::HandleServiceStatus(const nlohmann::json& params,
                                                          int& out_error_code,
                                                          std::string& out_error_message) {
    (void)params;
    // V4 服务安装的只读投影:install.json(profile 树内,升级回滚的对账
    // 凭据)+ gateway 实例活态(锁文件读 + 探活)。零外部命令零写盘;
    // 注册状态的完整对账(schtasks/systemctl 对 install.json)走
    // `lubancode gateway doctor`,页面不代跑安装/卸载。
    nlohmann::json result;
    const std::filesystem::path record_file =
        options_.gateway_paths.profile_dir / "service" / "install.json";
    std::error_code ec;
    bool installed = false;
    if (std::filesystem::is_regular_file(record_file, ec) && !ec) {
        std::string load_error;
        const auto record = gateway::ServiceInstallRecord::Load(record_file, &load_error);
        if (record.has_value()) {
            installed = true;
            nlohmann::json record_json;
            record_json["platform"] = record->platform;
            record_json["profile"] = record->profile;
            record_json["exePath"] = record->exe_path;
            record_json["lubancodeVersion"] = record->lubancode_version;
            record_json["gatewayRoot"] = record->gateway_root;
            record_json["installedAtMs"] = record->installed_at_ms;
            result["installRecord"] = std::move(record_json);
        } else {
            result["installRecordError"] =
                load_error.empty() ? std::string("安装记录读不懂") : load_error;
        }
    }
    result["installed"] = installed;
    // gateway 实例活态(锁在 + 持有进程活)。与 assistant 的任务面互斥是
    // 同一把锁(§三单写者):这里只如实报,不动它。
    std::string instance_state = "not_running";
    std::string instance_detail;
    const bool gateway_lock_present =
        std::filesystem::exists(options_.gateway_paths.lock_file, ec) && !ec;
    if (gateway_lock_present) {
        std::string lock_error;
        const auto holder =
            gateway::ReadGatewayLockFile(options_.gateway_paths.lock_file, &lock_error);
        if (!holder.has_value()) {
            instance_state = "lock_unreadable";
            instance_detail = lock_error;
        } else if (options_.is_alive(holder->pid)) {
            instance_state = "running";
            // 持锁者可能是 gateway run,也可能是本助理的任务面(两把业务
            // 面共用同一把单写者锁)——锁账只记 pid/boot,页面按"锁被活
            // 进程持有"理解。
            instance_detail =
                "pid " + std::to_string(holder->pid) + ",boot " + holder->boot_id;
        } else {
            instance_state = "stale_lock";
            instance_detail = "锁是陈旧的(持有进程已退出)";
        }
    }
    result["gatewayInstance"] =
        nlohmann::json{{"state", instance_state}, {"detail", instance_detail}};
    result["boundary"] =
        "安装/卸载走命令行: lubancode gateway service install / uninstall(页面不代跑安装);"
        "注册状态对账走 lubancode gateway doctor。";
    return result;
}

// ---------------------------------------------------------------------------
// 方法注册
// ---------------------------------------------------------------------------

void RegisterAssistantChannelMethods(app_server::Dispatcher& dispatcher,
                                      const std::shared_ptr<AssistantChannelFace>& face) {
    const auto register_method = [&dispatcher,
                                  &face](const char* method,
                                         nlohmann::json (AssistantChannelFace::*handler)(
                                             const nlohmann::json&, int&, std::string&)) {
        dispatcher.RegisterMethod(
            method, [face, method, handler](const app_server::IncomingRequest& request,
                                            app_server::DispatchContext&)
                       -> std::optional<nlohmann::json> {
                int error_code = 0;
                std::string error_message;
                nlohmann::json result =
                    (*face.*handler)(request.params, error_code, error_message);
                if (error_code != 0) {
                    return app_server::MakeError(request.id, error_code, error_message);
                }
                return app_server::MakeResult(request.id, std::move(result));
            });
    };
    register_method("channel/list", &AssistantChannelFace::HandleChannelList);
    register_method("channel/status", &AssistantChannelFace::HandleChannelStatus);
    register_method("channel/pairing/respond",
                    &AssistantChannelFace::HandleChannelPairingRespond);
    register_method("gateway/service/status", &AssistantChannelFace::HandleServiceStatus);
}

}  // namespace lubancode::app
