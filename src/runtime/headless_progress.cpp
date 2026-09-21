#include "runtime/headless_progress.hpp"

#include <iomanip>
#include <sstream>
#include <utility>

#include "api/types.hpp"
#include "privacy/secret_scan.hpp"

namespace lubancode::runtime {
namespace {
std::string Percent(std::int64_t numerator, std::int64_t denominator) {
    if (denominator <= 0) return "不适用";
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << 100.0 * numerator / denominator << "%";
    return out.str();
}
std::string Count(std::int64_t value, bool reported) {
    return reported ? std::to_string(value) : "未报告";
}
}  // namespace

HeadlessProgressReporter::HeadlessProgressReporter(Emit emit, std::string label, std::string model)
    : emit_(std::move(emit)), label_(std::move(label)), model_(std::move(model)) {}

std::string HeadlessProgressReporter::Preview(const std::string& text, std::size_t cap) {
    std::string clean = privacy::RedactSecrets(text);
    for (char& c : clean) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7f) c = ' ';
    }
    if (clean.size() > cap) {
        std::size_t end = cap;
        while (end > 0 && (static_cast<unsigned char>(clean[end]) & 0xc0) == 0x80) --end;
        clean.resize(end);
        clean += "…";
    }
    return clean;
}

void HeadlessProgressReporter::Note(const std::string& text) const {
    if (emit_) {
        try { emit_("[" + Preview(label_, 180) + "] " + text); }
        catch (...) { /* 观察口不能改变执行结果。 */ }
    }
}

