// QQ Access Token 管理(QQ 机器人接入单 Q1):AppID/Secret 换 token、单飞
// 刷新、到期重取、错误码分型(§十五原生适配器 auth 模块)。
//
// 单飞合同:刷新期间后来的 GetValidToken 阻塞等同一次结果——一只账号最多
// 一只在途 token 请求,不放大平台压力。到期重取按 expires_in - margin 提前;
// Invalidate()(调用方收到 401)强制下次现取。
//
// SV-06 起缓存状态机(token 值/到期时钟/互斥/invalidate)归共用件
// channel::ExpiringTokenCache,本类是薄门面:只留 QQ 请求体构造、
// expires_in 载荷解析与平台错误分型(SV-06 第一批行为等价归并)。
//
// 泄露禁令:错误 detail 不带 secret/token 值(稳定分型 + 人话)。
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <string>

#include "channel/qq/qq_http.hpp"
#include "channel/token_cache.hpp"

namespace lubancode::channel::qq {

class QqTokenManager {
public:
    enum class ErrorKind {
        NetworkError,
        InvalidCredentials,  // 非空 HTTP 200 但缺 access_token / 非 2xx 鉴权拒绝
        ServerError,         // 5xx
        RateLimited,         // 429
    };
    struct Error {
        ErrorKind kind = ErrorKind::NetworkError;
        std::string detail;  // 脱敏
    };

    struct Options {
        std::string app_id;
        std::string client_secret;  // 进程内持有,不落日志
        QqHttpFunc http;
        std::function<std::int64_t()> now_ms;
        std::string token_url = "https://bots.qq.com/app/getAppAccessToken";
        // 提前刷新余量(秒):expires_in - margin 视为到期,避免边界尖刺。
        std::int64_t refresh_margin_secs = 300;
    };

    explicit QqTokenManager(Options options)
        : options_(std::move(options)), cache_(MakeCache()) {}

    // 取可用 token;到期/失效/首取都现刷。错误分型见 ErrorKind。
    std::expected<std::string, Error> GetValidToken();

    // 标记当前 token 失效(平台 401 后由发送侧调用);下次 GetValidToken 现刷。
    void Invalidate();

private:
    // QQ refresh 适配器:发取 token 请求、解析 expires_in 载荷、错误分型。
    std::expected<ExpiringTokenCache<Error>::Refreshed, Error> Refresh();
    ExpiringTokenCache<Error> MakeCache();

    Options options_;
    ExpiringTokenCache<Error> cache_;  // 缓存状态机(SV-06 共用件)
};

}  // namespace lubancode::channel::qq
