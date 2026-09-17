// 飞书 tenant_access_token 管理(飞书/企微设计单 F1,§5.8):app_id/secret
// 换 token、单飞刷新、到期重取。照 QqTokenManager 的合同逐条对齐:
//
//   - 单飞:刷新期间后来的 GetValidToken 阻塞等同一次结果——一只账号最多
//     一只在途 token 请求;
//   - 提前刷新余量 refresh_margin_secs(生产 300s = 5 分钟,设计单钉死);
//   - Invalidate()(发送侧收到 401)强制下次现取;
//   - 泄露禁令:错误 detail 不带 secret/token 值。
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "channel/feishu/feishu_http.hpp"

namespace lubancode::channel::feishu {

class FeishuTokenManager {
public:
    enum class ErrorKind {
        NetworkError,
        InvalidCredentials,  // 非空 HTTP 200 但 code!=0 / 缺字段 / 非 2xx 拒绝
        ServerError,         // 5xx
        RateLimited,         // 429
    };
    struct Error {
        ErrorKind kind = ErrorKind::NetworkError;
        std::string detail;  // 脱敏
    };

    struct Options {
        std::string app_id;
        std::string app_secret;  // 进程内持有,不落日志
        FeishuHttpFunc http;
        std::function<std::int64_t()> now_ms;
        std::string token_url =
            "https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal";
        // 提前刷新余量(秒):expire - margin 视为到期(设计单 §5.8:5 分钟)。
        std::int64_t refresh_margin_secs = 300;
    };

    explicit FeishuTokenManager(Options options) : options_(std::move(options)) {}

    // 取可用 token;到期/失效/首取都现刷。错误分型见 ErrorKind。
    std::expected<std::string, Error> GetValidToken();

    // 标记当前 token 失效(发送侧 401 后调用);下次 GetValidToken 现刷。
    void Invalidate();

private:
    std::expected<std::string, Error> RefreshLocked();

    Options options_;
    std::mutex mutex_;  // 单飞:整个刷新在锁内,后来者等这一次结果
    std::optional<std::string> token_;
    std::int64_t expires_at_ms_ = 0;
};

}  // namespace lubancode::channel::feishu
