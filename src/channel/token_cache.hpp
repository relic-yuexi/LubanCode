// 到期令牌缓存的单一状态机(架构审查 SV-06):QQ 与飞书令牌缓存原先
// 复制整套失效与刷新规矩,本件收敛为共用件。共同职责只含 token 值、
// 到期时钟、互斥单飞、invalidate;平台请求体、响应字段解析与错误分型
// 仍各归平台 refresh 适配器注入(ErrorT 即平台错误类型,缓存不感知)。
//
// 合同(与原 QqTokenManager/FeishuTokenManager 逐条对齐,SV-06 第一批
// 只做行为等价归并):
//   - 单飞:刷新在锁内进行,后来的 GetValid 阻塞等同一次结果——一只
//     账号最多一只在途 token 请求,不放大平台压力;
//   - 提前刷新余量 refresh_margin_secs:lifetime - margin 视为到期
//     (0 帽,防负寿命);
//   - Invalidate() 清缓存,下次现刷;
//   - 失败不缓存 token:刷新返回错误时原 token 不动。同代等待者共享
//     失败结果的合同见 SV-06 第二批提交。
//
// 模板头件零 IO:不吃平台载荷,不登记 CMake 源清单。
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace lubancode::channel {

template <typename ErrorT>
class ExpiringTokenCache {
public:
    // 平台 refresh 适配器的产物:token 值 + 平台报的有效期秒(未扣
    // margin,margin 由本缓存统一扣——两平台的余量公式相同,归共用件)。
    struct Refreshed {
        std::string token;
        std::int64_t lifetime_secs = 0;
    };

    struct Options {
        std::function<std::int64_t()> now_ms;
        // 提前刷新余量(秒):lifetime_secs - margin 视为到期。
        std::int64_t refresh_margin_secs = 300;
        // 刷新适配器:发平台请求、解析载荷、错误分型;错误类型归平台。
        std::function<std::expected<Refreshed, ErrorT>()> refresh;
    };

    explicit ExpiringTokenCache(Options options) : options_(std::move(options)) {}

    // 取可用 token;到期/失效/首取都现刷。
    std::expected<std::string, ErrorT> GetValid();

    // 标记当前 token 失效(平台 401 后由发送侧调用);下次 GetValid 现刷。
    void Invalidate();

private:
    Options options_;
    std::mutex mutex_;  // 单飞:整个刷新在锁内,后来者等这一次结果
    std::optional<std::string> token_;
    std::int64_t expires_at_ms_ = 0;
};

template <typename ErrorT>
void ExpiringTokenCache<ErrorT>::Invalidate() {
    const std::lock_guard<std::mutex> lock(mutex_);
    token_.reset();
    expires_at_ms_ = 0;
}

template <typename ErrorT>
std::expected<std::string, ErrorT> ExpiringTokenCache<ErrorT>::GetValid() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (token_.has_value() && options_.now_ms() < expires_at_ms_) {
        return *token_;
    }
    // 刷新在锁内:并发调用等同一次结果(失败不缓存,与原实现一致)。
    const auto refreshed = options_.refresh();
    if (!refreshed.has_value()) {
        return std::unexpected(refreshed.error());
    }
    token_ = refreshed->token;
    const std::int64_t lifetime_ms =
        (refreshed->lifetime_secs - options_.refresh_margin_secs) * 1000;
    expires_at_ms_ = options_.now_ms() + (lifetime_ms > 0 ? lifetime_ms : 0);
    return *token_;
}

}  // namespace lubancode::channel
