// PTC 能力画像(规格"PTC 支持不是布尔值"节):四档状态、五条硬条件、
// 画像指纹、熔断器、auto 门槛。全部纯逻辑 + 一份 JSON 存档。
//
// 四档:
//   unsupported  硬条件不齐,或探针稳定失败;
//   unknown      尚未测过(默认;JSON 走起);
//   experimental 基本能跑,只许用户显式强开(programmatic 档);
//   verified     完整基准过线,auto 才可选。
//
// 画像绑定 provider + endpoint + 精确 model id + wire + Python 版本 +
// harness(ptc prompt/stub)版本。任一项变了,指纹变,旧画像查不到,
// 天然降回 unknown——不拿同名模型在另一端点的成绩作保。

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ptc/runner.hpp"

namespace lubancode::ptc {

// harness 版本:bootstrap/stub 生成器/协议改动时递增。指纹成分之一。
inline constexpr const char* kPtcHarnessRevision = "ptc-v2";

enum class PtcStatus { Unsupported, Unknown, Experimental, Verified };

std::string ToString(PtcStatus status);
std::optional<PtcStatus> ParseStatus(const std::string& text);

// 一枚画像(规格 JSON 形状的运行对象)。
struct PtcProfile {
    std::string fingerprint;
    PtcStatus status = PtcStatus::Unknown;
    std::string language = "python";
    // 探针成绩(verified 的依据;0 = 未测)。
    double single_call_accuracy = 0.0;
    double chain_accuracy = 0.0;
    double fanout_accuracy = 0.0;
    double runtime_error_rate = 0.0;
    int max_verified_chain = 0;
    int max_verified_fanout = 0;
    std::string verified_at;           // ISO 日期(手跑探针时填)
    std::string harness_revision;      // 验证时的 harness 版本
    nlohmann::json ToJson() const;
    static std::optional<PtcProfile> FromJson(const nlohmann::json& json);
};

// 画像指纹:任一成分变,指纹变。全部成分显式入串,不带默认魔数。
std::string BuildPtcFingerprint(const std::string& provider, const std::string& endpoint, const std::string& model,
                                const std::string& wire, const std::string& python_version,
                                const std::string& harness_revision);

// 五条硬条件(规格"先查五条硬条件"节):少一条,直接走 JSON。
struct PtcHardConditions {
    bool sandbox_reliable = false;     // 1. 平台有可靠沙箱(POSIX rlimit 不算)
    bool model_free_code = true;       // 2. 模型能输出自由代码(provider 没锁正文)
    bool context_fits_stubs = true;    // 3. 上下文装得下当前 stub 集
    bool tools_wired = true;           // 4. 入选工具已接 PTC RPC/权限/hooks/取消链
    bool python_version_ok = false;    // 5. Python 运行时版本符合宿主声明(>=3.9)

    bool AllMet() const { return sandbox_reliable && model_free_code && context_fits_stubs && tools_wired && python_version_ok; }
    // 未满足项的人读清单。
    std::vector<std::string> FailureTexts() const;
};

// auto 的门槛(规格"基准"节):verified + 硬条件齐 + (预估链长 >= 4 或
// fan-out >= 8)且入选工具都 PTC 就绪,才选 programmatic。首版没有 verified
// 画像,auto 恒落 json——门槛本身照规格实现,不拍脑袋放宽。
struct PtcAutoGates {
    PtcStatus profile_status = PtcStatus::Unknown;
    bool hard_conditions_met = false;
    int estimated_chain_depth = 0;   // 预估依赖链长
    int estimated_fanout = 0;        // 预估独立 fan-out
    int min_chain_depth = 4;         // 规格门槛:链长 >= 4
    int min_fanout = 8;              // 规格门槛:fan-out >= 8
};
enum class ToolCallingDecision { Json, Programmatic };
ToolCallingDecision ResolveToolCalling(const PtcAutoGates& gates);
std::string ToString(ToolCallingDecision decision);

// 熔断器(会话内):连续出现语法错/漏调用/RPC 协议错,本场从 PTC 降回
// JSON。计数规则:
//   - 语法错(Syntax)/RPC 协议错(Rpc/Protocol)+1;
//   - 空脚本(零调用且零 emit 的 Runtime)+1("漏调用");
//   - 工具层失败/异常收口不算——那是工具世界常态,不是模型不会写;
//   - 成功一次清零。
// 连续 N 次(默认 3)触发,降档后本会话不再升回。
class PtcCircuitBreaker {
public:
    explicit PtcCircuitBreaker(int threshold = 3) : threshold_(threshold) {}

    void Record(const PtcRunResult& run);
    bool Tripped() const { return tripped_; }
    int consecutive_faults() const { return consecutive_faults_; }
    // 触发原因(人读;未触发为空)。
    std::string Reason() const;

private:
    int threshold_;
    int consecutive_faults_ = 0;
    bool tripped_ = false;
    std::string last_fault_;
};

// 画像存档:<home>/.lubancode/ptc_profiles.json,一份 {fingerprint: profile}
// 的 object。坏文件不当错(返回空表,写回时重建)。
class PtcProfileStore {
public:
    explicit PtcProfileStore(std::string path) : path_(std::move(path)) {}
    // 读全量(文件不存在/坏 = 空)。
    std::vector<PtcProfile> Load() const;
    std::optional<PtcProfile> Find(const std::string& fingerprint) const;
    // 写一条(整文件重写;其余条目原样保留)。
    bool Save(const PtcProfile& profile, std::string* error = nullptr);

private:
    std::string path_;
};

// 存档默认路径:<home>/.lubancode/ptc_profiles.json(找不到 home = 空串,
// 调用方按"没有存档"处理)。
std::string DefaultProfileStorePath();

}  // namespace lubancode::ptc
