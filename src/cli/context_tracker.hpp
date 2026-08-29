// ContextTracker:会话级"上下文占用"记账,给 /context 和自动 compact 用。
//
// 跟 main.cpp 里 UsageStats(按一次 RunTurn() 内所有请求的 input/output
// token 求和,统计"这一问一答总共花了多少 token",每次 RunTurn 都清零重
// 算)不是一回事——ContextTracker 只认"最近一次请求"的用量,新一次请求
// 到达就整个覆盖掉上一次的数字,不跨请求累加。这么定是因为:每次请求都是
// 把当前完整历史重新发一遍给模型,这个用量本来就已经是"这份历史占了多大"
// 的真实度量(真实用量记账,不是拿字符数瞎估),累加多次请求反而是重复
// 计数、数字会越滚越大、跟"当前历史实际占用"这件事对不上。
//
// 占用公式是 TotalInputTokens(input+cache_read+cache_creation) + output
// ——api::Usage 的统一口径下三家 wire 语义一致(input_tokens 一律是"非缓存
// 输入"),这一只公式对所有家都对。

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "api/types.hpp"

namespace lubancode::cli {

// 占用超过窗口这个百分比时,判定"该自动压缩了"。
constexpr int kAutoCompactThresholdPercent = 80;

class ContextTracker {
public:
    explicit ContextTracker(std::size_t window_tokens);

    // ---- 每请求缓存诊断账(问题 9)---------------------------------------
    //
    // 一笔请求的本地前缀视角,agent loop 的前缀记账(agent/prefix.hpp 的
    // 指纹)在发请求前算好、随 UsageReport/事件流带到这里。全部字段只含
    // 短 hash、长度与判定——不落 prompt 正文、API key 或完整 URL(单子
    // 验收明文);缺省值 = "该路径没有诊断信息"(单测/单发),显示层另写
    // "诊断未随行",不拿默认值猜。
    struct CacheDiagnostics {
        bool present = false;  // 事件流真带了诊断字段(交互路径必带)
        int cache_epoch = 1;
        std::string epoch_break_reason;   // 本请求断了 epoch 的点名(空 = 没断)
        bool prefix_append_only = true;   // 本地视角:自上一请求旧消息未变只追加
        bool epoch_first_request = false; // 本 epoch 首请求,没有可比的前一份
        std::string system_hash;          // 16 hex,显示层截 8
        std::string tools_hash;
        std::string prefix_hash;          // 稳定消息前缀合成指纹(空 = 无稳定前缀)
        std::size_t stable_prefix_messages = 0; // 稳定前缀条数
        std::size_t total_messages = 0;         // 本次请求消息总数
        std::int64_t wire_common_prefix_bytes = -1;  // 诊断模式:-1 = 未开/不可得
    };

    // miss 分型(问题 9):命中率掉下来时,断在哪一层一眼可辨——
    //   Hit          provider 报了 cached_tokens > 0,这一笔不是 miss;
    //   FirstRequest epoch 首请求,本地就没有可比的前一份,miss 是天然的;
    //   EpochBreak   本地前缀变了(model/system/tools/旧消息),锅在本地,
    //                break_reason 点名是哪根梁;
    //   UpstreamMiss 本地前缀稳定(追加律成立)而 provider 报 cached=0
    //                ——锅不在本地,明写"上游未命中";
    //   Unreported   provider 没回 usage,缺测另记,不冒充 0%。
    // 诊断没随行(diag.present=false)时不分型,显示层写"诊断未随行"。
    enum class CacheMissKind { Hit, FirstRequest, EpochBreak, UpstreamMiss, Unreported, Unknown };

    // 分型判定(纯逻辑,单测钉):优先级 Unreported > FirstRequest >
    // EpochBreak > UpstreamMiss > Hit——缺测最不该被冒充,本地断因先于
    // 上游结论(本地断了,上游报什么都不用猜)。
    static CacheMissKind ClassifyMiss(bool reported, std::int64_t cache_read, const CacheDiagnostics& diag);

    // 用最近一次请求的 usage 覆盖当前占用(input_tokens + cache_read_tokens +
    // cache_creation_tokens + output_tokens),不是累加——理由见文件头注释。
    void Update(const api::Usage& usage);

