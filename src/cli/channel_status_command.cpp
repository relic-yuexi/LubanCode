// channel status 子命令实现:合同见 hpp 注释。
#include "cli/channel_status_command.hpp"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <istream>
#include <optional>

#include "app/channel_connection_reporter.hpp"
#include "channel/channel_config.hpp"
#include "channel/manager.hpp"
#include "channel/pairing.hpp"
#include "channel/work_ledger.hpp"
#include "config/config.hpp"
#include "gateway/profile.hpp"
#include "gateway/reply_outbox.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "platform/wall_clock.hpp"

namespace lubancode::cli {

namespace {

// 时间短说(只读投影用):UTC 秒级展示,不引本地时区依赖;0/坏值如实报。
std::string FormatRecentTurnTime(std::int64_t at_ms) {
    if (at_ms <= 0) {
        return "时间未知";
    }
    const std::time_t seconds = static_cast<std::time_t>(at_ms / 1000);
    std::tm tm_buffer{};
#ifdef _WIN32
    const std::tm* tm_value = gmtime_s(&tm_buffer, &seconds) == 0 ? &tm_buffer : nullptr;
#else
    const std::tm* tm_value = gmtime_r(&seconds, &tm_buffer);
#endif
    if (tm_value == nullptr) {
        return "时间未知";
    }
    char buffer[40] = {0};
    if (std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d UTC",
                      tm_value->tm_year + 1900, tm_value->tm_mon + 1, tm_value->tm_mday,
                      tm_value->tm_hour, tm_value->tm_min, tm_value->tm_sec) <= 0) {
        return "时间未知";
    }
    return buffer;
}

// 快照字段的安全读取(json 缺键一律 contains(),const operator[] 查缺键
// 是 UB——纪律第 6 条)。
std::string SnapshotString(const nlohmann::json& snapshot, const char* key) {
    return snapshot.contains(key) && snapshot.at(key).is_string()
               ? snapshot.at(key).get<std::string>()
               : std::string();
}

bool SnapshotBool(const nlohmann::json& snapshot, const char* key) {
    return snapshot.contains(key) && snapshot.at(key).is_boolean() &&
           snapshot.at(key).get<bool>();
}

std::int64_t SnapshotInt(const nlohmann::json& snapshot, const char* key) {
    return snapshot.contains(key) && snapshot.at(key).is_number_integer()
               ? snapshot.at(key).get<std::int64_t>()
               : 0;
}

unsigned long SnapshotPid(const nlohmann::json& snapshot) {
    const std::int64_t pid = SnapshotInt(snapshot, "pid");
    return pid > 0 ? static_cast<unsigned long>(pid) : 0;
}

}  // namespace

ChannelStatusVerdict JudgeChannelStatus(const nlohmann::json* snapshot,
                                        const std::string& channel_id,
                                        const std::string& account_id,
                                        std::int64_t now_ms,
                                        const std::function<bool(unsigned long)>& is_alive) {
    ChannelStatusVerdict verdict;
    const std::string who = channel_id + "/" + account_id;
    if (snapshot == nullptr || !snapshot->is_object()) {
        verdict.lines.push_back(who + ": 没有连接状态快照(Gateway 未运行,或该账号未装配)");
        verdict.report["verdict"] = "no_snapshot";
        return verdict;
    }
    const nlohmann::json& body = *snapshot;
    const unsigned long pid = SnapshotPid(body);
    const std::int64_t updated_at_ms = SnapshotInt(body, "updated_at_ms");
    const bool connected = SnapshotBool(body, "connected");

    // 裁决序:进程存活 > 快照新鲜 > connected。每一层失败都如实说,
    // 不拿下一层的结果粉饰。
    if (pid == 0 || (is_alive && !is_alive(pid))) {
        verdict.lines.push_back(who + ": 不在线——发布快照的 Gateway 进程已退出"
                                      "(pid " + std::to_string(pid) + ",旧快照不当作在线)");
        verdict.report["verdict"] = "process_dead";
        verdict.report["snapshot"] = body;
        return verdict;
    }
    if (app::IsConnectionSnapshotStale(now_ms, updated_at_ms)) {
        verdict.lines.push_back(
            who + ": 在线状态未知——快照过期(距上次更新 " +
            std::to_string((now_ms - updated_at_ms + 999) / 1000) +
            " 秒,阈值 60 秒;Gateway 在跑但没在刷新,按不在线处理)");
        verdict.report["verdict"] = "stale_snapshot";
        verdict.report["snapshot"] = body;
        return verdict;
    }
    verdict.report["snapshot"] = body;
    if (!connected) {
        const std::string stage = SnapshotString(body, "stage");
        std::string failure_text;
        if (body.contains("last_failure") && body.at("last_failure").is_object()) {
            const nlohmann::json& failure = body.at("last_failure");
            failure_text = ",最近失败: " + SnapshotString(failure, "error_code") + "(" +
                           SnapshotString(failure, "stage") + ")";
            const std::string detail = SnapshotString(failure, "detail");
            if (!detail.empty()) {
                failure_text += " " + detail;
            }
        }
        const std::int64_t retry_at = SnapshotInt(body, "next_retry_at_ms");
        const std::string retry_text =
            retry_at > 0
                ? ";" + std::to_string((retry_at - now_ms + 999) / 1000) + " 秒后重试"
                : std::string();
        verdict.lines.push_back(who + ": 未连接(阶段 " + stage + failure_text + ")" +
                                retry_text);
        verdict.report["verdict"] = "not_connected";
        return verdict;
    }
    verdict.online = true;
    verdict.exit_code = 0;
    verdict.lines.push_back(who + ": 已连接 QQ(boot " + SnapshotString(body, "boot_id") +
                                  ",pid " + std::to_string(pid) + ")");
    verdict.report["verdict"] = "connected";
    return verdict;
}

