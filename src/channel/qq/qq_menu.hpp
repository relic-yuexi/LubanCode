// QQ 自定义菜单与指令面板(QQ 接入单 Q7 §十三):官方 v2_menu/v2_panels
// 家族的载荷构造/解析 + 显式启用后的发布器。
//
// 官方接口事实(autogen 六页,2026-09-16 逐字核读,fixture 钉死在册):
//   GET  /v2/menu                       30 QPM → {version, menu?}(未设置过
//                                            菜单时 menu 字段为空)
//   PUT  /v2/menu                        5 QPM → {version};body {menu:{items}};
//                                            整覆盖(传入即覆盖完整菜单)
//   GET  /v2/panels?scope=&cursor=&limit 30 QPM → {records,next_cursor,is_end}
//   POST /v2/panels                     10 QPM → {panel_id};一个 bot 最多
//                                            20 块面板
//   PUT  /v2/panels/{id}                10 QPM → {version};只覆盖元素与备注,
//                                            不动关联对象
//   PUT  /v2/panels/{id}/target         60 QPM → 无响应体;op=add/del,单批
//                                            openid ≤20
//   菜单 items ≤10、子菜单 ≤5(不嵌套);面板元素 ≤20。名称按平台字符规则
//   (汉字 2 字符)。错误码族 40030006-40030021(40030013 超量、40030009
//   并发冲突、40030021 all 面板不吃 specific 目标)。
//
// 发布纪律(§十三):
//   - 显式启用才发布(menu.publish / panel.enabled);安装与普通启动不
//     自动覆盖用户在开放平台配好的菜单/面板。
//   - 发布前先读远端:远端与本地已发布摘要一致才 PUT(只有我们改过);
//     远端被人工改过 → 冲突,不覆盖,给本地可见提示。
//   - 面板按备注里的所有权前缀认领,找到就复用更新,不反复创建耗尽
//     20 块额度。
//   - 各接口独立限速(QPM 滑窗);限速/失败 → 报告 + 退避重试,不刷屏。
//   - 状态摘要落 <account_dir>/menu-panel.json(原子写),重启不重发布。
//
// 零 IO 的是载荷纯函数;发布器持 http seam + token manager + 状态文件,
// 由装配层(wiring)在 Gateway tick 里调,不在宿主锁内碰网络。
#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "channel/channel_config.hpp"
#include "channel/digest.hpp"
#include "channel/qq/qq_auth.hpp"
#include "channel/qq/qq_http.hpp"
#include "platform/atomic_write.hpp"