void HeadlessProgressReporter::Observe(const ServerEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& p = event.payload;
    if (p.value("subordinate", false)) return;
    if (event.kind == ServerEventKind::TurnStarted) {
        Note("开始处理 model=" + Preview(model_) + " turn=" + Preview(event.turn_id));
    } else if (event.kind == ServerEventKind::ModelStepStarted) {
        ++requests_;
        Note("请求模型 step=" + std::to_string(p.value("step_index", 0) + 1));
    } else if (event.kind == ServerEventKind::ItemStarted && event.item_kind == ItemKind::Tool) {
        const auto name = p.value("tool_name", std::string("unknown"));
        tools_[event.item_id] = name;
        std::string summary;
        if (p.contains("input") && p["input"].is_object()) {
            // 只挑定位操作所需字段，不把写入正文、图片字节等整包打印。
            for (const auto* key : {"command", "cmd", "path", "file_path", "query", "pattern"}) {
                if (p["input"].contains(key) && p["input"][key].is_string())
                    summary += " " + std::string(key) + "=" + Preview(p["input"][key].get<std::string>());
            }
        }
        Note("请求工具 " + Preview(name) + summary + "（尚不代表已执行）");
    } else if (event.kind == ServerEventKind::ItemCompleted && event.item_kind == ItemKind::Tool) {
        const auto it = tools_.find(event.item_id);
        const auto name = it == tools_.end() ? event.item_id : it->second;
        Note("工具结果 " + Preview(name) + " status=" +
             (event.outcome ? ToString(*event.outcome) : "unknown") +
             " " + Preview(p.value("result", std::string())));
        if (it != tools_.end()) tools_.erase(it);
    } else if (event.kind == ServerEventKind::UsageUpdated) {
        const bool reported = p.value("reported_by_provider", false);
        const bool read = p.value("cache_read_reported_by_provider", false);
        const bool write = p.value("cache_creation_reported_by_provider", false);
        api::Usage usage;
        usage.input_tokens = p.value("input_tokens", std::int64_t{0});
        usage.output_tokens = p.value("output_tokens", std::int64_t{0});
        usage.cache_read_tokens = p.value("cache_read_tokens", std::int64_t{0});
        usage.cache_creation_tokens = p.value("cache_creation_tokens", std::int64_t{0});
        const auto total = api::TotalInputTokens(usage);
        const auto anomaly = p.value("usage_anomaly", std::string());
        std::string line = "用量 step=" + std::to_string(p.value("step_index", 0) + 1) +
            " 输入=" + Count(total, reported) + " 输出=" + Count(usage.output_tokens, reported) +
            " 缓存读=" + Count(usage.cache_read_tokens, read) +
            " 缓存写=" + Count(usage.cache_creation_tokens, write);
        if (reported && read && anomaly.empty()) line += " 命中率=" + Percent(usage.cache_read_tokens, total);
        line += " API=" + std::to_string(p.value("api_duration_ms", std::int64_t{0})) + "ms";
        Note(line);
        if (reported) {
            ++usage_reports_;
            input_ += total;
            output_ += usage.output_tokens;
            if (read) { ++reads_reported_; cache_read_ += usage.cache_read_tokens; }
            if (write) { ++writes_reported_; cache_write_ += usage.cache_creation_tokens; }
            Note("本次输入占运行窗口=" + std::to_string(total) + "/" +
                 (window_ ? std::to_string(window_) : "未知") +
                 (window_ ? "（" + Percent(total, static_cast<std::int64_t>(window_)) + "）" : "") +
                 "；这是本次请求输入，不是整轮累计消耗");
        }
        if (!anomaly.empty()) Note("用量口径异常：" + Preview(anomaly));
        Note("本地前缀 epoch=" + std::to_string(p.value("cache_epoch", 0)) +
             " 首请求=" + (p.value("epoch_first_request", false) ? "是" : "否") +
             " 仅追加=" + (p.value("prefix_append_only", false) ? "是" : "否") +
             " 变化原因=" + Preview(p.value("epoch_break_reason", std::string("无"))) +
             "（不等于服务端缓存命中）");
    } else if (event.kind == ServerEventKind::TurnCompleted) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_).count();
        const auto partial = usage_reports_ < requests_ ? "（部分请求未报告）" : "";
        Note("模型回合结束 status=" + (event.outcome ? ToString(*event.outcome) : "unknown") +
             " 耗时=" + std::to_string(elapsed) + "ms 模型步数=" + std::to_string(requests_) +
             " 累计输入=" + Count(input_, usage_reports_ > 0) +
             " 累计输出=" + Count(output_, usage_reports_ > 0) + partial +
             " 缓存读合计=" + Count(cache_read_, reads_reported_ > 0) +
             (reads_reported_ < requests_ && reads_reported_ > 0 ? "（仅已报告）" : "") +
             " 缓存写合计=" + Count(cache_write_, writes_reported_ > 0) +
             (writes_reported_ < requests_ && writes_reported_ > 0 ? "（仅已报告）" : "") +
             "；回复是否送达另看投递回执");
    }
}

void HeadlessProgressReporter::Context(const agent::ContextPressure& p) {
    std::lock_guard<std::mutex> lock(mutex_);
    window_ = p.window_tokens;
    using Phase = agent::ContextPressure::Phase;
    if (p.phase == Phase::PreRequest) {
        Note("context 估算：工作视图=" + std::to_string(p.working_view_tokens) +
             " 预计请求含输出预留=" + std::to_string(p.projected_tokens) +
             " 运行窗口=" + std::to_string(p.window_tokens) +
             " 占用=" + Percent(static_cast<std::int64_t>(p.projected_tokens),
                                static_cast<std::int64_t>(p.window_tokens)) +
             (p.projected_overflow ? "；达到上下文压力阈值，未据此宣称已压缩" : ""));
    } else if (p.phase == Phase::AfterHardTrim && p.hard_truncated_results) {
        Note("context：超长工具结果发生有损截断（不是语义压缩）");
    } else if (p.phase == Phase::SendOverflow) {
        Note("context：服务端拒绝输入超窗");
    } else if (p.phase == Phase::PreflightExceeded) {
        Note(std::string("context 预检：") + (p.reserve_clamped ? "输出预留已收窄" : "超出窗口"));
    }
}
}  // namespace lubancode::runtime