// 四步状态(§5.1 末条):配置 → 在线 → 配对 → 模型,固定次序分栏。哪步
// 卡住指哪步的下一步,不拿后面的状态粉饰前面的缺口。
ChannelFourStateView BuildChannelFourState(const std::string& channel_id,
                                           const std::string& account_id,
                                           const ChannelFourStateInput& input) {
    ChannelFourStateView view;
    nlohmann::json report = nlohmann::json::object();

    // 1) 配置已存。
    if (input.account_configured) {
        view.lines.push_back("1. 配置已存:是");
        report["config_saved"] = true;
    } else {
        std::string detail = input.config_detail.empty()
                                 ? std::string("账号不在全局配置里(或未启用/缺 AppID/凭据)")
                                 : input.config_detail;
        view.lines.push_back("1. 配置已存:否——" + detail);
        view.lines.push_back("   下一步: lubancode channel setup " + channel_id +
                             " --account " + account_id);
        report["config_saved"] = false;
        report["config_detail"] = detail;
    }

    // 2) QQ 在线(连接快照裁决;不是配置的延续,是独立事实)。
    if (input.online) {
        view.lines.push_back("2. QQ 在线:是" +
                             (input.online_detail.empty() ? std::string()
                                                          : "(" + input.online_detail + ")"));
        report["online"] = true;
    } else {
        std::string detail = input.online_detail.empty()
                                 ? std::string("没有可用的在线快照")
                                 : input.online_detail;
        view.lines.push_back("2. QQ 在线:否——" + detail);
        view.lines.push_back("   下一步: 在工作目录运行 lubancode gateway run(或 lubancode im)");
        report["online"] = false;
        report["online_detail"] = detail;
    }

    // 3) 身份已配对(pairing 账只读投影)。
    if (!input.pairing_parse_ok) {
        view.lines.push_back("3. 身份已配对:未知——配对账读不懂(<状态根>/channels/" +
                             channel_id + "/" + account_id + "/pairing.json)");
        report["paired"] = "unreadable";
    } else if (input.pairing_approved > 0) {
        view.lines.push_back("3. 身份已配对:是(已批准 " +
                             std::to_string(input.pairing_approved) + " 个身份)");
        report["paired"] = true;
        report["pairing_approved"] = input.pairing_approved;
    } else if (input.pairing_pending > 0) {
        view.lines.push_back("3. 身份已配对:否——有待批准的配对 " +
                             std::to_string(input.pairing_pending) + " 笔");
        view.lines.push_back("   下一步: lubancode channel pairing approve " + channel_id + " " +
                             account_id + " <配对码>(码在用户收到的提示里)");
        report["paired"] = false;
        report["pairing_pending"] = input.pairing_pending;
    } else {
        view.lines.push_back("3. 身份已配对:否——还没有人配对(让用户先给机器人发条消息)");
        report["paired"] = false;
    }

    // 4) 模型配置齐全(P1 修订:沿 #85 的 configured 面,只报配置;"能回复"
    //    是执行结局,不由配置冒充——另起一行报最近真实调用)。
    if (input.model_configured) {
        view.lines.push_back("4. 模型配置齐全:是");
        report["model_ready"] = true;
    } else {
        std::string detail =
            input.model_detail.empty() ? std::string("模型配置不完整") : input.model_detail;
        view.lines.push_back("4. 模型配置齐全:否——" + detail);
        view.lines.push_back("   下一步: lubancode assistant 页面里配模型(config/model/set),"
                             "或检查全局配置的 provider/model/api key");
        report["model_ready"] = false;
        report["model_detail"] = detail;
    }
    // 最近真实执行结局(账号 ingress 账 + dead-letter 旁路账;配置齐全
    // 不等于真的能回复——第二三轮静默失败正是栽在这句话上)。
    if (input.recent_turn_present) {
        report["recent_turn_ok"] = input.recent_turn_ok;
        report["recent_turn_sid"] = input.recent_turn_sid;
        report["recent_turn_at_ms"] = input.recent_turn_at_ms;
        if (!input.recent_turn_detail.empty()) {
            report["recent_turn_detail"] = input.recent_turn_detail;
        }
        view.lines.push_back(
            std::string("5. 最近真实调用:") + (input.recent_turn_ok ? "成功" : "失败") + "(来信 sid=" +
            std::to_string(input.recent_turn_sid) + "," +
            FormatRecentTurnTime(input.recent_turn_at_ms) + ")" +
            (input.recent_turn_ok || input.recent_turn_detail.empty()
                 ? std::string()
                 : "——" + input.recent_turn_detail));
    } else {
        view.lines.push_back("5. 最近真实调用:还没有(这只账号没跑过模型轮;配置齐全不代表"
                             "调用会成功)");
        report["recent_turn_ok"] = nullptr;
    }

    view.report = std::move(report);
    return view;
}