    // 回合内 on_usage 的统一入口:usage 带回有效实测(四项 token 不全为
    // 零)就按 Update 覆盖占用、清掉旧值标记;四项全零(provider 没在流末
    // 给 usage——现有 Usage 的全零默认值分不出"真实为零"与"字段缺失",
    // 而真实请求四项不可能全为零,按"没给"处理)时不清零、不覆盖,只把
    // 现有数字标成旧值。状态栏/状态行据 usage_stale() 带 ~ 提醒,别让人把
    // 上一次的实测当成这一次的新数;ESC/HTTP 错误路径压根不会走到
    // on_usage,自然也不会把旧数伪装成本次新值。
    //
    // turn_id / step_index 是这次请求的身份证(问题 5):turn_id 指所属
    // 外层用户轮次(runtime 事件自带),step_index 是该轮内第几次模型请求
    //(agent loop 的步号)。usage 缺测(全零)也照记一笔 unreported 的
    // 请求账——"未回报"另记缺测,不冒充 0%,更不许整行蒸发。默认参数只
    // 照顾旧调用点(单测/单发),交互路径必带。
    //
    // diag(问题 9)是同一笔请求的本地前缀视角诊断账(agent loop 的前缀
    // 记账一路经 UsageReport/事件流带过来):缓存 epoch、追加律、稳定前缀
    // 长度与指纹、miss 分型全靠它。缺省(单测/单发)按"没有诊断信息"记,
    // 显示层写"诊断未随行",不猜。
    void ApplyUsage(const api::Usage& usage, const std::string& turn_id = std::string(), int step_index = 0,
                    const CacheDiagnostics& diag = CacheDiagnostics{});

    // 最近一次"请求结束"是否没有带回实测 usage(旧值标记)。一次实测都没
    // 发生过(刚启动,current_tokens 还是 0)时为 false——那时也没有数字
    // 可标旧。/context 与常驻状态行读同一只 tracker,两处口径一致。
    bool usage_stale() const { return usage_stale_; }

    std::size_t current_tokens() const { return current_tokens_; }
    std::size_t window_tokens() const { return window_tokens_; }

    // 最近一次请求的缓存命中量(usage.cache_read_tokens),跟 current_tokens
    // 一样是覆盖式,不累加;/context 分类明细在"对话历史"行尾括注用。
    // 厂商没给(或还没发过请求)就是 0。
    std::int64_t last_cache_read_tokens() const { return last_cache_read_tokens_; }

    // 最近一次请求的完整输入(TotalInputTokens),命中率分母用——只取输入,
    // 不把 output 混进去。0 = 还没实测过。
    std::int64_t last_total_input_tokens() const { return last_input_tokens_; }

    // 最近一次请求的缓存命中率(百分比,四舍五入)。分母只取输入;没实测
    // (总输入为 0)时返回 -1,调用方写"服务端未回报",不许拿 0 冒充真未命中。
    int last_cache_hit_percent() const {
        if (last_input_tokens_ <= 0) {
            return -1;
        }
        const double ratio = static_cast<double>(last_cache_read_tokens_) /
                             static_cast<double>(last_input_tokens_) * 100.0;
        return static_cast<int>(ratio + 0.5);
    }

    // ---- 会话级缓存诊断结论(前缀缓存诊断单) ----
    // server_prefix_caching:/doctor cache 从服务端 metrics 读到的
    // enable_prefix_caching 结论。nullopt = 还没读过(或读不到),显示层按
    // "未验证"措辞;false 时统计行/状态栏写"服务端未启用",不再拿含糊的
    // "0%" 糊人。两个累计字段是"本场命中率"的分子分母(每笔 usage 摊开
    // 累加,跨轮不清零)——与 last_* 的"最近一次"语义分开。
    void set_server_prefix_caching(std::optional<bool> enabled) { server_prefix_caching_ = enabled; }
    std::optional<bool> server_prefix_caching() const { return server_prefix_caching_; }

    std::int64_t session_cache_read_total() const { return session_cache_read_total_; }
    std::int64_t session_input_total() const { return session_input_total_; }

    // 本场(会话启动至今)缓存命中率(百分比);一次实测都没有返回 -1。
    int session_cache_hit_percent() const {
        if (session_input_total_ <= 0) {
            return -1;
        }
        const double ratio = static_cast<double>(session_cache_read_total_) /
                             static_cast<double>(session_input_total_) * 100.0;
        return static_cast<int>(ratio + 0.5);
    }

    // 逐请求缓存命中历史:每次"一次带回 usage 的 provider 请求"记一条
    // "该次请求完整输入 / 该次请求命中",环形缓冲,保留最近
    // kCacheHistorySize 次。注意口径:一行是一次**模型请求**,不是一轮用户
    // 问答——一条用户输入触发工具循环时会连发多次请求,每笔带 turn_id 与
    // step_index,可追到所属外层轮次(真实实测问题单问题 5:"轮"字混用
    // 误导,这里把名字钉死在"请求"上)。分子分母都是"那一次请求"的,
    // 不加总、不掺重复——跟 session_* 累计口径互补:累计口径回答"整个
    // session 我发了多少输入、多少走了缓存读",逐请求口径回答"每一次
    // 请求各自命中多少"。命中率掉的时候,一眼看出是哪一次、什么操作
    // 导致的(epoch 断因在回合统计/流水账里另有细账)。
    static constexpr std::size_t kCacheHistorySize = 12;

