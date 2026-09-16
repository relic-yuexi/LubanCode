// 见 hpp 合同注释(QQ 接入单 Q7)。
#include "channel/qq/qq_menu.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <utility>

#include "channel/qq/qq_proto.hpp"  // ParseLooseInt64(平台数值字段两态)

namespace lubancode::channel::qq {

namespace {

// 期望/远端摘要:nlohmann 对象按键排序,dump() 即规范形,sha256 定 digest。
// 同一份配置永远同一摘要;远端与摘要比对不受键序影响。
std::string DigestJson(const nlohmann::json& value) {
    return Sha256Hex(value.dump());
}

// 平台错误体业务码(A03 共用规则):code/err_code 两形状,唯一字段取其值,
// 并存等值取该值;字段非法或两码冲突——不猜,返回 nullopt(菜单账按无有效
// 码走 rejected_<status>,不当成功)。
std::optional<std::int64_t> PlatformCodeOf(const nlohmann::json& body) {
    if (!body.is_object()) {
        return std::nullopt;
    }
    std::optional<std::int64_t> code;
    std::optional<std::int64_t> err_code;
    if (body.contains("code")) {
        code = ParseLooseInt64(body.at("code"));
    }
    if (body.contains("err_code")) {
        err_code = ParseLooseInt64(body.at("err_code"));
    }
    if (code.has_value() && err_code.has_value()) {
        return *code == *err_code ? code : std::nullopt;
    }
    if (code.has_value()) {
        return code;
    }
    return err_code;
}

}  // namespace

// ---------------------------------------------------------------------------
// 载荷纯函数
// ---------------------------------------------------------------------------

nlohmann::json BuildMenuItemsPayload(const ChannelMenuUserConfig& menu) {
    nlohmann::json items = nlohmann::json::array();
    for (const auto& item : menu.items) {
        nlohmann::json entry = nlohmann::json::object();
        entry["type"] = item.type;
        entry["name"] = item.name;
        if (item.type == "send_message") {
            entry["send_message"] = item.send_message;
        } else if (item.type == "link") {
            entry["link"] = item.link;
        } else if (item.type == "switch") {
            entry["switch"] = nlohmann::json{{"switch_id", item.switch_id},
                                             {"default", item.switch_default}};
        } else if (item.type == "menu") {
            nlohmann::json subs = nlohmann::json::array();
            for (const auto& sub : item.sub_menu_items) {
                nlohmann::json sub_entry = nlohmann::json::object();
                sub_entry["type"] = sub.type;
                sub_entry["name"] = sub.name;
                if (sub.type == "send_message") {
                    sub_entry["send_message"] = sub.send_message;
                } else {
                    sub_entry["link"] = sub.link;
                }
                subs.push_back(std::move(sub_entry));
            }
            entry["sub_menu_items"] = std::move(subs);
        }
        items.push_back(std::move(entry));
    }
    return items;
}

nlohmann::json BuildMenuPutBody(const ChannelMenuUserConfig& menu) {
    return nlohmann::json{{"menu", nlohmann::json{{"items", BuildMenuItemsPayload(menu)}}}};
}

std::optional<MenuGetResult> ParseMenuGetBody(const nlohmann::json& body, std::string* error) {
    if (!body.is_object()) {
        if (error != nullptr) *error = "menu get body not an object";
        return std::nullopt;
    }
    MenuGetResult result;
    if (body.contains("version")) {
        const auto version = ParseLooseInt64(body.at("version"));
        if (!version.has_value()) {
            if (error != nullptr) *error = "menu get version not an integer";
            return std::nullopt;
        }
        result.version = *version;
    }
    if (body.contains("menu") && !body.at("menu").is_null()) {
        const nlohmann::json& menu = body.at("menu");
        if (!menu.is_object() || !menu.contains("items") || !menu.at("items").is_array()) {
            if (error != nullptr) *error = "menu get menu.items not an array";
            return std::nullopt;
        }
        result.has_menu = true;
        result.menu = menu;
    }
    return result;
}

std::string MakePanelRemark(const std::string& channel_id, const std::string& account_id,
                            const std::string& user_remark) {
    // 所有权前缀:面板列表里认领"这块是我们发的"的唯一标记(备注不对用户
    // 展示,平台上限 255 字符;配置层给用户备注压了 200,前缀必放得下)。
    return "lubancode:" + channel_id + ":" + account_id + ":" + user_remark;
}

nlohmann::json BuildPanelPayload(const ChannelPanelUserConfig& panel,
                                 const std::string& remark) {
    nlohmann::json items = nlohmann::json::array();
    for (const auto& item : panel.items) {
        nlohmann::json entry = nlohmann::json::object();
        entry["type"] = item.type;
        entry["name"] = item.name;
        if (!item.desc.empty()) {
            entry["desc"] = item.desc;
        }
        if (item.only_admin) {
            entry["only_admin"] = true;
        }
        if (item.type == "link") {
            entry["link"] = item.link;
        }
        items.push_back(std::move(entry));
    }
    return nlohmann::json{{"items", std::move(items)}, {"remark", remark}};
}

nlohmann::json BuildPanelCreateBody(const ChannelPanelUserConfig& panel,
                                    const std::string& remark) {
    // 创建不带 openid 清单:关联对象一律走 target 接口(单一路径,重启
    // 增量可算);target_type 缺省 all,specific 显式传。
    nlohmann::json body = nlohmann::json::object();
    body["scope"] = panel.scope;
    if (!panel.target_type.empty()) {
        body["target_type"] = panel.target_type;
    }
    body["panel"] = BuildPanelPayload(panel, remark);
    return body;
}

nlohmann::json BuildPanelUpdateBody(const ChannelPanelUserConfig& panel,
                                    const std::string& remark) {
    return nlohmann::json{{"panel", BuildPanelPayload(panel, remark)}};
}

std::optional<std::string> ParsePanelCreateBody(const nlohmann::json& body, std::string* error) {
    if (!body.is_object() || !body.contains("panel_id") || !body.at("panel_id").is_string() ||
        body.at("panel_id").get<std::string>().empty()) {
        if (error != nullptr) *error = "panel create body missing panel_id";
        return std::nullopt;
    }
    return body.at("panel_id").get<std::string>();
}

std::optional<std::int64_t> ParsePanelVersionBody(const nlohmann::json& body, std::string* error) {
    if (!body.is_object() || !body.contains("version")) {
        if (error != nullptr) *error = "panel update body missing version";
        return std::nullopt;
    }
    const auto version = ParseLooseInt64(body.at("version"));
    if (!version.has_value()) {
        if (error != nullptr) *error = "panel update version not an integer";
        return std::nullopt;
    }
    return version;
}

std::optional<PanelsListResult> ParsePanelsListBody(const nlohmann::json& body,
                                                    std::string* error) {
    if (!body.is_object() || !body.contains("records") || !body.at("records").is_array()) {
        if (error != nullptr) *error = "panels list body missing records";
        return std::nullopt;
    }
    PanelsListResult result;
    for (const auto& record : body.at("records")) {
        if (!record.is_object() || !record.contains("panel_id") ||
            !record.at("panel_id").is_string()) {
            if (error != nullptr) *error = "panels list record missing panel_id";
            return std::nullopt;
        }
        PanelRecordView view;
        view.panel_id = record.at("panel_id").get<std::string>();
        if (record.contains("scope") && record.at("scope").is_string()) {
            view.scope = record.at("scope").get<std::string>();
        }
        if (record.contains("target_type") && record.at("target_type").is_string()) {
            view.target_type = record.at("target_type").get<std::string>();
        }
        if (record.contains("panel") && record.at("panel").is_object()) {
            view.panel = record.at("panel");
        }
        if (record.contains("version")) {
            if (const auto version = ParseLooseInt64(record.at("version"))) {
                view.version = *version;
            }
        }
        result.records.push_back(std::move(view));
    }
    if (body.contains("next_cursor") && body.at("next_cursor").is_string()) {
        result.next_cursor = body.at("next_cursor").get<std::string>();
    }
    if (body.contains("is_end") && body.at("is_end").is_boolean()) {
        result.is_end = body.at("is_end").get<bool>();
    } else if (!result.next_cursor.empty()) {
        result.is_end = false;
    }
    return result;
}

nlohmann::json BuildPanelTargetBody(const std::string& op,
                                    const std::vector<std::string>& user_openids) {
    return nlohmann::json{{"op", op}, {"user_openids", user_openids}};
}

// ---------------------------------------------------------------------------
// 限速
// ---------------------------------------------------------------------------

bool MenuQpmLimiter::TryAcquire(const Budget& budget, std::int64_t now_ms,
                                std::int64_t* retry_at_ms) {
    while (!stamps_.empty() && now_ms - stamps_.front() >= budget.window_ms) {
        stamps_.pop_front();
    }
    if (static_cast<int>(stamps_.size()) >= budget.quota) {
        if (retry_at_ms != nullptr) {
            *retry_at_ms = stamps_.front() + budget.window_ms;
        }
        return false;
    }
    stamps_.push_back(now_ms);
    return true;
}

// ---------------------------------------------------------------------------
// 发布器
// ---------------------------------------------------------------------------

// 各接口族的官方 QPM(§十三/接口页)。
const MenuQpmLimiter::Budget kBudgetMenuWrite{5, 60'000};
const MenuQpmLimiter::Budget kBudgetPanelWrite{10, 60'000};
const MenuQpmLimiter::Budget kBudgetRead{30, 60'000};
const MenuQpmLimiter::Budget kBudgetTarget{60, 60'000};

QqMenuPanelPublisher::CallResult QqMenuPanelPublisher::Call(
    const char* rate_class, const MenuQpmLimiter::Budget& budget, const std::string& method,
    const std::string& path, const nlohmann::json* request_body, MenuPanelSyncReport* report) {
    const std::int64_t now = options_.now_ms ? options_.now_ms() : 0;
    CallResult result;
    std::int64_t retry_at = 0;
    if (!limiters_[rate_class].TryAcquire(budget, now, &retry_at)) {
        result.error_code = "menu.rate_limited";
        result.error_detail = std::string(rate_class) + " qpm budget exhausted";
        result.retry_at_ms = retry_at;
        return result;
    }
    if (options_.tokens == nullptr) {
        result.error_code = "menu.token_failed";
        result.error_detail = "token manager not wired";
        return result;
    }
    const auto token = options_.tokens->GetValidToken();
    if (!token.has_value()) {
        result.error_code = "menu.token_failed";
        result.error_detail = "get access token: " + token.error().detail;
        result.retry_at_ms = now + 30'000;
        return result;
    }
    QqHttpRequest request;
    request.method = method;
    request.url = options_.api_base + path;
    request.headers.emplace_back("Authorization", "QQBot " + *token);
    if (request_body != nullptr) {
        request.body = request_body->dump();
    }
    const auto response = options_.http(request);
    if (!response.has_value()) {
        result.error_code = "menu.http_failed";
        result.error_detail = response.error();  // 脱敏人话(seam 合同)
        result.retry_at_ms = now + 30'000;
        return result;
    }
    result.status = response->status;
    if (response->status == 401 || response->status == 403) {
        options_.tokens->Invalidate();
        result.error_code = "menu.auth_failed";
        result.error_detail = "platform rejected token (status " +
                              std::to_string(response->status) + ")";
        result.retry_at_ms = now + 30'000;
        return result;
    }
    if (response->status == 429) {
        result.error_code = "menu.rate_limited";
        result.error_detail = "platform 429";
        result.retry_at_ms = now + 60'000;
        return result;
    }
    if (response->status < 200 || response->status >= 300) {
        // 4xx/5xx:body 若是 {"code":..} 错误体,按平台码分型(40030013 配额/
        // 40030009 并发冲突/40030006 面板不存在);message 不进 detail(可能
        // 带请求回显)。
        nlohmann::json parsed =
            nlohmann::json::parse(response->body, nullptr, /*allow_exceptions=*/false);
        const auto platform_code =
            parsed.is_object() ? PlatformCodeOf(parsed) : std::optional<std::int64_t>();
        if (response->status >= 500) {
            result.error_code = "menu.server_error";
            result.error_detail = "status " + std::to_string(response->status);
            result.retry_at_ms = now + 60'000;
        } else if (platform_code.has_value()) {
            const std::int64_t code = *platform_code;
            if (code == 40030013) {
                result.error_code = "menu.quota_exceeded";
                result.error_detail = "platform quota 40030013";
            } else if (code == 40030009) {
                result.error_code = "menu.concurrent_conflict";
                result.error_detail = "platform busy 40030009";
                result.retry_at_ms = now + 10'000;
            } else if (code == 40030006) {
                result.error_code = "menu.panel_missing";
                result.error_detail = "platform panel gone 40030006";
            } else {
                result.error_code = "menu.rejected_" + std::to_string(code);
                result.error_detail = "status " + std::to_string(response->status) +
                                      " code " + std::to_string(code);
            }
        } else {
            result.error_code = "menu.rejected_" + std::to_string(response->status);
            result.error_detail = "status " + std::to_string(response->status);
        }
        return result;
    }
    // 2xx:target 接口无响应体;其余解析 JSON(空体按空对象收)。
    if (response->body.empty()) {
        result.body = nlohmann::json::object();
        result.body_parsed = true;
    } else {
        result.body =
            nlohmann::json::parse(response->body, nullptr, /*allow_exceptions=*/false);
        if (!result.body.is_discarded()) {
            result.body_parsed = true;
        } else {
            result.error_code = "menu.parse_failed";
            result.error_detail = "2xx body not json (status " +
                                  std::to_string(response->status) + ")";
        }
    }
    result.ok = result.body_parsed;
    return result;
}

nlohmann::json QqMenuPanelPublisher::LoadState() {
    std::error_code ec;
    if (!std::filesystem::exists(options_.state_file, ec)) {
        return nlohmann::json::object();
    }
    std::ifstream input(options_.state_file, std::ios::binary);
    if (!input) {
        return nlohmann::json::object();
    }
    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const nlohmann::json parsed =
        nlohmann::json::parse(bytes, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return nlohmann::json::object();  // 坏账当没有:下轮按首发布走,不猜
    }
    return parsed;
}

void QqMenuPanelPublisher::SaveState(const nlohmann::json& state) {
    // 状态是"已发布摘要"的缓存:ProcessCrashDurability 档够(掉电丢一次只
    // 多一轮对账,平台侧事实不受影响)。失败如实回错码,Sync 折成
    // state_write_failed。
    if (options_.state_file.empty()) {
        return;
    }
    const auto written =
        platform::AtomicWriteFile(options_.state_file, state.dump(),
                                  platform::WriteDurability::ProcessCrashDurability);
    if (!written.has_value()) {
        last_state_error_ = written.error().code;
    }
}

static bool StateHasDigest(const nlohmann::json& section) {
    return section.is_object() && section.contains("published_digest") &&
           section.at("published_digest").is_string();
}

MenuPanelSyncReport QqMenuPanelPublisher::Sync(const SyncInput& input) {
    MenuPanelSyncReport report;
    nlohmann::json state = LoadState();
    bool state_dirty = false;
    const std::int64_t now = options_.now_ms ? options_.now_ms() : 0;

    // ---- 1) 全局菜单 -------------------------------------------------------
    if (input.publish_menu) {
        const nlohmann::json desired = BuildMenuPutBody(input.menu).at("menu");
        const std::string desired_digest = DigestJson(desired);
        const auto remote = Call("read", kBudgetRead, "GET", "/v2/menu", nullptr, &report);
        report.attempted = true;
        if (!remote.ok) {
            report.error_code = remote.error_code;
            report.error_detail = remote.error_detail;
            report.retry_at_ms = remote.retry_at_ms;
            return report;
        }
        std::string parse_error;
        const auto menu_state = ParseMenuGetBody(remote.body, &parse_error);
        if (!menu_state.has_value()) {
            report.error_code = "menu.parse_failed";
            report.error_detail = "menu get: " + parse_error;
            report.retry_at_ms = now + 60'000;
            return report;
        }
        const std::string remote_digest =
            menu_state->has_menu ? DigestJson(menu_state->menu) : DigestJson(nlohmann::json());
        if (remote_digest == desired_digest) {
            report.menu_up_to_date = true;
            report.menu_version = menu_state->version;
        } else {
            const nlohmann::json& menu_section =
                state.contains("menu") && state.at("menu").is_object() ? state.at("menu")
                                                                       : nlohmann::json::object();
            const bool published_before = StateHasDigest(menu_section);
            if (published_before &&
                menu_section.at("published_digest").get<std::string>() != remote_digest) {
                // 远端与本地已发布摘要都对不上:开放平台后台被人工改过。
                // 不覆盖,给本地可见提示(§十三"GET/PUT 版本冲突不覆盖")。
                report.menu_conflict = true;
                report.notices.push_back(
                    "QQ 菜单远端配置与本地上次发布的不一致(疑似在开放平台人工修改过),"
                    "本次不覆盖;要发布请先核对差异。");
            } else {
                if (!published_before && menu_state->has_menu) {
                    report.notices.push_back("QQ 菜单首次发布:远端已有菜单,本次按显式启用"
                                             "的配置覆盖(差异已记入本地状态)。");
                }
                const nlohmann::json put_body = BuildMenuPutBody(input.menu);
                const auto put =
                    Call("menu_write", kBudgetMenuWrite, "PUT", "/v2/menu", &put_body, &report);
                if (!put.ok) {
                    report.error_code = put.error_code;
                    report.error_detail = put.error_detail;
                    report.retry_at_ms = put.retry_at_ms;
                    return report;
                }
                std::string version_error;
                const auto version = ParsePanelVersionBody(put.body, &version_error);
                if (!version.has_value()) {
                    report.error_code = "menu.parse_failed";
                    report.error_detail = "menu put: " + version_error;
                    report.retry_at_ms = now + 60'000;
                    return report;
                }
                report.menu_published = true;
                report.menu_version = *version;
                state["menu"] = nlohmann::json{{"published_digest", desired_digest},
                                               {"version", *version}};
                state_dirty = true;
            }
        }
    }

    // ---- 2) 指令面板 -------------------------------------------------------
    if (input.publish_panel) {
        const std::string remark =
            MakePanelRemark(input.channel_id, input.account_id, input.panel.remark);
        const nlohmann::json desired_panel = BuildPanelPayload(input.panel, remark);
        const std::string desired_digest = DigestJson(desired_panel);

        // 认领:面板列表按备注所有权前缀找(不按 panel_id——id 会随远端
        // 重建变;备注是我们自己写的稳定标记)。分页拉全(≤20 块,一页 50
        // 帽按官方 limit 上限)。
        std::optional<PanelRecordView> owned;
        std::string cursor;
        for (int page = 0; page < 5 && !owned.has_value(); ++page) {
            std::string path = "/v2/panels?scope=" + input.panel.scope + "&limit=50";
            if (!cursor.empty()) {
                // cursor 是平台回传的透传串(官方示例为空/简单 token;真平台
                // 是否含需转义字符——未验,记 Q3)。
                path += "&cursor=" + cursor;
            }
            const auto list = Call("read", kBudgetRead, "GET", path, nullptr, &report);
            report.attempted = true;
            if (!list.ok) {
                report.error_code = list.error_code;
                report.error_detail = list.error_detail;
                report.retry_at_ms = list.retry_at_ms;
                return report;
            }
            std::string parse_error;
            const auto parsed = ParsePanelsListBody(list.body, &parse_error);
            if (!parsed.has_value()) {
                report.error_code = "menu.parse_failed";
                report.error_detail = "panels list: " + parse_error;
                report.retry_at_ms = now + 60'000;
                return report;
            }
            for (const auto& record : parsed->records) {
                if (record.panel.is_object() && record.panel.contains("remark") &&
                    record.panel.at("remark").is_string() &&
                    record.panel.at("remark").get<std::string>() == remark) {
                    owned = record;
                    break;
                }
            }
            if (parsed->is_end || parsed->next_cursor.empty()) {
                break;
            }
            cursor = parsed->next_cursor;
        }

        nlohmann::json panel_section =
            state.contains("panel") && state.at("panel").is_object() ? state.at("panel")
                                                                     : nlohmann::json::object();

        if (!owned.has_value()) {
            // 没有 → 创建(唯一会耗 20 块额度的路;配额错误如实报)。
            const nlohmann::json create_body = BuildPanelCreateBody(input.panel, remark);
            const auto create = Call("panel_write", kBudgetPanelWrite, "POST", "/v2/panels",
                                     &create_body, &report);
            if (!create.ok) {
                report.error_code = create.error_code;
                report.error_detail = create.error_detail;
                report.retry_at_ms = create.retry_at_ms;
                return report;
            }
            std::string parse_error;
            const auto panel_id = ParsePanelCreateBody(create.body, &parse_error);
            if (!panel_id.has_value()) {
                report.error_code = "menu.parse_failed";
                report.error_detail = "panel create: " + parse_error;
                report.retry_at_ms = now + 60'000;
                return report;
            }
            report.panel_created = true;
            report.panel_id = *panel_id;
            panel_section = nlohmann::json{{"panel_id", *panel_id},
                                           {"published_digest", desired_digest},
                                           {"version", 0},
                                           {"targets", nlohmann::json::array()}};
            state_dirty = true;
        } else {
            report.panel_id = owned->panel_id;
            const std::string remote_digest = DigestJson(owned->panel);
            if (remote_digest == desired_digest) {
                report.panel_up_to_date = true;
                report.panel_version = owned->version;
                panel_section["panel_id"] = owned->panel_id;
                panel_section["version"] = owned->version;
                panel_section["published_digest"] = desired_digest;
                state_dirty = true;  // panel_id 可能刚被认领(旧 state 指别的)
            } else if (StateHasDigest(panel_section) &&
                       panel_section.at("published_digest").get<std::string>() != remote_digest) {
                report.notices.push_back("QQ 指令面板远端内容与本地上次发布的不一致(疑似"
                                         "人工修改过),本次不覆盖;要发布请先核对差异。");
                if (panel_section.contains("panel_id") &&
                    panel_section.at("panel_id") != owned->panel_id) {
                    panel_section["panel_id"] = owned->panel_id;
                    state_dirty = true;
                }
            } else {
                const nlohmann::json update_body = BuildPanelUpdateBody(input.panel, remark);
                const auto update =
                    Call("panel_write", kBudgetPanelWrite, "PUT", "/v2/panels/" + owned->panel_id,
                         &update_body, &report);
                if (!update.ok) {
                    report.error_code = update.error_code;
                    report.error_detail = update.error_detail;
                    report.retry_at_ms = update.retry_at_ms;
                    return report;
                }
                std::string parse_error;
                const auto version = ParsePanelVersionBody(update.body, &parse_error);
                if (!version.has_value()) {
                    report.error_code = "menu.parse_failed";
                    report.error_detail = "panel update: " + parse_error;
                    report.retry_at_ms = now + 60'000;
                    return report;
                }
                report.panel_updated = true;
                report.panel_version = *version;
                panel_section["panel_id"] = owned->panel_id;
                panel_section["published_digest"] = desired_digest;
                panel_section["version"] = *version;
                state_dirty = true;
            }
        }

        // ---- 3) 关联对象(c2c specific:已配对用户增删) --------------------
        if (input.panel.target_type == "specific" && !report.panel_id.empty()) {
            std::vector<std::string> current;
            if (panel_section.contains("targets") && panel_section.at("targets").is_array()) {
                for (const auto& target : panel_section.at("targets")) {
                    if (target.is_string()) {
                        current.push_back(target.get<std::string>());
                    }
                }
            }
            std::vector<std::string> desired = input.desired_targets;
            std::sort(desired.begin(), desired.end());
            desired.erase(std::unique(desired.begin(), desired.end()), desired.end());
            std::sort(current.begin(), current.end());
            std::vector<std::string> add;
            std::vector<std::string> remove;
            std::set_difference(desired.begin(), desired.end(), current.begin(), current.end(),
                                std::back_inserter(add));
            std::set_difference(current.begin(), current.end(), desired.begin(), desired.end(),
                                std::back_inserter(remove));
            // 单批 ≤20(官方),按批发;任何一批失败 → 报告重试(增量按未变的
            // 本地账重算,重试安全)。
            auto push_batches = [&](const std::vector<std::string>& openids, const char* op,
                                    std::vector<std::string>* done) -> bool {
                for (std::size_t offset = 0; offset < openids.size(); offset += 20) {
                    std::vector<std::string> batch(openids.begin() + offset,
                                                   std::begin(openids) +
                                                       std::min(openids.size(), offset + 20));
                    const nlohmann::json target_body = BuildPanelTargetBody(op, batch);
                    const auto call = Call("target", kBudgetTarget, "PUT",
                                           "/v2/panels/" + report.panel_id + "/target",
                                           &target_body, &report);
                    if (!call.ok) {
                        report.error_code = call.error_code;
                        report.error_detail = call.error_detail;
                        report.retry_at_ms = call.retry_at_ms;
                        return false;
                    }
                    done->insert(done->end(), batch.begin(), batch.end());
                }
                return true;
            };
            if (!push_batches(add, "add", &report.targets_added) ||
                !push_batches(remove, "del", &report.targets_removed)) {
                return report;
            }
            if (!add.empty() || !remove.empty()) {
                nlohmann::json targets = nlohmann::json::array();
                for (const auto& target : desired) {
                    targets.push_back(target);
                }
                panel_section["targets"] = std::move(targets);
                state_dirty = true;
            }
        }
        state["panel"] = panel_section;
    }

    if (state_dirty) {
        SaveState(state);
        // 状态写失败 → 如实报(下轮全量对账,不丢发布事实——平台侧已是新配置)。
        if (!last_state_error_.empty()) {
            report.error_code = "menu.state_write_failed";
            report.error_detail = last_state_error_;
            report.retry_at_ms = now + 60'000;
            last_state_error_.clear();
        }
    }
    return report;
}

}  // namespace lubancode::channel::qq
