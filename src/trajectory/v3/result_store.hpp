// 工具结果仓与模型预览(§4.16-4.19):全文收集与模型预览分开——原文按
// artifact 不可变落档,模型只看 32 KiB 内的最终文本预览(含说明区、文件/
// 通道标记、省略提示,统一量 UTF-8 字节;容量恢复按 §4.38 降到 16/8/4 KiB)。
//
// 目录合同(§4.16):sessions/<sessionId>/artifacts/<resultId>.json 为该次
// 结果的不可变描述;.json 不是状态文件,状态变化只追加 JSONL。
// 落稳次序:先临时文件 -> 再落稳 -> 发布不可变名;随后由调用方在账上落
// tool.result.persisted(result_ref 数组 + 执行终态引用)。
//
// 预览规则(§4.17):未超限原样返回;超限先留说明预算 M,正文 B=budget-M
// 头尾均分(floor(B/2) / B-floor(B/2)),UTF-8 边界对齐,头尾段各重新标
// 来源;清单自身超限时另存完整清单 artifact(output_index)+省略计数;
// 4 KiB 仍放不下必要来源记 preview_unrepresentable,不悄悄丢来源。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::trajectory::v3 {

// ---------------------------------------------------------------------------
// 模型预览生成(纯函数,无 IO;输入来自结果仓或内存捕获)
// ---------------------------------------------------------------------------

struct PreviewChannel {
    std::string display_path;  // result_ref 里能唯一对应的路径(不用 basename)
    std::string channel;       // stdout/stderr/combined/...
    std::string text;          // 该通道已捕获文本(可空)
    bool capture_complete = true;
    std::string capture_reason;         // capture_complete=false 时必填
    std::uint64_t output_bytes = 0;     // 实际观察到的原始输出字节数
    bool output_bytes_lower_bound = false;  // 仅下界时显示"至少 N bytes"
};

struct PreviewRequest {
    std::optional<std::int64_t> exit_code;  // 进程工具;非进程工具缺省
    std::vector<PreviewChannel> channels;   // 按稳定通道次序
    std::uint64_t max_preview_bytes = 32768;
    // 清单自身超限时,调用方先把完整清单存成 artifact 再把路径传入,
    // 预览只列容纳得下的路径 + omitted_output_count。
    std::optional<std::string> output_index_path;
};

struct PreviewResult {
    std::string text;  // 模型可见的最终文本(UTF-8 字节数 ≤ max_preview_bytes)
    bool truncated = false;        // 预览省略了正文(≠ 捕获不完整)
    std::vector<std::string> full_output;      // 收全材料的路径(总为数组)
    std::vector<std::string> captured_output;  // 部分材料的路径
    // 清单装不下且没有 output_index 可指:调用方拿 listing_text 存 artifact
    // 后带 output_index_path 重算。
    bool listing_overflow = false;
    std::string listing_text;
    std::optional<std::string> output_index_path;
    int omitted_output_count = 0;
    bool preview_unrepresentable = false;  // 必要来源也装不下(§4.38)
};

PreviewResult BuildToolPreview(const PreviewRequest& request);

// 降档档位(§4.38):32 -> 16 -> 8 -> 4 KiB,任一档够用就停。
inline constexpr std::uint64_t kPreviewBudgets[] = {32768, 16384, 8192, 4096};

// ---------------------------------------------------------------------------
// 结果仓(session 目录内的不可变 artifact 管理)
// ---------------------------------------------------------------------------

class ResultStore {
public:
    // session_dir:含 <sessionId>.jsonl 的目录;artifacts/ 建在其下。
    static std::expected<ResultStore, std::string> Open(
        const std::filesystem::path& session_dir);

    struct ChannelOutput {
        std::string channel;                // stdout/stderr/combined/report/...
        std::string media_type = "text/plain";
        std::string data;                   // 实际保存的原始输出(可空)
        bool capture_complete = true;
        std::string capture_reason;         // 不完整时必填(quota/io_error/...)
        std::uint64_t output_bytes = 0;     // 实际观察到的原始输出字节数
        bool output_bytes_lower_bound = false;
        std::string encoding = "utf-8";
    };

    struct PersistRequest {
        std::string result_kind;  // process/text/structured/multimodal(§4.16)
        std::vector<ChannelOutput> outputs;
        nlohmann::json content;            // 可选:文本正文(如搜索摘要)
        nlohmann::json structured_content; // 可选:结构化内容;缺失≠空对象
        std::string execution_event_ref;   // 执行终态 eventId(账上为准)
        nlohmann::json preview_policy;     // 准入时留的策略快照
        nlohmann::json capture_limits;
        std::string tool_call_id;
        std::uint64_t attempt = 1;
    };

    struct PersistedResult {
        std::string result_id;
        std::vector<nlohmann::json> result_ref;  // 六键 artifactRef 数组(含 metadata)
        bool ok = false;
        std::string error;
    };

    // 先临时文件 -> 落稳 -> 发布不可变名;返回 result_ref 供
    // tool.result.persisted 落账。空通道且收全:不造空文件,描述里记
    // bytes=0/capture_complete=true(§4.16)。
    PersistedResult Persist(const PersistRequest& request);

    // 完整清单 artifact(§4.17 极端情况):清单自身超限时由调用方先存。
    std::expected<std::string, std::string> PersistListing(const std::string& listing_name,
                                                           const std::string& text);

    const std::filesystem::path& artifacts_dir() const { return artifacts_dir_; }

private:
    explicit ResultStore(std::filesystem::path artifacts_dir);
    std::filesystem::path artifacts_dir_;
    // 下一枚 result 号:开仓时扫已有 res-*.json 取最大 +1。
    std::uint64_t next_result_number_ = 1;
};

// 六键 artifactRef 组装(§3.1)。
nlohmann::json MakeArtifactRef(std::string artifact_id, std::string kind, std::string path,
                               std::string sha256, std::uint64_t bytes, std::string media_type);

}  // namespace lubancode::trajectory::v3