namespace lubancode::channel::qq {

// ---------------------------------------------------------------------------
// 载荷构造(配置 → 官方 payload;items 数组与整份 menu/panel 对象两种粒度,
// 后者做发布比较的期望值)
// ---------------------------------------------------------------------------

// 菜单 items 数组(官方 MenuItem 形状;只放配置里有的键)。
nlohmann::json BuildMenuItemsPayload(const ChannelMenuUserConfig& menu);
// PUT /v2/menu 的请求体 {"menu": {"items": [...]}}。
nlohmann::json BuildMenuPutBody(const ChannelMenuUserConfig& menu);

// GET /v2/menu 响应:{version, menu?}。menu 缺失/null = 平台还没设置过
// 菜单(has_menu=false)。
struct MenuGetResult {
    std::int64_t version = -1;
    bool has_menu = false;
    nlohmann::json menu = nlohmann::json::object();
};
std::optional<MenuGetResult> ParseMenuGetBody(const nlohmann::json& body, std::string* error);

// 面板备注 = 所有权前缀 + 用户备注(前缀认领用,不对用户展示;≤255)。
std::string MakePanelRemark(const std::string& channel_id, const std::string& account_id,
                            const std::string& user_remark);
// 面板配置对象(官方 Panel 形状:{items, remark})。
nlohmann::json BuildPanelPayload(const ChannelPanelUserConfig& panel, const std::string& remark);
// POST /v2/panels 请求体 {scope, target_type, panel}(创建不带 openid 清单,
// 关联对象一律走 target 接口,单一路径)。
nlohmann::json BuildPanelCreateBody(const ChannelPanelUserConfig& panel,
                                    const std::string& remark);
// PUT /v2/panels/{id} 请求体 {panel}。
nlohmann::json BuildPanelUpdateBody(const ChannelPanelUserConfig& panel,
                                    const std::string& remark);

// POST /v2/panels 响应 {panel_id}。
std::optional<std::string> ParsePanelCreateBody(const nlohmann::json& body, std::string* error);
// PUT /v2/panels/{id} 响应 {version}。
std::optional<std::int64_t> ParsePanelVersionBody(const nlohmann::json& body, std::string* error);

// GET /v2/panels 响应。version 数值字段走宽松解析(平台可能回字符串)。
struct PanelRecordView {
    std::string panel_id;
    std::string scope;
    std::string target_type;
    nlohmann::json panel = nlohmann::json::object();
    std::int64_t version = -1;
};
struct PanelsListResult {
    std::vector<PanelRecordView> records;
    std::string next_cursor;
    bool is_end = true;
};
std::optional<PanelsListResult> ParsePanelsListBody(const nlohmann::json& body,
                                                    std::string* error);

// PUT /v2/panels/{id}/target 请求体 {op, user_openids}(c2c 场景)。
nlohmann::json BuildPanelTargetBody(const std::string& op,
                                    const std::vector<std::string>& user_openids);

// ---------------------------------------------------------------------------
// 发布器
// ---------------------------------------------------------------------------

// QPM 滑窗限速(60 秒窗口内至多 quota 次;超额报 retry_at)。窗口走注入
// 的 now_ms,测试用假钟。
class MenuQpmLimiter {
public:
    struct Budget {
        int quota = 1;
        std::int64_t window_ms = 60'000;
    };
    // 想再调一次:窗口内配额够 → true(并记账);不够 → false 且
    // retry_at_ms 出窗时刻。
    bool TryAcquire(const Budget& budget, std::int64_t now_ms, std::int64_t* retry_at_ms);
    std::size_t recent_count() const { return stamps_.size(); }

private:
    std::deque<std::int64_t> stamps_;
};

// 发布结果(全部脱敏:不带 token、不带完整平台响应)。
struct MenuPanelSyncReport {
    bool attempted = false;        // 真碰过平台(有网络调用)
    bool menu_published = false;   // 本次 PUT 过菜单
    bool menu_up_to_date = false;  // 远端已是期望菜单(零写)
    bool menu_conflict = false;    // 远端被人工改过,未覆盖
    bool panel_created = false;
    bool panel_updated = false;
    bool panel_up_to_date = false;
    std::string panel_id;          // 认领/新建的面板(空 = 没碰面板)
    std::int64_t menu_version = -1;
    std::int64_t panel_version = -1;
    std::vector<std::string> targets_added;
    std::vector<std::string> targets_removed;
    std::vector<std::string> notices;  // 本地可见提示(差异/冲突/配额/替换过什么)
    // 失败分型(稳定码,空 = 成功):menu.http_failed/menu.token_failed/
    // menu.rate_limited/menu.auth_failed/menu.server_error/menu.quota_exceeded/
    // menu.concurrent_conflict/menu.rejected_<code>/menu.panel_missing/
    // menu.state_write_failed。
    std::string error_code;
    std::string error_detail;
    std::int64_t retry_at_ms = 0;  // 失败/限速后的下一次可试时刻(0 = 无需重试)
};

class QqMenuPanelPublisher {
public:
    struct Options {
        QqHttpFunc http;
        QqTokenManager* tokens = nullptr;  // 装配层与适配器共用(单飞刷新)
        std::string api_base = "https://api.sgroup.qq.com";
        std::filesystem::path state_file;  // <account_dir>/menu-panel.json
        std::function<std::int64_t()> now_ms;
    };

    explicit QqMenuPanelPublisher(Options options) : options_(std::move(options)) {}

    struct SyncInput {
        std::string channel_id;
        std::string account_id;
        bool publish_menu = false;
        ChannelMenuUserConfig menu;         // 期望菜单(items)
        bool publish_panel = false;         // panel.enabled
        ChannelPanelUserConfig panel;       // 期望面板
        // target_type=specific 时的期望关联(已配对 sender;按本地账增删)。
        std::vector<std::string> desired_targets;
    };

    // 跑一轮发布(菜单 → 面板 → 目标)。失败不抛异常:分型进报告。
    MenuPanelSyncReport Sync(const SyncInput& input);

private:
    // 一笔平台调用(限速 + token + 错误分型)。非 2xx → error_code 填。
    struct CallResult {
        bool ok = false;
        int status = 0;
        nlohmann::json body = nlohmann::json::object();
        bool body_parsed = false;
        std::string error_code;
        std::string error_detail;
        std::int64_t retry_at_ms = 0;
    };
    CallResult Call(const char* rate_class, const MenuQpmLimiter::Budget& budget,
                    const std::string& method, const std::string& path,
                    const nlohmann::json* request_body, MenuPanelSyncReport* report);

    // 状态文件读写(原子写;schema 见 .cpp)。
    nlohmann::json LoadState();
    void SaveState(const nlohmann::json& state);

    Options options_;
    std::string last_state_error_;  // SaveState 最近一次失败码(回 Sync 报告用)
    // 限速账:类名 → 滑窗。类:menu_write(5)/panel_write(10)/read(30)/
    // target(60)。进程内,重启即清(限速是平台配额的礼貌,不是正确性账)。
    std::map<std::string, MenuQpmLimiter> limiters_;
};

}  // namespace lubancode::channel::qq
