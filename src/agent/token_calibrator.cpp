// token 估算校准器的实现(形状与护栏语义见 token_calibrator.hpp 文件头)。

#include "agent/token_calibrator.hpp"

#include <algorithm>
#include <cmath>

namespace lubancode::agent {

namespace {

// 升序序列的中位数;空序列返回 0(调用方保证非空)。
double MedianOf(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    if (n % 2 == 1) {
        return values[n / 2];
    }
    return (values[n / 2 - 1] + values[n / 2]) / 2.0;
}

// 一对样本的 实报/估算 比率;估算为零时返回 0(视为账不成立)。
double SampleRatio(const TokenCalibrationSample& sample) {
    if (sample.estimated_tokens == 0) {
        return 0.0;
    }
    return static_cast<double>(sample.reported_input_tokens) /
           static_cast<double>(sample.estimated_tokens);
}

}  // namespace

std::string TokenCalibrator::BucketKey(const std::string& provider, const std::string& model) {
    // '\x1f' 做分隔:provider/model 名里都不许出现的控制字符,拼出来的键
    // 不会撞桶。
    return provider + '\x1f' + model;
}

TokenCalibrator::RecordVerdict TokenCalibrator::Record(const std::string& provider, const std::string& model,
                                                       const TokenCalibrationSample& sample) {
    // 账不成立的一律弃:实报<=0(没报或明报全零)、字节为零、估算过小
    //(脚手架占比过高)。这些不进漂移账——它们不是"比率不对",是压根
    // 没量出比率。
    if (sample.reported_input_tokens <= 0 || sample.request_bytes == 0 ||
        sample.estimated_tokens < kMinRequestEstimateTokens) {
        return RecordVerdict::RejectedInvalid;
    }
    const double ratio = SampleRatio(sample);
    if (!(ratio >= kHardRatioLow && ratio <= kHardRatioHigh)) {
        return RecordVerdict::RejectedHardBand;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    Bucket& bucket = buckets_[BucketKey(provider, model)];
    if (bucket.window.size() >= kMinSamples) {
        std::vector<double> ratios;
        ratios.reserve(bucket.window.size());
        for (const auto& s : bucket.window) {
            ratios.push_back(SampleRatio(s));
        }
        const double median = MedianOf(std::move(ratios));
        const double upper = median * (1.0 + kAnomalyBand);
        const double lower = median * (1.0 - kAnomalyBand);
        if (ratio > upper || ratio < lower) {
            // 异常带外。先看是不是漂移:连续两枚、且本枚偏离超中位
            // kDriftFactor 倍——分词口径真换了,清窗重起,本样本独居新窗
            //(重置后仍须再攒一对才算校准,别拿一枚孤样本当定盘星)。
            const bool beyond_drift = ratio > median * kDriftFactor || ratio < median / kDriftFactor;
            if (beyond_drift && bucket.anomaly_streak >= 1) {
                bucket.window.clear();
                bucket.window.push_back(sample);
                bucket.anomaly_streak = 0;
                return RecordVerdict::WindowReset;
            }
            ++bucket.anomaly_streak;
            return RecordVerdict::RejectedAnomaly;
        }
    }
    bucket.anomaly_streak = 0;
    bucket.window.push_back(sample);
    if (bucket.window.size() > kWindow) {
        bucket.window.erase(bucket.window.begin());
    }
    return RecordVerdict::Accepted;
}

double TokenCalibrator::Coefficient(const std::string& provider, const std::string& model) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = buckets_.find(BucketKey(provider, model));
    if (it == buckets_.end() || it->second.window.size() < kMinSamples) {
        return 1.0;
    }
    std::vector<double> ratios;
    ratios.reserve(it->second.window.size());
    for (const auto& s : it->second.window) {
        ratios.push_back(SampleRatio(s));
    }
    return MedianOf(std::move(ratios));
}

TokenCalibrationStatus TokenCalibrator::StatusOf(const std::string& provider, const std::string& model) const {
    TokenCalibrationStatus status;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = buckets_.find(BucketKey(provider, model));
    if (it == buckets_.end()) {
        return status;  // 桶不存在:零样本,系数兜底
    }
    const Bucket& bucket = it->second;
    // 窗内实数如实报——没到校准线也说清攒了几对,不吞样本数。
    status.sample_count = bucket.window.size();
    if (bucket.window.size() < kMinSamples) {
        return status;  // calibrated=false,coefficient=1.0,tokens_per_byte 零值
    }
    std::vector<double> ratios;
    std::vector<double> per_byte;
    ratios.reserve(bucket.window.size());
    per_byte.reserve(bucket.window.size());
    for (const auto& s : bucket.window) {
        ratios.push_back(SampleRatio(s));
        per_byte.push_back(static_cast<double>(s.reported_input_tokens) /
                           static_cast<double>(s.request_bytes));
    }
    status.calibrated = true;
    status.coefficient = MedianOf(std::move(ratios));
    status.tokens_per_byte = MedianOf(std::move(per_byte));
    if (status.coefficient > 0.0) {
        status.estimate_deviation_percent =
            static_cast<int>(std::lround((1.0 / status.coefficient - 1.0) * 100.0));
    }
    return status;
}

void TokenCalibrator::ResetForTest() {
    std::lock_guard<std::mutex> lock(mutex_);
    buckets_.clear();
}

TokenCalibrator& DefaultTokenCalibrator() {
    static TokenCalibrator instance;
    return instance;
}

}  // namespace lubancode::agent