// ---- 最近来信链(P1:来信→准入→执行→投递) -----------------------------------

void DeriveRecentTurnOutcome(const channel::ChannelIngressRecentChain& chain,
                             ChannelFourStateInput* input) {
    for (const auto& entry : chain.entries) {
        // 链 sid 降序:第一枚"执行过"的来信(绑过场或死信/已回)即最近结局。
        const bool executed = entry.state == "dead_letter" || entry.state == "replied" ||
                              entry.state == "delivered" || entry.state == "delivery_failed" ||
                              entry.state == "completed_without_reply";
        if (!executed) {
            continue;
        }
        input->recent_turn_present = true;
        input->recent_turn_ok = entry.state != "dead_letter";
        input->recent_turn_sid = entry.sid;
        input->recent_turn_at_ms =
            entry.state == "dead_letter" && entry.dead_letter_at_ms > 0
                ? entry.dead_letter_at_ms
                : entry.received_at_ms;
        if (!input->recent_turn_ok) {
            std::string reason = entry.reason;
            if (reason.size() > 160) {
                reason.resize(160);
                reason += "…";
            }
            input->recent_turn_detail = reason;
        }
        return;
    }
}

namespace {

// 段状态的中文短说(pending/sending 归"在途")。
std::string DeliveryStateText(const std::string& state) {
    if (state == "sent" || state == "delivered") return "已送达";
    if (state == "failed" || state == "flagged") return "投递失败";
    if (state == "delivery_unknown") return "投递结果未知(超时)";
    if (state == "sending") return "发送中";
    return "待发送";
}

}  // namespace