    // 逐请求记录:问题 5 的 turn_id/step_index/unreported 之上,问题 9 再
    // 抄入每请求诊断账(见类头的 CacheDiagnostics)与 miss 分型。
    struct CacheRequestRecord {
        std::int64_t input_tokens = 0;      // 该次请求完整输入(TotalInputTokens)
        std::int64_t cache_read_tokens = 0; // 该次请求缓存命中
        // 该次请求属于哪个外层用户轮次(runtime 发的 turn_id,如 "turn-3";
        // 空 = 事件没带,显示层按"轮次不明"分组)。
        std::string turn_id;
        // 该轮内第几次模型请求(0-based,agent loop 的 step_index,一步
        // 一次请求;显示层 +1 报"请求 1/2/3…")。
        int step_index = 0;
        // 该次请求 provider 没回 usage(缺测):显示层写"未回报",不冒充 0%。
        bool unreported = false;
        // ---- 问题 9 诊断账(由 ApplyUsage 从 CacheDiagnostics 抄入) ----
        bool diagnostics_present = false;   // false = 该路径没带诊断(显示层另写)
        int cache_epoch = 1;
        std::string epoch_break_reason;     // 本请求断 epoch 的点名(空 = 没断)
        bool prefix_append_only = true;     // 本地视角追加律
        bool epoch_first_request = false;
        std::string system_hash;            // 显示层截 8 位
        std::string tools_hash;
        std::string prefix_hash;
        std::size_t stable_prefix_messages = 0;
        std::size_t total_messages = 0;
        std::int64_t wire_common_prefix_bytes = -1;
        CacheMissKind miss_kind = CacheMissKind::Unknown;
        // 该次请求命中率(百分比,四舍五入);input 为 0(含未回报)时返回 -1。
        int hit_percent() const {
            if (input_tokens <= 0) {
                return -1;
            }
            const double ratio = static_cast<double>(cache_read_tokens) /
                                 static_cast<double>(input_tokens) * 100.0;
            return static_cast<int>(ratio + 0.5);
        }
    };

    // 外层用户轮次的登记账(/context 分组显示用):turn_id 配一句人话标签
    //(用户输入首行截断)。会话层在发轮前 BeginUserTurn 登记;没登记到的
    // turn_id(单发/续跑等路径)由记录路径自动补号,标签留空。
    struct UserTurnLabel {
        std::string turn_id;
        std::string label;  // 用户输入首行(截断);空 = 未登记
        int ordinal = 0;    // 本会话第几个用户轮次(1 起,登记序)
    };

    // 登记一个外层用户轮次(发轮前调):turn_id 已在账上就只补标签,不重号。
    void BeginUserTurn(const std::string& turn_id, const std::string& label);

    // 已登记的用户轮次(按登记序)。显示层拿 turn_id 查标签与序号。
    const std::vector<UserTurnLabel>& turn_labels() const { return turn_labels_; }

    // 按 turn_id 查登记账;没登记过返回 nullptr。
    const UserTurnLabel* FindTurnLabel(const std::string& turn_id) const;

    // 本会话(含已被环形缓冲挤掉的)总共记过多少次模型请求——显示层
    // 拿它写"全会话共 N 次",不把环形上限 12 冒充总数。
    std::int64_t total_model_requests() const { return total_model_requests_; }

    // 最近 kCacheHistorySize 次模型请求,按时间顺序(最旧在前)。一次
    // 实测都没有时为空。调用方直接读,显示层负责分组与截断。
    const std::vector<CacheRequestRecord>& cache_request_history() const { return cache_history_; }

    // /context <档位> 用:会话级临时改窗口大小,不改配置文件。
    void set_window_tokens(std::size_t window_tokens) { window_tokens_ = window_tokens; }

    // 占用百分比,四舍五入到整数;window_tokens_ 是 0 时按 0 处理(不除零、
    // 不炸)。
    int UsagePercent() const;

    // 占用是否超过 kAutoCompactThresholdPercent,该自动压缩了。
    bool ShouldAutoCompact() const;

private:
    // 陌生 turn_id 自动补号(记录路径兜底);空 id 不登记,显示层按
    // "轮次不明"分组。
    void RegisterTurnIfMissing(const std::string& turn_id);

    std::size_t current_tokens_ = 0;
    std::size_t window_tokens_;
    std::int64_t last_cache_read_tokens_ = 0;
    std::int64_t last_input_tokens_ = 0;
    bool usage_stale_ = false;
    std::optional<bool> server_prefix_caching_;
    std::int64_t session_cache_read_total_ = 0;
    std::int64_t session_input_total_ = 0;
    std::vector<CacheRequestRecord> cache_history_;
    // 请求总账(不被环形缓冲挤掉)与用户轮次登记账(有界,只留最近
    // kMaxTurnLabels 个——12 笔请求至多跨 12 个轮次,32 留足余量)。
    std::int64_t total_model_requests_ = 0;
    std::vector<UserTurnLabel> turn_labels_;
    int next_turn_ordinal_ = 0;
    static constexpr std::size_t kMaxTurnLabels = 32;
};

}  // namespace lubancode::cli
