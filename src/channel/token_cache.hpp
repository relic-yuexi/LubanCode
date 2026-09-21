// 到期令牌缓存的单一状态机(架构审查 SV-06):QQ 与飞书令牌缓存原先
// 复制整套失效与刷新规矩,本件收敛为共用件。共同职责只含 token 值、
// 到期时钟、互斥单飞、刷新代次、invalidate;平台请求体、响应字段解析
// 与错误分型仍各归平台 refresh 适配器注入(ErrorT 即平台错误类型,缓存
// 不感知)。
//
// 单飞与同代合同(SV-06 第二批把头注承诺补齐成真合同):
//   - 一只账号最多一只在途 token 请求:首个调用当 leader 让出锁去刷
//     (HTTP 不占锁),同场后来的调用在条件变量上等同一次结果——成功
//     共享新 token,失败共享同款错误,不再各刷一遍;
//   - 同代判定按代次:调用进门记 arrival 代,失败落在"飞行起始代"上
//     记账;只有与失败刷新同代(飞行期间在场)的等待者领这枚错误。
//     失败代一过即失配——后来的独立调用照常重试,失败缓存时长以代次
//     为界,绝不永久缓存失败;
//   - Invalidate()(平台 401 后发送侧调用)只清当下缓存并推代;刷新
//     在途时不阻不炸,在途产出的新 token 晚于本次失效落账——旧 token
//     的失效请求清不掉新代 token;
//   - 提前刷新余量 refresh_margin_secs:lifetime - margin 视为到期
//     (0 帽,防负寿命);到期判定是严格小于,恰好到期即现刷。
//
// 模板头件零 IO:不吃平台载荷,不登记 CMake 源清单。
#pragma once

#include <condition_variable>
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

    // 标记当前 token 失效;下次 GetValid 现刷。
    void Invalidate();

private:
    Options options_;
    std::mutex mutex_;
    std::condition_variable flight_cv_;  // 等在飞刷新上的同代调用
    // 代次:Invalidate 与每次刷新完成(成败都算)各推一代。调用进门记
    // arrival 代——失败只在"飞行起始代"上可领,代次一推进即失配。
    std::uint64_t generation_ = 0;
    bool refresh_in_flight_ = false;
    std::optional<std::uint64_t> failure_flight_gen_;  // 失败刷新的起始代
    std::optional<ErrorT> shared_failure_;             // 同代等待者共享的错误
    std::optional<std::string> token_;
    std::int64_t expires_at_ms_ = 0;
};

template <typename ErrorT>
void ExpiringTokenCache<ErrorT>::Invalidate() {
    const std::lock_guard<std::mutex> lock(mutex_);
    token_.reset();
    expires_at_ms_ = 0;
    failure_flight_gen_.reset();
    shared_failure_.reset();
    ++generation_;
    // 在途刷新不夺不阻:它产出的新 token 晚于本次失效落账,照常进缓存
    //(旧 401 清不掉新代 token);若它失败,份额照它自己的起始代记账。
}

template <typename ErrorT>
std::expected<std::string, ErrorT> ExpiringTokenCache<ErrorT>::GetValid() {
    std::unique_lock<std::mutex> lock(mutex_);
    const std::uint64_t arrival_gen = generation_;
    for (;;) {
        // 命中缓存须三事齐:token 在场、未到期、无在途刷新(有在途就等
        // 它的结果——先查钟再查在途,进门探测次序与旧实现一致)。
        if (token_.has_value() && options_.now_ms() < expires_at_ms_ &&
            !refresh_in_flight_) {
            return *token_;
        }
        if (refresh_in_flight_) {
            flight_cv_.wait(lock, [this] { return !refresh_in_flight_; });
            // 醒来重走:成功则命中新 token;失败则按代次领同款错误;
            // 已有新飞行再起则继续等(仍等同一次结果)。
            continue;
        }
        if (failure_flight_gen_.has_value() && *failure_flight_gen_ == arrival_gen) {
            // 同代失败共享:我在这次失败刷新飞行期间在场,领同款错误,
            // 不再发 HTTP。独立后来者读到的是推进后的新代,不落此枝。
            return std::unexpected(*shared_failure_);
        }
        // 当 leader:推旗让出锁去刷——HTTP 不占锁,同代调用才进得来
        // 记账,Invalidate 也即时生效。
        const std::uint64_t flight_base_gen = generation_;
        refresh_in_flight_ = true;
        lock.unlock();
        std::expected<Refreshed, ErrorT> refreshed;
        try {
            refreshed = options_.refresh();
        } catch (...) {
            // 适配器抛异常(与旧实现一致向上穿),但先收摊:清在途旗、
            // 推代、唤醒等待者,不留永久的在途标记卡死后来者。
            lock.lock();
            refresh_in_flight_ = false;
            ++generation_;
            flight_cv_.notify_all();
            throw;
        }
        lock.lock();
        refresh_in_flight_ = false;
        std::expected<std::string, ErrorT> result;
        if (refreshed.has_value()) {
            token_ = refreshed->token;
            const std::int64_t lifetime_ms =
                (refreshed->lifetime_secs - options_.refresh_margin_secs) * 1000;
            expires_at_ms_ = options_.now_ms() + (lifetime_ms > 0 ? lifetime_ms : 0);
            failure_flight_gen_.reset();
            shared_failure_.reset();
            result = *token_;
        } else {
            // 失败记账在飞行起始代上:飞行期间进场的等待者共享本错误;
            // 无人在场则同样落账(等待者零个自然无人来领,下一代调用
            // 因代次推进不落此枝,照常独立重试)。
            shared_failure_ = refreshed.error();
            failure_flight_gen_ = flight_base_gen;
            result = std::unexpected(refreshed.error());
        }
        ++generation_;
        flight_cv_.notify_all();
        return result;
    }
}

}  // namespace lubancode::channel