ChannelRecentChainView BuildChannelRecentChain(const ChannelRecentChainInput& input,
                                               std::size_t limit) {
    ChannelRecentChainView view;
    nlohmann::json entries = nlohmann::json::array();
    if (!input.ledger_present) {
        view.lines.push_back("最近来信链:还没有来信账(没人给机器人发过消息)");
        view.report = entries;
        return view;
    }
    view.lines.push_back("最近来信链(最新在前,至多 " + std::to_string(limit) + " 条):");
    std::size_t shown = 0;
    for (const auto& entry : input.ingress) {
        if (shown >= limit) break;
        ++shown;
        nlohmann::json report = nlohmann::json::object();
        report["sid"] = entry.sid;
        report["state"] = entry.state;
        report["received_at_ms"] = entry.received_at_ms;
        if (!entry.reason.empty()) report["reason"] = entry.reason;
        if (!entry.conversation_id.empty()) report["conversation_id"] = entry.conversation_id;
        std::string line = "  sid=" + std::to_string(entry.sid) + " [" + entry.state + "] 来信 " +
                           FormatRecentTurnTime(entry.received_at_ms != 0
                                                     ? entry.received_at_ms
                                                     : entry.dead_letter_at_ms);
        // 准入:旁路终态(rejected/rate_limited/unsupported)即准入面失败。
        if (entry.state == "rejected" || entry.state == "rate_limited" ||
            entry.state == "unsupported") {
            line += " 准入:未过(" + entry.state + ")";
        } else {
            line += " 准入:已过";
        }
        // 执行:work 绑定在 = 真开过场;dead_letter 的 reason 是执行败因。
        const auto bound = input.bound_sessions.find(entry.sid);
        if (entry.state == "dead_letter") {
            line += " 执行:失败";
            if (!entry.reason.empty()) {
                line += "(" + entry.reason + ")";
                report["reason"] = entry.reason;
            }
        } else if (bound != input.bound_sessions.end()) {
            line += " 执行:已绑场(" + bound->second + ")";
        } else {
            line += " 执行:未见绑定";
        }
        // 投递:正文段(ingress:)与失败提示段(turnfail:)分说。
        for (const auto& delivery : input.deliveries) {
            if (delivery.sid != entry.sid) continue;
            line += delivery.is_failure_notice ? " 失败提示:" + DeliveryStateText(delivery.state)
                                               : " 投递:" + DeliveryStateText(delivery.state);
            if (!delivery.delivery_error.empty() &&
                (delivery.state == "failed" || delivery.state == "delivery_unknown")) {
                line += "(" + delivery.delivery_error + ")";
            }
        }
        view.lines.push_back(std::move(line));
        entries.push_back(std::move(report));
    }
    if (shown == 0) {
        view.lines.push_back("  (账在,还没有事件)");
    }
    view.report = std::move(entries);
    return view;
}

// ---- 配置探针(W3 共用面):读文件 + 判据,单一真源 -------------------------

ChannelsConfigProbe LoadChannelsUserConfigFromFile(const std::filesystem::path& config_path) {
    ChannelsConfigProbe probe;
    std::error_code read_ec;
    if (!std::filesystem::is_regular_file(config_path, read_ec) || read_ec) {
        probe.detail = "全局配置文件不存在(还没跑过配置向导)";
        return probe;
    }
    std::ifstream stream(config_path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const auto parsed = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("channels") ||
        !parsed["channels"].is_object()) {
        probe.detail = "配置文件里没有可用的 channels 段";
        return probe;
    }
    std::string channels_error;
    const auto channels =
        channel::ParseChannelsUserConfig(parsed["channels"],
                                         platform::PathToUtf8(config_path), &channels_error);
    if (!channels.has_value()) {
        probe.detail = "channels 段解析失败: " + channels_error;
        return probe;
    }
    probe.ok = true;
    probe.channels = std::move(*channels);
    return probe;
}

ChannelAccountConfigProbe ProbeChannelAccountConfig(
    const std::map<std::string, channel::ChannelUserConfig>& channels,
    const std::string& channel_id, const std::string& account_id) {
    ChannelAccountConfigProbe probe;
    const auto channel_it = channels.find(channel_id);
    if (channel_it == channels.end()) {
        probe.detail = "channels 段里没有 " + channel_id;
        return probe;
    }
    const auto account_it = channel_it->second.accounts.find(account_id);
    if (account_it == channel_it->second.accounts.end()) {
        probe.detail = "账号 " + account_id + " 不在配置里";
        return probe;
    }
    if (!channel_it->second.enabled || !account_it->second.enabled) {
        probe.detail = "渠道或账号未启用";
        return probe;
    }
    if (account_it->second.app_id.empty()) {
        probe.detail = "AppID 未填";
        return probe;
    }
    if (channel::DescribeCredentialSource(account_it->second) ==
        channel::CredentialSource::Missing) {
        probe.detail = "凭据来源未配(secret_file/secret_env)";
        return probe;
    }
    probe.configured = true;
    return probe;
}

