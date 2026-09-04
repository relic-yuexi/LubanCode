// token 估算校准器(真实 usage 反推 byte 比率单,2026-09 用户提案)。
//
// 提案要义:每枚请求发出去,本地字节账与估算账都定得下来,provider 实报
// 的 usage 也回得来——几对样本就能反推这枚模型的真实 token/byte 比率。
// BBPE 分词下多 byte 可能合一个 token,精确计数要背分词器依赖;但同一会
// 话的内容形状稳定,统计出的平均水准已够预算与触发判定用。不同模型各异,
// 各算各的。
//
// 一对样本 = (本地发送 byte 数, 本地默认尺估算 token 数, provider 实报
// 完整输入 token 数)。校准系数取窗口内 `实报/估算` 的中位数,估算口
// (EstimateUtf8Tokens 一族)乘上它,压缩触发双闸、preflight 三项账与
// /context 百分比便自动吃到。
//
// 对账口径是完整输入(TotalInputTokens = 非缓存 + 缓存读 + 缓存写):
// provider 的分词器数的就是整份 prompt,缓存只改计费不改计数。单子护栏
// 点过的坑——拿非缓存 input_tokens 对整份字节,cache 命中时必然虚低,
// 样本全废;完整输入口径天然免疫。异常样本剔除与漂移重置再兜一层:个别
// provider 的缓存计数若失真,落在中位数带外的样本进不了窗口。
//
// 分桶:按 (provider, model) 各记各的,不跨模型共用比率(单子明言)。
// 进程内缓存即可,不落盘;跨会话落盘另议(要走配置/存储正门)。

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace lubancode::agent {

// 一对校准样本:一次真实模型请求的本地账 vs provider 实报。
struct TokenCalibrationSample {
    // 本地发送的文本字节账:system + messages + 工具定义(name/描述/schema)
    // 的 UTF-8 字节数之和(与 loop 拼请求处同一副口径)。
    std::size_t request_bytes = 0;
    // 同一份请求按默认尺(EstimateUtf8Tokens 一族,不乘系数)估的 token 数。
    // 系数的锚是默认尺——量的时候乘过系数,量出来的就是自我追尾。
    std::size_t estimated_tokens = 0;
    // provider 实报完整输入(api::TotalInputTokens 口径)。
    std::int64_t reported_input_tokens = 0;
};

// /context 校准行的材料(全部现算,不落盘)。
struct TokenCalibrationStatus {
    bool calibrated = false;         // 窗口内有效样本 >= kMinSamples(两句前不校准)
    std::size_t sample_count = 0;    // 窗口内有效样本数(含被挤出的历史累计另算)
    double tokens_per_byte = 0.0;    // 窗口内 `实报输入/本地字节` 的中位,诊断显示用
    double coefficient = 1.0;        // 窗口内 `实报输入/默认尺估算` 的中位,估算口乘它
    // 默认尺偏差:round((1/系数 - 1) * 100)。+25 = 默认尺比真实高 25%
    //(估算要往下修);-20 = 默认尺比真实低 20%。0 = 无偏差。
    int estimate_deviation_percent = 0;
};

class TokenCalibrator {
public:
    // 滚动窗口:最近 kWindow 对样本取中位(单子建议 8)。
    static constexpr std::size_t kWindow = 8;
    // 首两对之前用现估系数(1.0)兜底。
    static constexpr std::size_t kMinSamples = 2;
    // 小请求不记:协议脚手架(角色标签/JSON 包络/逐消息骨架)在两三千
    // token 以下的请求里占比过高,比率被脚手架带歪,不是内容形状。
    static constexpr std::size_t kMinRequestEstimateTokens = 2048;
    // 硬带:单对样本的 实报/估算 落在 [0.2, 5.0] 之外直接弃——真分词器对
    // 默认尺(ASCII 4 字符/token、非 ASCII 1.5 token/字)不可能偏到这外头,
    // 出界只可能是 usage 报得残缺或口径错位。
    static constexpr double kHardRatioLow = 0.2;
    static constexpr double kHardRatioHigh = 5.0;
    // 异常带:与窗口中位偏离超过 ±40% 的样本剔除(个别 provider 缓存计数
    // 失真、半截 usage 一类),不让单枚脏样本拖动中位。
    static constexpr double kAnomalyBand = 0.40;
    // 漂移重置:连续两枚被异常带拦下、且偏离超中位 2 倍——不是脏样本,是
    // 分词口径真换了(换端点/换 tokenizer)。清窗重起,别拿旧中位把新口径
    // 永远拦在门外。
    static constexpr double kDriftFactor = 2.0;

    enum class RecordVerdict {
        Accepted,        // 入窗
        RejectedInvalid, // 账不成立(估算/字节/实报为零或过小)
        RejectedHardBand,// 硬带外,弃
        RejectedAnomaly, // 异常带外,弃(连续两枚超漂移线则触发重置)
        WindowReset,     // 漂移重置:窗口清空,本样本独居新窗
    };

    // 记一对样本(含全部护栏判定)。线程安全:主会话与子代理线程都可能记。
    RecordVerdict Record(const std::string& provider, const std::string& model,
                         const TokenCalibrationSample& sample);

    // 当前校准系数:窗口内 `实报/估算` 的中位;样本不足 kMinSamples 时 1.0
    //(默认尺兜底)。桶不存在同样 1.0。
    double Coefficient(const std::string& provider, const std::string& model) const;

    // /context 校准行的材料。
    TokenCalibrationStatus StatusOf(const std::string& provider, const std::string& model) const;

    // 清全部桶(测试隔离用;生产没有重置口——漂移重置由护栏自管)。
    void ResetForTest();

private:
    struct Bucket {
        std::vector<TokenCalibrationSample> window;  // 最旧在前,容量 kWindow
        int anomaly_streak = 0;                      // 连续被异常带拦下的枚数
    };
    static std::string BucketKey(const std::string& provider, const std::string& model);

    mutable std::mutex mutex_;
    std::map<std::string, Bucket> buckets_;
};

// 进程级默认实例:主会话/子代理/app-server 共用一只,(provider,model)
// 分桶天然共享。会话不接线(TurnWiring::token_calibrator 空)就谁也不碰
// 它,行为与从前一字不差。
TokenCalibrator& DefaultTokenCalibrator();

}  // namespace lubancode::agent
