#include "runtime/channel_file_delivery.hpp"

#include <fstream>
#include <memory>
#include "channel/tool_guard.hpp"
#include "platform/atomic_write.hpp"
#include "platform/paths.hpp"
#include "platform/sha256.hpp"
#include "runtime/channel_media_service.hpp"

namespace lubancode::runtime {
namespace {
constexpr std::size_t kMaxBytes = 20 * 1024 * 1024;
std::filesystem::path Manifest(const std::filesystem::path& root, const std::string& id) {
    return root / "requests" / (platform::Sha256Hex(id) + ".json");
}
bool Inside(const std::filesystem::path& path, const std::filesystem::path& root) {
    const auto p = platform::PathComparisonKey(path);
    auto r = platform::PathComparisonKey(root);
    if (!r.empty() && r.back() != '/') r += '/';
    return !r.empty() && p.rfind(r, 0) == 0;
}
std::optional<std::string> ReadBounded(const std::filesystem::path& path, std::size_t cap) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return std::nullopt;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > cap) return std::nullopt;
    std::ifstream stream(path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    stream.read(bytes.data(), static_cast<std::streamsize>(size));
    if (!stream || static_cast<std::size_t>(stream.gcount()) != size || stream.peek() != EOF)
        return std::nullopt;
    return bytes;
}
tools::Tool::Result Reply(const std::string& text, bool error = false) {
    tools::Tool::Result result;
    result.SetText(text);
    result.is_error = error;
    return result;
}
class SendFileTool final : public tools::Tool {
public:
    std::string name() const override { return "send_file"; }
    std::string description() const override {
        return "将当前工作区内一件文件随本轮回复发回当前 QQ 聊天。上限 20 MiB，每轮一件；"
               "成功只表示已暂存待投递，不代表 QQ 已收件。仅在用户请求取回文件时调用。";
    }
    bool needs_confirm() const override { return true; }
    tools::ApprovalClass approval_class() const override { return tools::ApprovalClass::External; }
    tools::EffectClass effect_class() const override { return tools::EffectClass::LocalReversible; }
    tools::Idempotency idempotency() const override { return tools::Idempotency::Idempotent; }
    nlohmann::json input_schema() const override {
        return {{"type", "object"}, {"properties", {{"path", {{"type", "string"},
            {"description", "工作区内文件路径，绝对路径或相对当前工作目录"}}}}},
            {"required", {"path"}}, {"additionalProperties", false}};
    }
    Result execute(const nlohmann::json& input) override { return ChannelFileDeliveryScope::Stage(input); }
};
}  // namespace

thread_local ChannelFileDeliveryScope* ChannelFileDeliveryScope::current_ = nullptr;
ChannelFileDeliveryScope::ChannelFileDeliveryScope(std::filesystem::path workspace,
    std::filesystem::path staging, std::string operation_id)
    : workspace_(std::move(workspace)), staging_(std::move(staging)), operation_id_(std::move(operation_id)),
      previous_(current_) { current_ = this; }
ChannelFileDeliveryScope::~ChannelFileDeliveryScope() { current_ = previous_; }

tools::Tool::Result ChannelFileDeliveryScope::Stage(const nlohmann::json& input) {
    if (current_ == nullptr) return Reply("send_file 只可在 QQ 聊天轮内使用。", true);
    if (!input.contains("path") || !input["path"].is_string() || input["path"].get<std::string>().empty())
        return Reply("请提供文件 path。", true);
    const auto& scope = *current_;
    auto path = platform::Utf8ToPath(input["path"].get<std::string>());
    if (path.is_relative()) path = scope.workspace_ / path;
    std::error_code ec;
    path = std::filesystem::weakly_canonical(path, ec);
    if (ec || !Inside(path, scope.workspace_)) return Reply("只可投递当前工作区内文件。", true);
    const auto blocked = channel::ChannelToolPathBlocked("read_file", {{"path", platform::PathToUtf8(path)}},
        channel::DefaultChannelProtectedPaths(), platform::PathToUtf8(scope.workspace_));
    if (!blocked.empty()) return Reply(blocked, true);
    const auto bytes = ReadBounded(path, kMaxBytes);
    if (!bytes) return Reply("文件读不了，或超过 20 MiB。", true);
    const auto hash = platform::Sha256Hex(*bytes);
    const auto frozen = scope.staging_ / (hash + ".bin");
    const auto manifest = Manifest(scope.staging_, scope.operation_id_);
    if (std::filesystem::exists(manifest, ec)) {
        const auto existing = StagedChannelFile(scope.staging_, scope.operation_id_);
        if (existing && existing->local_path == platform::PathToUtf8(frozen))
            return Reply("该文件已暂存，随本轮回复投递；请勿声称已经送达。");
        return Reply("本轮已有一件待投递文件，请下一轮再发其他文件。", true);
    }
    if (!platform::AtomicWriteFile(frozen, *bytes, platform::WriteDurability::ProcessCrashDurability))
        return Reply("文件暂存失败。", true);
    const auto name = SanitizeChannelAttachmentName(platform::PathToUtf8(path.filename()));
    const nlohmann::json record = {{"path", platform::PathToUtf8(frozen)}, {"name", name},
        {"sha256", hash}, {"size", bytes->size()}};
    if (!platform::AtomicWriteFile(manifest, record.dump(), platform::WriteDurability::ProcessCrashDurability))
        return Reply("投递清单保存失败。", true);
    return Reply("已暂存 " + name + "，随本轮回复投递；尚未确认 QQ 收件。");
}

std::optional<gateway::DurableReplyOutbox::ChannelAttachment> StagedChannelFile(
    const std::filesystem::path& staging, const std::string& operation_id) {
    const auto text = ReadBounded(Manifest(staging, operation_id), 16384);
    if (!text) return std::nullopt;
    try {
        const auto record = nlohmann::json::parse(*text);
        const auto path = platform::Utf8ToPath(record.at("path").get<std::string>());
        if (!Inside(path, staging)) return std::nullopt;
        const auto bytes = ReadBounded(path, kMaxBytes);
        if (!bytes || platform::Sha256Hex(*bytes) != record.at("sha256").get<std::string>()) return std::nullopt;
        gateway::DurableReplyOutbox::ChannelAttachment result;
        result.local_path = platform::PathToUtf8(path);
        result.file_name = SanitizeChannelAttachmentName(record.at("name").get<std::string>());
        result.mime_type = "application/octet-stream";
        result.size_bytes = static_cast<std::int64_t>(bytes->size());
        return result;
    } catch (const nlohmann::json::exception&) { return std::nullopt; }
}

void RegisterChannelFileTool(tools::ToolRegistry& registry) {
    if (registry.Find("send_file") == nullptr) registry.Register(std::make_unique<SendFileTool>());
}
}  // namespace lubancode::runtime