int RunChannelStatusCommand(const ChannelStatusCommandArgs& args) {
    // 渠道/账号 id 先过守门:它们直接拼状态根下的路径,带路径段 = 越界。
    if (!channel::IsValidChannelId(args.channel_id)) {
        std::fprintf(stderr, "channel status: 渠道 id 不合法(非空、无路径段、无控制字符): %s\n",
                     args.channel_id.c_str());
        return 1;
    }
    if (!channel::IsValidChannelAccountId(args.account_id)) {
        std::fprintf(stderr, "channel status: 账号 id 不合法(非空、无路径段、无控制字符): %s\n",
                     args.account_id.c_str());
        return 1;
    }
    const std::filesystem::path channels_root = channel::DefaultChannelsStateRoot();
    if (channels_root.empty()) {
        std::fprintf(stderr,
                     "channel status: 渠道状态根不可用(应用根变量坏或找不到主目录)\n");
        return 1;
    }
    const std::filesystem::path file =
        app::ChannelConnectionReporter::SnapshotFilePath(channels_root, args.channel_id,
                                                         args.account_id);
    std::error_code ec;
    std::optional<nlohmann::json> snapshot;
    if (std::filesystem::is_regular_file(file, ec) && !ec) {
        std::ifstream stream(file, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
        const auto parsed = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (!parsed.is_discarded()) {
            snapshot = parsed;
        }
    }
    const ChannelStatusVerdict verdict = JudgeChannelStatus(
        snapshot.has_value() ? &*snapshot : nullptr, args.channel_id, args.account_id,
        platform::WallClockNowMs(),
        [](unsigned long pid) { return platform::IsProcessAlive(pid); });

    // ---- 四步状态探针(§5.1 末条):配置 / 在线 / 配对 / 模型 -----------
    ChannelFourStateInput four_state;
    four_state.online = verdict.online;
    if (!verdict.online) {
        // 未在线摘要:裁决第一行的人话(快照缺失/进程死/未连接的阶段)。
        if (!verdict.lines.empty()) {
            four_state.online_detail = verdict.lines.front();
        }
    } else if (snapshot.has_value()) {
        four_state.online_detail = "boot " + SnapshotString(*snapshot, "boot_id");
    }
    // 配置:全局 config.json 的 channels 段只读解析(与 channel setup 同源)。
    if (const auto config_file = config::GlobalConfigFilePath(); config_file.has_value()) {
        const auto probe =
            LoadChannelsUserConfigFromFile(platform::Utf8ToPath(*config_file));
        if (probe.ok) {
            const auto account_probe =
                ProbeChannelAccountConfig(probe.channels, args.channel_id, args.account_id);
            four_state.account_configured = account_probe.configured;
            four_state.config_detail = account_probe.detail;
        } else {
            four_state.account_configured = false;
            four_state.config_detail = probe.detail;
        }
    }
    // 配对:pairing 账只读投影(零建目录零写盘)。
    const auto pairing_projection = channel::PairingStore::ReadProjection(
        channels_root / args.channel_id / args.account_id);
    four_state.pairing_present = pairing_projection.present;
    four_state.pairing_parse_ok = pairing_projection.parse_ok;
    four_state.pairing_approved = pairing_projection.approved;
    four_state.pairing_pending = pairing_projection.pending;
    // 模型:沿 #85 assistant config/status 的 configured 面,不重造判据。
    const auto model_config = config::LoadFromEnv();
    if (model_config.has_value()) {
        const auto model_ready = config::RequireConfigured(*model_config);
        four_state.model_configured = model_ready.has_value();
        if (!model_ready.has_value()) {
            four_state.model_detail = model_ready.error();
        }
    } else {
        four_state.model_detail = "配置装载失败: " + model_config.error();
    }
    // 最近真实执行结局 + 来信链(P1):账号三本账只读投影(ingress 链 +
    // dead-letter 旁路账 + outbox 段 + work 绑定)。零建目录零写盘。
    const std::filesystem::path account_dir = channels_root / args.channel_id / args.account_id;
    const auto recent_chain = channel::ReadChannelIngressRecentChain(account_dir, 32);
    DeriveRecentTurnOutcome(recent_chain, &four_state);
    const ChannelFourStateView four = BuildChannelFourState(args.channel_id, args.account_id,
                                                            four_state);
    // 链视图的投递段:gateway 各 profile 的 outbox 只读投影按来源前缀筛
    //(ingress: 正文段;turnfail: 失败提示段——两层失败都在这能看见)。
    ChannelRecentChainInput chain_input;
    chain_input.ledger_present = recent_chain.ledger_present;
    chain_input.ingress = recent_chain.entries;
    const std::string ingress_prefix =
        "ingress:" + args.channel_id + ":" + args.account_id + ":";
    const std::string turnfail_prefix =
        "turnfail:" + args.channel_id + ":" + args.account_id + ":";
    std::error_code list_ec;
    const std::filesystem::path profiles_dir = channels_root.parent_path() / "gateway" / "profiles";
    if (channels_root.has_parent_path() &&
        std::filesystem::is_directory(profiles_dir, list_ec) && !list_ec) {
        std::error_code iter_ec;
        for (const auto& profile_entry :
             std::filesystem::directory_iterator(profiles_dir, iter_ec)) {
            if (iter_ec || !profile_entry.is_directory()) {
                continue;
            }
            const std::filesystem::path outbox_log =
                profile_entry.path() / "delivery" / "outbox.jsonl";
            std::error_code file_ec;
            if (!std::filesystem::is_regular_file(outbox_log, file_ec) || file_ec) {
                continue;
            }
            const auto projection = gateway::ReadOutboxProjection(outbox_log);
            for (const auto& item_pair : projection.items) {
                const gateway::ReplyOutboxItem& item = item_pair.second;
                const std::string& ref = item.source_ref;
                const bool is_ingress = ref.rfind(ingress_prefix, 0) == 0;
                const bool is_turnfail = ref.rfind(turnfail_prefix, 0) == 0;
                if (!is_ingress && !is_turnfail) {
                    continue;
                }
                const std::string sid_text =
                    ref.substr((is_ingress ? ingress_prefix : turnfail_prefix).size());
                std::int64_t sid = 0;
                bool digits = !sid_text.empty() && sid_text.size() < 20;
                for (const char c : sid_text) {
                    if (c < '0' || c > '9') {
                        digits = false;
                        break;
                    }
                }
                if (!digits) {
                    continue;
                }
                sid = std::stoll(sid_text);
                ChannelRecentChainInput::Delivery delivery;
                delivery.sid = sid;
                delivery.is_failure_notice = is_turnfail;
                delivery.state = item.state;
                delivery.delivery_error = item.delivery_error;
                delivery.ordinal = item.ordinal;
                chain_input.deliveries.push_back(std::move(delivery));
            }
        }
    }
    // 执行绑定:work ledger 只读(文件在才开,零建目录)。
    const std::filesystem::path work_ledger_file = account_dir / "work.jsonl";
    if (std::filesystem::is_regular_file(work_ledger_file, list_ec) && !list_ec) {
        channel::ChannelWorkLedger work_ledger;
        if (channel::ChannelWorkLedger::Open(&work_ledger, work_ledger_file).ok) {
            for (const auto& entry : chain_input.ingress) {
                const auto bound = work_ledger.FindBound(entry.sid);
                if (bound.has_value()) {
                    chain_input.bound_sessions.emplace(entry.sid, bound->session_id);
                }
            }
        }
    }
    const ChannelRecentChainView chain = BuildChannelRecentChain(chain_input, 8);

    if (args.json) {
        nlohmann::json report = verdict.report;
        report["four_state"] = four.report;
        report["recent_chain"] = chain.report;
        std::printf("%s\n", report.dump().c_str());
    } else {
        std::printf("%s/%s 状态:\n", args.channel_id.c_str(), args.account_id.c_str());
        for (const std::string& line : four.lines) {
            std::printf("%s\n", line.c_str());
        }
        std::printf("连接明细:\n");
        for (const std::string& line : verdict.lines) {
            std::printf("%s\n", line.c_str());
        }
        for (const std::string& line : chain.lines) {
            std::printf("%s\n", line.c_str());
        }
    }
    return verdict.exit_code;
}

}  // namespace lubancode::cli
