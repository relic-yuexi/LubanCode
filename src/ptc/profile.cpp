// profile.hpp 的实现:画像序列化、指纹、auto 门槛、熔断器、存档。

#include "ptc/profile.hpp"

#include <algorithm>
#include <fstream>
#include <map>

#include "config/config.hpp"  // HomeLubancodeDir
#include "platform/paths.hpp"

namespace lubancode::ptc {

std::string ToString(PtcStatus status) {
    switch (status) {
        case PtcStatus::Unsupported: return "unsupported";
        case PtcStatus::Unknown: return "unknown";
        case PtcStatus::Experimental: return "experimental";
        case PtcStatus::Verified: return "verified";
    }
    return "unknown";
}

std::optional<PtcStatus> ParseStatus(const std::string& text) {
    if (text == "unsupported") return PtcStatus::Unsupported;
    if (text == "unknown") return PtcStatus::Unknown;
    if (text == "experimental") return PtcStatus::Experimental;
    if (text == "verified") return PtcStatus::Verified;
    return std::nullopt;
}

nlohmann::json PtcProfile::ToJson() const {
    nlohmann::json out = nlohmann::json::object();
    out["fingerprint"] = fingerprint;
    out["status"] = ToString(status);
    out["language"] = language;
    out["single_call_accuracy"] = single_call_accuracy;
    out["chain_accuracy"] = chain_accuracy;
    out["fanout_accuracy"] = fanout_accuracy;
    out["runtime_error_rate"] = runtime_error_rate;
    out["max_verified_chain"] = max_verified_chain;
    out["max_verified_fanout"] = max_verified_fanout;
    out["verified_at"] = verified_at;
    out["harness_revision"] = harness_revision;
    return out;
}

std::optional<PtcProfile> PtcProfile::FromJson(const nlohmann::json& json) {
    if (!json.is_object() || !json.contains("fingerprint") || !json["fingerprint"].is_string()) {
        return std::nullopt;
    }
    PtcProfile profile;
    profile.fingerprint = json["fingerprint"].get<std::string>();
    if (json.contains("status") && json["status"].is_string()) {
        const auto status = ParseStatus(json["status"].get<std::string>());
        if (!status.has_value()) {
            return std::nullopt;
        }
        profile.status = *status;
    }
    const auto read_double = [&json](const char* key, double& slot) {
        if (json.contains(key) && json[key].is_number()) {
            slot = json[key].get<double>();
        }
    };
    read_double("single_call_accuracy", profile.single_call_accuracy);
    read_double("chain_accuracy", profile.chain_accuracy);
    read_double("fanout_accuracy", profile.fanout_accuracy);
    read_double("runtime_error_rate", profile.runtime_error_rate);
    const auto read_int = [&json](const char* key, int& slot) {
        if (json.contains(key) && json[key].is_number_integer()) {
            slot = json[key].get<int>();
        }
    };
    read_int("max_verified_chain", profile.max_verified_chain);
    read_int("max_verified_fanout", profile.max_verified_fanout);
    if (json.contains("verified_at") && json["verified_at"].is_string()) {
        profile.verified_at = json["verified_at"].get<std::string>();
    }
    if (json.contains("harness_revision") && json["harness_revision"].is_string()) {
        profile.harness_revision = json["harness_revision"].get<std::string>();
    }
    return profile;
}

std::string BuildPtcFingerprint(const std::string& provider, const std::string& endpoint, const std::string& model,
                                const std::string& wire, const std::string& python_version,
                                const std::string& harness_revision) {
    // 各成分显式分段(\x1f 分隔),不靠拼接歧义。
    const std::string joined = provider + "\x1f" + endpoint + "\x1f" + model + "\x1f" + wire + "\x1f" +
                               python_version + "\x1f" + harness_revision;
    return PtcRunner::StableHashText(joined);
}

std::vector<std::string> PtcHardConditions::FailureTexts() const {
    std::vector<std::string> out;
    if (!sandbox_reliable) {
        out.push_back("平台没有可靠 Python 沙箱(Windows Job Object+受限 token 达标;POSIX rlimit 不算)");
    }
    if (!model_free_code) {
        out.push_back("模型/端不能输出自由代码(正文被锁成结构化格式)");
    }
    if (!context_fits_stubs) {
        out.push_back("上下文装不下当前 stub 集");
    }
    if (!tools_wired) {
        out.push_back("入选工具未全部接通 PTC RPC/权限/hooks/取消链");
    }
    if (!python_version_ok) {
        out.push_back("Python 运行时版本不符(须 >= 3.9)");
    }
    return out;
}

std::string ToString(ToolCallingDecision decision) {
    return decision == ToolCallingDecision::Programmatic ? "ptc" : "json";
}

ToolCallingDecision ResolveToolCalling(const PtcAutoGates& gates) {
    if (!gates.hard_conditions_met) {
        return ToolCallingDecision::Json;
    }
    if (gates.profile_status != PtcStatus::Verified) {
        // 规格原文:厂商目录只写"支持"而本机没跑过探针,仍不得自动启用。
        return ToolCallingDecision::Json;
    }
    const bool deep_enough =
        gates.estimated_chain_depth >= gates.min_chain_depth || gates.estimated_fanout >= gates.min_fanout;
    if (!deep_enough) {
        return ToolCallingDecision::Json;
    }
    return ToolCallingDecision::Programmatic;
}

void PtcCircuitBreaker::Record(const PtcRunResult& run) {
    if (tripped_) {
        return;  // 降档后本场不再升回,后续结果只记账不动闸
    }
    bool fault = false;
    std::string what;
    switch (run.failure) {
        case PtcFailure::Syntax:
            fault = true;
            what = "脚本语法错";
            break;
        case PtcFailure::Rpc:
        case PtcFailure::Protocol:
            fault = true;
            what = "RPC 协议错";
            break;
        case PtcFailure::Runtime:
            // 漏调用/空脚本:零调用且没 emit 才算模型不会写;有调用有失败
            // 是工具世界常态,不算熔断证据。
            if (run.ZeroCallsHappened() && run.emit_value.is_null()) {
                fault = true;
                what = "空脚本(零调用零摘要)";
            }
            break;
        default:
            break;
    }
    if (run.ok) {
        consecutive_faults_ = 0;
        last_fault_.clear();
        return;
    }
    if (!fault) {
        return;  // 撞墙/取消/沙箱终止不动熔断计数
    }
    ++consecutive_faults_;
    last_fault_ = what;
    if (consecutive_faults_ >= threshold_) {
        tripped_ = true;
    }
}

std::string PtcCircuitBreaker::Reason() const {
    if (!tripped_) {
        return {};
    }
    return "连续 " + std::to_string(consecutive_faults_) + " 次 " + last_fault_ + ",本场已降回 JSON 工具调用";
}

std::vector<PtcProfile> PtcProfileStore::Load() const {
    std::vector<PtcProfile> out;
    std::ifstream in(lubancode::platform::Utf8ToWide(path_), std::ios::binary);
    if (!in) {
        return out;  // 没有存档 = 全 unknown,不算错
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto parsed = nlohmann::json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return out;  // 坏文件不当错;下一次 Save 重建
    }
    for (auto it = parsed.begin(); it != parsed.end(); ++it) {
        auto profile = PtcProfile::FromJson(it.value());
        if (profile.has_value()) {
            out.push_back(std::move(*profile));
        }
    }
    return out;
}

std::optional<PtcProfile> PtcProfileStore::Find(const std::string& fingerprint) const {
    for (const auto& profile : Load()) {
        if (profile.fingerprint == fingerprint) {
            return profile;
        }
    }
    return std::nullopt;
}

bool PtcProfileStore::Save(const PtcProfile& profile, std::string* error) {
    // 全量读改写:保留别的指纹条目。
    nlohmann::json root = nlohmann::json::object();
    std::ifstream in(lubancode::platform::Utf8ToWide(path_), std::ios::binary);
    if (in) {
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto parsed = nlohmann::json::parse(text, nullptr, false);
        if (!parsed.is_discarded() && parsed.is_object()) {
            root = parsed;
        }
    }
    root[profile.fingerprint] = profile.ToJson();
    std::ofstream out(lubancode::platform::Utf8ToWide(path_), std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error != nullptr) {
            *error = "打不开画像存档写入: " + path_;
        }
        return false;
    }
    out << root.dump(2);
    return out.good();
}

std::string DefaultProfileStorePath() {
    const auto home = lubancode::config::HomeLubancodeDir();
    if (!home.has_value()) {
        return {};
    }
    return *home + "/ptc_profiles.json";
}

}  // namespace lubancode::ptc
