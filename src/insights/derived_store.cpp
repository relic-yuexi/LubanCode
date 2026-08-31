#include "insights/derived_store.hpp"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "insights/report_model.hpp"  // kInsightsAnalyzerVersion

namespace lubancode::insights {

std::filesystem::path SessionSummaryPath(const std::filesystem::path& session_dir) {
    return session_dir / kDerivedSummaryDir / "session-summary.json";
}

DerivedWriteResult WriteSessionSummaryAtomic(const std::filesystem::path& session_dir,
                                             const SessionInsightSummary& summary) {
    DerivedWriteResult result;
    const std::filesystem::path target = SessionSummaryPath(session_dir);
    result.path = target;
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        result.error_code = "derived.mkdir_failed";
        result.message = "派生目录建不成:" + target.parent_path().string() + ": " + ec.message();
        return result;
    }
    std::filesystem::path tmp = target;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            result.error_code = "derived.tmp_open_failed";
            result.message = "临时文件打不开:" + tmp.string();
            return result;
        }
        out << summary.ToJson().dump(2) << "\n";
        out.flush();
        if (!out) {
            result.error_code = "derived.tmp_write_failed";
            result.message = "临时文件写失败:" + tmp.string();
            std::filesystem::remove(tmp, ec);
            return result;
        }
    }
    // Windows 的 rename 不覆盖已有文件,先挪走旧的再换(全程不留半截)。
    std::filesystem::path old;
    if (std::filesystem::exists(target, ec)) {
        old = target;
        old += ".old";
        std::filesystem::remove(old, ec);
        std::error_code rename_ec;
        std::filesystem::rename(target, old, rename_ec);
        if (rename_ec) {
            old.clear();
        }
    }
    std::error_code rename_ec;
    std::filesystem::rename(tmp, target, rename_ec);
    if (rename_ec) {
        // 换不上:把旧摘要挪回去,清 tmp,如实报错。
        if (!old.empty()) {
            std::error_code back_ec;
            std::filesystem::rename(old, target, back_ec);
        }
        std::filesystem::remove(tmp, ec);
        result.error_code = "derived.rename_failed";
        result.message = "原子替换失败:" + rename_ec.message();
        return result;
    }
    if (!old.empty()) {
        std::filesystem::remove(old, ec);
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
