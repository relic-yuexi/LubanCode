#include "insights/derived_store.hpp"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "insights/report_model.hpp"  // kInsightsAnalyzerVersion
#include "platform/atomic_write.hpp"  // 统一原子写(审计 P1:替掉 .tmp+.old 搬移协议)

namespace lubancode::insights {

std::filesystem::path SessionSummaryPath(const std::filesystem::path& session_dir) {
    return session_dir / kDerivedSummaryDir / "session-summary.json";
}

DerivedWriteResult WriteSessionSummaryAtomic(const std::filesystem::path& session_dir,
                                             const SessionInsightSummary& summary) {
    DerivedWriteResult result;
    const std::filesystem::path target = SessionSummaryPath(session_dir);
    result.path = target;
    // 统一走 platform::AtomicWriteFile:唯一临时名、平台原子替换、失败不
    // 动正式摘要。derived.* 稳定码保持原值(读侧按码记账)。
    const auto written = platform::AtomicWriteFile(target, summary.ToJson().dump(2) + "\n");
    if (!written.has_value()) {
        static const std::string kTmpOpen("atomic.tmp_open_failed");
        static const std::string kTmpWrite("atomic.tmp_write_failed");
        static const std::string kMkdir("atomic.mkdir_failed");
        const std::string& code = written.error().code;
        if (code == kTmpOpen) {
            result.error_code = "derived.tmp_open_failed";
        } else if (code == kTmpWrite) {
            result.error_code = "derived.tmp_write_failed";
        } else if (code == kMkdir) {
            result.error_code = "derived.mkdir_failed";
        } else {
            result.error_code = "derived.rename_failed";
        }
        result.message = written.error().message;
        return result;
    }
    result.ok = true;
    return result;
}

DerivedReadResult ReadExistingSessionSummary(const std::filesystem::path& session_dir) {
    DerivedReadResult result;
    const std::filesystem::path path = SessionSummaryPath(session_dir);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return result;
    }
    result.exists = true;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.error = "摘要打不开:" + path.filename().string();
        return result;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const auto parsed = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (parsed.is_discarded()) {
        result.error = "摘要不是合法 JSON:" + path.filename().string();
        return result;
    }
    std::string error;
    auto summary = SessionInsightSummary::FromJsonStrict(parsed, &error);
    if (!summary.has_value()) {
        result.error = "摘要不合合同:" + error;
        return result;
    }
    result.summary = std::move(*summary);
    result.parse_ok = true;
    return result;
}

bool IsSummaryStale(const DerivedReadResult& existing,
                    const std::map<std::string, std::string>& current_hashes) {
    if (!existing.exists || !existing.parse_ok) {
        return true;
    }
    if (existing.summary.source.stream_terminal_hashes != current_hashes) {
        return true;
    }
    // analyzer 版本:summary 里存的与现行 A0 常量比(kInsightsAnalyzerVersion
    // 是默认值;显式比一遍防读回来的字段被改过)。
    return existing.summary.analyzer_version != kInsightsAnalyzerVersion;
}

}  // namespace lubancode::insights
