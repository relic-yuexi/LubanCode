// hook 宿主能力束实现(LuaHook 单 P1-C,§五/§5.1)。Lua 边界的形状转换
// 在 plugin_lua_host.cpp;这里的每一件都是宿主执行件——路径授权、字节帽、
// 原子写、状态限额、子执行记账与递归治理都不靠脚本自觉。
#include "runtime/hook_host_services.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <utility>

#include "tools/schema_check.hpp"  // ValidateInputAgainstSchema:准入的 schema 复验

namespace lubancode::runtime {

namespace {

HookApiError FsError(std::string_view code, std::string message) {
    return HookApiError{std::string(code), std::move(message)};
}

// 词法包含:candidate 是否落在 root 下(含 root 本身)。按组件比,不碰
// native()(Windows 是 wstring);rel 为空(不同盘/无关)或含 ".." 组件
// 都算不在。
bool PathIsUnder(const std::filesystem::path& candidate, const std::filesystem::path& root) {
    const std::filesystem::path rel = candidate.lexically_relative(root);
    if (rel.empty()) {
        return false;
    }
    for (const auto& part : rel) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

// 符号链接逃逸复核:按 weakly_canonical(不在场的尾巴保持词法)再比一次
// 授权根。根自己不在场(新包目录)时退词法比对——授权根是宿主发的,
// 词法一致就够了;脚本造的符号链接指到根外会在 canonical 侧露馅。
std::filesystem::path BestEffortCanonical(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : canonical;
}

bool LooksLikeEscapingRelative(const std::filesystem::path& normalized) {
    for (const auto& part : normalized) {
        if (part == "..") {
            return true;
        }
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// 受控文件
// ---------------------------------------------------------------------------

std::expected<std::filesystem::path, HookApiError> AuthorizeHookFsPath(const HookFsGrant& grant,
                                                                       std::string_view raw_path,
                                                                       bool for_write) {
    if (raw_path.empty()) {
        return std::unexpected(FsError(hookapi_err::kFsBadPath, "路径是空串"));
    }
    const std::filesystem::path base = grant.base.lexically_normal();
    const std::filesystem::path raw(raw_path.begin(), raw_path.end());
    if (raw.empty()) {
        return std::unexpected(FsError(hookapi_err::kFsBadPath, "路径是空串"));
    }
    // NTFS 备用数据流("foo:stream")一类:路径里出现冒号以外的盘符语义时
    // 拒之(Windows 盘符 "C:" 在绝对路径分支按平台认)。
    std::filesystem::path resolved = raw.is_absolute() ? raw : base / raw;
    resolved = resolved.lexically_normal();
    if (resolved.empty() || LooksLikeEscapingRelative(resolved)) {
        return std::unexpected(FsError(hookapi_err::kFsBadPath, "路径越界: " + std::string(raw_path)));
    }

    const std::vector<std::filesystem::path>& roots = for_write ? grant.write_roots : grant.read_roots;
    for (const std::filesystem::path& root : roots) {
        std::filesystem::path abs_root = root.is_absolute() ? root : base / root;
        abs_root = abs_root.lexically_normal();
        if (!PathIsUnder(resolved, abs_root)) {
            continue;
        }
        const std::filesystem::path canon_candidate = BestEffortCanonical(resolved);
        const std::filesystem::path canon_root = BestEffortCanonical(abs_root);
        if (!PathIsUnder(canon_candidate, canon_root)) {
            continue;  // 符号链接逃逸:换下一枚根,都不中即拒
        }
        return canon_candidate;
    }
    return std::unexpected(FsError(hookapi_err::kFsPathDenied,
                                   "路径不在授权根内(读取与写入各有一份授权子树)"));
}

namespace {

void RecordFsOp(LuaHookServices& services, const char* op, const std::filesystem::path& path,
                std::uint64_t bytes) {
    services.fs_ops.push_back(nlohmann::json{{"op", op},
                                             {"path", path.generic_string()},
                                             {"bytes", bytes}});
}

}  // namespace

// 读文件:授权 -> 在场与常规检查 -> 字节帽 -> 读全文。不存在/非常规文件
// 按 kFsMissing/IO 错收口。
std::expected<std::string, HookApiError> ReadHookFsFile(LuaHookServices& services, std::string_view raw_path) {
    auto authorized = AuthorizeHookFsPath(services.fs, raw_path, /*for_write=*/false);
    if (!authorized.has_value()) {
        return std::unexpected(authorized.error());
    }
    std::error_code ec;
    if (!std::filesystem::exists(*authorized, ec) || ec) {
        return std::unexpected(FsError(hookapi_err::kFsMissing, "文件不存在"));
    }
    if (!std::filesystem::is_regular_file(*authorized, ec) || ec) {
        return std::unexpected(FsError(hookapi_err::kFsIoError, "不是常规文件"));
    }
    const std::uintmax_t size = std::filesystem::file_size(*authorized, ec);
    if (ec) {
        return std::unexpected(FsError(hookapi_err::kFsIoError, "读文件大小失败"));
    }
    if (size > services.fs.max_read_bytes) {
        return std::unexpected(FsError(hookapi_err::kFsTooLarge,
                                       "文件 " + std::to_string(size) + " 字节,超单次读帽 " +
                                           std::to_string(services.fs.max_read_bytes)));
    }
    std::ifstream file(*authorized, std::ios::binary);
    if (!file) {
        return std::unexpected(FsError(hookapi_err::kFsIoError, "文件打不开"));
    }
    std::string content(static_cast<std::size_t>(size), '\0');
    if (size > 0 && !file.read(content.data(), static_cast<std::streamsize>(size))) {
        return std::unexpected(FsError(hookapi_err::kFsIoError, "文件读不全"));
    }
    RecordFsOp(services, "read", *authorized, size);
    return content;
}

// 原子写:同目录临时文件落稳再 rename 顶替(§五"原子写");父目录须在场
//(不替脚本隐式建目录);content 超帽即拒。
std::expected<bool, HookApiError> WriteHookFsFile(LuaHookServices& services, std::string_view raw_path,
                                                  std::string_view content) {
    if (content.size() > services.fs.max_write_bytes) {
        return std::unexpected(FsError(hookapi_err::kFsTooLarge,
                                       "写入 " + std::to_string(content.size()) + " 字节,超单次写帽 " +
                                           std::to_string(services.fs.max_write_bytes)));
    }
    auto authorized = AuthorizeHookFsPath(services.fs, raw_path, /*for_write=*/true);
    if (!authorized.has_value()) {
        return std::unexpected(authorized.error());
    }
    const std::filesystem::path parent = authorized->parent_path();
    std::error_code ec;
    if (!std::filesystem::is_directory(parent, ec) || ec) {
        return std::unexpected(FsError(hookapi_err::kFsMissing, "父目录不存在(不隐式建目录)"));
    }
    // 同目录临时文件:rename 落在同一个卷上才是原子的。
    static std::atomic<std::uint64_t> temp_counter{0};
    const std::filesystem::path temp =
        parent / (".luban-hook-" + std::to_string(temp_counter.fetch_add(1)) + ".tmp");
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            return std::unexpected(FsError(hookapi_err::kFsIoError, "临时文件建不出"));
        }
        if (!content.empty() && !file.write(content.data(), static_cast<std::streamsize>(content.size()))) {
            std::filesystem::remove(temp, ec);
            return std::unexpected(FsError(hookapi_err::kFsIoError, "临时文件写不满"));
        }
        file.flush();
    }
    // Windows/POSIX 同语义:rename 顶替既有目标(MSVC 实现走
    // MoveFileEx REPLACE_EXISTING)。
    std::filesystem::rename(temp, *authorized, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return std::unexpected(FsError(hookapi_err::kFsIoError, "原子替换失败: " + ec.message()));
    }
    RecordFsOp(services, "write", *authorized, content.size());
    return true;
}

// 列目录:一层,名字 + 是否目录;超 max_list_entries 即拒(不悄悄截断
// ——目录清单也是一次执行事实)。
std::expected<std::vector<std::pair<std::string, bool>>, HookApiError> ListHookFsDir(
    LuaHookServices& services, std::string_view raw_path) {
    auto authorized = AuthorizeHookFsPath(services.fs, raw_path, /*for_write=*/false);
    if (!authorized.has_value()) {
        return std::unexpected(authorized.error());
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(*authorized, ec) || ec) {
        return std::unexpected(FsError(hookapi_err::kFsMissing, "目录不存在"));
    }
    std::vector<std::pair<std::string, bool>> entries;
    for (std::filesystem::directory_iterator it(*authorized, ec), end; !ec && it != end; it.increment(ec)) {
        if (static_cast<int>(entries.size()) >= services.fs.max_list_entries) {
            return std::unexpected(FsError(hookapi_err::kFsTooLarge,
                                           "目录条目超帽 " + std::to_string(services.fs.max_list_entries)));
        }
        const std::string name = it->path().filename().generic_string();
        std::error_code dir_ec;
        const bool is_dir = it->is_directory(dir_ec) && !dir_ec;
        entries.emplace_back(name, is_dir);
    }
    if (ec) {
        return std::unexpected(FsError(hookapi_err::kFsIoError, "目录迭代失败: " + ec.message()));
    }
    RecordFsOp(services, "list", *authorized, entries.size());
    return entries;
}

// ---------------------------------------------------------------------------
// 插件状态
// ---------------------------------------------------------------------------

std::optional<HookApiError> HookStateStore::ValidateKey(const std::string& key) {
    if (key.empty() || key.size() > 128) {
        return HookApiError{std::string(hookapi_err::kStateBadKey), "key 须是 1..128 字符"};
    }
    for (const char c : key) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '_' || c == '-';
        if (!ok) {
            return HookApiError{std::string(hookapi_err::kStateBadKey), "key 只认 [A-Za-z0-9._-]"};
        }
    }
    return std::nullopt;
}

std::expected<nlohmann::json, HookApiError> HookStateStore::Get(const std::string& package,
                                                                const std::string& key) const {
    if (const auto key_error = ValidateKey(key)) {
        return std::unexpected(*key_error);
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto pkg = values_.find(package);
    if (pkg == values_.end()) {
        return nlohmann::json();
    }
    const auto entry = pkg->second.find(key);
    if (entry == pkg->second.end()) {
        return nlohmann::json();
    }
    return entry->second;
}

std::expected<bool, HookApiError> HookStateStore::Set(const std::string& package, const std::string& key,
                                                      nlohmann::json value) {
    if (const auto key_error = ValidateKey(key)) {
        return std::unexpected(*key_error);
    }
    const std::uint64_t bytes = ValueBytes(value);
    if (bytes > limits_.max_value_bytes) {
        return std::unexpected(HookApiError{
            std::string(hookapi_err::kStateQuota),
            "单值 " + std::to_string(bytes) + " 字节,超帽 " + std::to_string(limits_.max_value_bytes)});
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    auto& pkg = values_[package];
    const auto existing = pkg.find(key);
    std::uint64_t total = totals_[package];
    if (existing != pkg.end()) {
        total -= ValueBytes(existing->second);
    } else if (pkg.size() + 1 > limits_.max_keys_per_package) {
        return std::unexpected(HookApiError{std::string(hookapi_err::kStateQuota),
                                            "包内键数超帽 " + std::to_string(limits_.max_keys_per_package)});
    }
    if (total + bytes > limits_.max_total_bytes_per_package) {
        return std::unexpected(HookApiError{std::string(hookapi_err::kStateQuota),
                                            "包内总字节超帽 " +
                                                std::to_string(limits_.max_total_bytes_per_package)});
    }
    pkg[key] = std::move(value);
    totals_[package] = total + bytes;
    return true;
}

std::vector<std::string> HookStateStore::Keys(const std::string& package) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> keys;
    const auto pkg = values_.find(package);
    if (pkg != values_.end()) {
        for (const auto& [key, value] : pkg->second) {
            keys.push_back(key);
        }
    }
    return keys;  // map 本身有序
}

// ---------------------------------------------------------------------------
// context 候选
// ---------------------------------------------------------------------------

std::optional<HookApiError> HookContextCollector::Append(std::string text, std::string source) {
    if (text.empty()) {
        return HookApiError{std::string(hookapi_err::kContextBadText), "追加候选的 text 是空串"};
    }
    if (entries.size() + 1 > limits.max_entries) {
        return HookApiError{std::string(hookapi_err::kContextQuota),
                            "候选条数超帽 " + std::to_string(limits.max_entries)};
    }
    if (total_bytes + text.size() > limits.max_total_bytes) {
        return HookApiError{std::string(hookapi_err::kContextQuota),
                            "候选总字节超帽 " + std::to_string(limits.max_total_bytes)};
    }
    total_bytes += text.size();
    entries.push_back(Entry{std::move(text), std::move(source)});
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// 结构化日志
// ---------------------------------------------------------------------------

nlohmann::json SanitizeHookLogFields(const nlohmann::json& fields) {
    constexpr std::size_t kMaxFieldValueChars = 2048;
    if (!fields.is_object()) {
        return nlohmann::json::object();
    }
    nlohmann::json out = nlohmann::json::object();
    for (const auto& [key, value] : fields.items()) {
        if (value.is_string()) {
            const std::string& text = value.get_ref<const std::string&>();
            if (text.size() > kMaxFieldValueChars) {
                out[key] = nlohmann::json{{"text", text.substr(0, kMaxFieldValueChars)},
                                          {"truncated", true},
                                          {"originalChars", text.size()}};
                continue;
            }
        }
        out[key] = value;
    }
    return out;
}

// ---------------------------------------------------------------------------
// 子执行记账
// ---------------------------------------------------------------------------

std::string NextHookSubExecutionId() {
    static std::atomic<std::uint64_t> counter{0};
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    return "hookexec_" + std::to_string(ms) + "_" + std::to_string(counter.fetch_add(1));
}

nlohmann::json HookSubExecutionRecord::ToJson() const {
    nlohmann::json out = nlohmann::json::object(
        {{"executionId", execution_id},
         {"hookDispatchId", hook_dispatch_id},
         {"hookInvocationId", hook_invocation_id},
         {"hookId", hook_id},
         {"logicalTool", logical_tool},
         {"backend", backend},
         {"input", input},
         {"admitted", admitted},
         {"terminal", terminal},
         {"executionUncertain", execution_uncertain},
         {"durationMs", duration_ms}});
    if (parent_action_id.has_value()) {
        out["parentActionId"] = *parent_action_id;
    }
    if (turn_id.has_value()) {
        out["turnId"] = *turn_id;
    }
    if (step_id.has_value()) {
        out["stepId"] = *step_id;
    }
    if (!error_code.empty()) {
        out["errorCode"] = error_code;
    }
    if (!result_summary.is_null()) {
        out["resultSummary"] = result_summary;
    }
    return out;
}

namespace {

using trajectory::v3::Durability;
using trajectory::v3::ToolActionSession;
using trajectory::v3::ToolIdentity;
using trajectory::v3::WriteReceipt;

class V3SubExecutionSession final : public HookSubExecutionLedger::Session {
public:
    V3SubExecutionSession(trajectory::v3::V3Writer& writer, ToolActionSession session,
                          HookSubExecutionRecord record)
        : writer_(writer), session_(std::move(session)), record_(std::move(record)) {}

    void Started(const nlohmann::json& tool_identity, const std::string& effective_args_ref) override {
        ToolIdentity identity;
        identity.logical_name = record_.logical_tool;
        if (tool_identity.is_object()) {
            identity.registration_source = tool_identity.value("registrationSource", std::string());
            identity.version = tool_identity.value("version", std::string());
            identity.execution_scope = tool_identity.value("executionScope", std::string());
        }
        (void)session_.Start(writer_, effective_args_ref, std::move(identity), std::nullopt,
                             nlohmann::json::object(), Durability::ProcessCrash);
    }

    void Reject(const std::string& reason) override {
        (void)session_.Reject(writer_, reason, Durability::PowerLoss);
    }
    void Finish(std::optional<std::int64_t> exit_code) override {
        (void)session_.Finish(writer_, exit_code, std::nullopt, Durability::PowerLoss);
    }
    void Fail(const std::string& error_code) override {
        (void)session_.Fail(writer_, error_code, std::nullopt, Durability::PowerLoss);
    }
    void Cancel(const std::string& reason) override {
        (void)session_.Cancel(writer_, /*phase=*/"during_execution", reason, Durability::PowerLoss);
    }
    void MarkUnknown(const std::string& reason) override {
        (void)session_.MarkUnknown(writer_, reason, Durability::PowerLoss);
    }

private:
    trajectory::v3::V3Writer& writer_;
    ToolActionSession session_;
    HookSubExecutionRecord record_;
};

}  // namespace

std::unique_ptr<HookSubExecutionLedger::Session> V3HookSubExecutionLedger::Open(
    const HookSubExecutionRecord& record) {
    // linkage 与 hooks.cpp BeginSubExecution 同形(§4.49/§4.23):独立
    // actionId,payload 带 parentActionId/hookDispatchId/hookInvocationId/
    // logicalTool/backend;不冒充模型 tool_call,不向 provider 塞 tool 消息。
    nlohmann::json linkage = nlohmann::json::object(
        {{"parentActionId", record.parent_action_id.value_or(std::string())},
         {"hookDispatchId", record.hook_dispatch_id},
         {"hookInvocationId", record.hook_invocation_id},
         {"hookId", record.hook_id},
         {"logicalTool", record.logical_tool},
         {"backend", record.backend},
         {"source", "hook_subexecution"}});
    ToolActionSession session = ToolActionSession::Admit(
        *writer_, record.turn_id.value_or(std::string()), record.step_id.value_or(std::string()),
        record.execution_id, "hook_subexecution", std::nullopt, std::nullopt, std::move(linkage),
        Durability::ProcessCrash);
    return std::make_unique<V3SubExecutionSession>(*writer_, std::move(session), record);
}

// ---------------------------------------------------------------------------
// 统一工具执行服务
// ---------------------------------------------------------------------------

namespace {

// 子执行的线程内嵌套深度:工具执行里再触发 hook、hook 再调工具,深度
// 逐层记账(§5.1 拒绝无界递归)。thread_local——hook 与工具执行同线程
// 串行,线程即栈。
int& SubExecutionDepth() {
    thread_local int depth = 0;
    return depth;
}

struct DepthGuard {
    int& depth;
    explicit DepthGuard(int& d) : depth(d) { ++depth; }
    ~DepthGuard() { --depth; }
};

std::string_view ToolOutcome(const tools::Tool::Result& result) { return result.outcome; }

// 工具身份快照(started 事件用;registration 的来源/版本账,不猜)。
nlohmann::json ToolIdentityOf(const tools::ToolRegistry* registry, const std::string& name) {
    const tools::ToolRegistration* registration = registry->RegistrationOf(name);
    nlohmann::json identity = nlohmann::json::object();
    if (registration != nullptr) {
        identity["registrationSource"] = registration->source_instance.empty()
                                             ? std::string("builtin")
                                             : registration->source_instance;
        identity["version"] = registration->version_or_digest;
    } else {
        identity["registrationSource"] = "builtin";
    }
    return identity;
}

// effectiveArgsRef:小参数内联 dump,大参数截断标记(事件账不塞全文)。
std::string ArgsRef(const nlohmann::json& input) {
    constexpr std::size_t kMaxInlineChars = 4096;
    const std::string dumped = input.dump();
    if (dumped.size() > kMaxInlineChars) {
        return "truncated:" + dumped.substr(0, kMaxInlineChars);
    }
    return "inline:" + dumped;
}

// 结果正文摘要进 record(截断;全文不进事件账)。
nlohmann::json ResultSummary(const tools::Tool::Result& result, std::uint64_t cap) {
    nlohmann::json summary = nlohmann::json::object();
    summary["isError"] = result.is_error;
    if (!result.outcome.empty()) {
        summary["outcome"] = result.outcome;
    }
    if (!result.error_code.empty()) {
        summary["errorCode"] = result.error_code;
    }
    std::string content = result.content;
    if (content.size() > cap) {
        summary["contentPreview"] = content.substr(0, static_cast<std::size_t>(cap));
        summary["contentTruncated"] = true;
        summary["contentBytes"] = content.size();
    } else {
        summary["content"] = std::move(content);
    }
    return summary;
}

std::string BackendLabel(const tools::ToolRegistry* registry, const std::string& name) {
    const tools::ToolRegistration* registration = registry->RegistrationOf(name);
    if (registration == nullptr) {
        return "unknown";
    }
    switch (registration->source_kind) {
        case tools::ToolSourceKind::Mcp:
            return "mcp:" + (registration->source_instance.empty() ? std::string("?")
                                                                   : registration->source_instance);
        case tools::ToolSourceKind::PluginLua:
            return "plugin_lua:" + registration->source_instance;
        case tools::ToolSourceKind::PluginNative:
            return "plugin_native:" + registration->source_instance;
        case tools::ToolSourceKind::Builtin:
            return "builtin";
        default:
            return registration->source_instance.empty() ? std::string("other")
                                                         : "other:" + registration->source_instance;
    }
}

}  // namespace

std::vector<std::pair<std::string, std::string>> HookToolExecutionService::ListAuthorized() const {
    std::vector<std::pair<std::string, std::string>> out;
    if (options_.registry == nullptr) {
        return out;
    }
    for (const std::string& allowed : options_.allow_tools) {
        const tools::Tool* tool = options_.registry->Find(allowed);
        if (tool == nullptr) {
            continue;
        }
        out.emplace_back(tool->name(), tool->description());
    }
    return out;
}

HookToolExecutionService::Result HookToolExecutionService::Call(
    const std::string& tool_name, const nlohmann::json& input, const std::string& hook_id,
    const std::string& hook_dispatch_id, const std::string& hook_invocation_id,
    const std::optional<std::string>& parent_action_id, const std::optional<std::string>& turn_id,
    const std::optional<std::string>& step_id, const std::atomic<bool>* cancel) {
    Result out;
    HookSubExecutionRecord record;
    record.execution_id = NextHookSubExecutionId();
    record.hook_id = hook_id;
    record.hook_dispatch_id = hook_dispatch_id;
    record.hook_invocation_id = hook_invocation_id;
    record.parent_action_id = parent_action_id;
    record.turn_id = turn_id;
    record.step_id = step_id;
    record.logical_tool = tool_name;
    record.input = input;
    out.execution_id = record.execution_id;

    const auto rejected = [&out, &record, this](std::string_view code, std::string message) {
        out.admitted = false;
        out.status = "rejected";
        out.error_code = std::string(code);
        out.message = message;
        record.admitted = false;
        record.terminal = "rejected";
        record.error_code = out.error_code;
        // 准入拒绝也入账(tool.execution.rejected):调用过谁、为何拒,审计
        // 看得见——"没有执行"不等于"没有发生"(§4.14 rejected 无 started)。
        if (options_.ledger != nullptr) {
            auto session = options_.ledger->Open(record);
            session->Reject(out.message);
        }
        out.record = record.ToJson();
        return out;
    };

    // ---- 准入(§5.1:模型调用与 hook 子执行共用准入要素;不因避递归跳过)。
    if (options_.registry == nullptr) {
        return rejected(hookapi_err::kToolRegistryMissing, "宿主未接工具注册表");
    }
    tools::Tool* tool = options_.registry->Find(tool_name);
    if (tool == nullptr) {
        return rejected(hookapi_err::kToolUnknown, "工具未注册: " + tool_name);
    }
    const bool allowed =
        std::find(options_.allow_tools.begin(), options_.allow_tools.end(), tool_name) !=
        options_.allow_tools.end();
    if (!allowed) {
        return rejected(hookapi_err::kToolNotAllowed, "工具不在 hook 的授权名单内: " + tool_name);
    }
    if (!input.is_object()) {
        return rejected(hookapi_err::kToolBadInput, "工具入参须是 JSON object");
    }
    if (const std::optional<std::string> schema_error =
            tools::ValidateInputAgainstSchema(input, tool->input_schema());
        schema_error.has_value()) {
        return rejected(hookapi_err::kToolBadInput, "入参不合 schema: " + *schema_error);
    }

    // ---- 递归治理(§5.1:ancestry/深度/总次数)。深度帽按"在途执行层数"
    // 记:顶层调用时在途 0;工具执行里再触发 hook 又调工具,在途逐层 +1,
    // 达帽即拒(不因避递归跳过上面已过的准入)。
    if (SubExecutionDepth() >= options_.max_recursion_depth) {
        return rejected(hookapi_err::kToolRecursionDepth,
                        "子执行嵌套深度超帽 " + std::to_string(options_.max_recursion_depth));
    }
    if (total_calls_ + 1 > options_.max_calls_per_invocation) {
        return rejected(hookapi_err::kToolCallCap,
                        "本 invocation 工具调用超帽 " + std::to_string(options_.max_calls_per_invocation));
    }
    int& same_tool = calls_per_tool_[tool_name];
    if (same_tool + 1 > options_.max_same_tool_calls) {
        return rejected(hookapi_err::kToolRepeatCap,
                        "同一工具在本 invocation 重复超帽 " + std::to_string(options_.max_same_tool_calls) +
                            "(副作用不自动重试,§5.1)");
    }

    record.admitted = true;
    record.backend = BackendLabel(options_.registry, tool_name);
    std::unique_ptr<HookSubExecutionLedger::Session> ledger_session;
    if (options_.ledger != nullptr) {
        ledger_session = options_.ledger->Open(record);
    }

    // ---- 执行:模型 Action 同一底座(Tool::execute + 取消旗贯通)。
    ++total_calls_;
    ++same_tool;
    const auto started_at = std::chrono::steady_clock::now();
    tools::ToolExecutionContext context;
    context.cancel = cancel;
    tools::Tool::Result result;
    {
        DepthGuard guard(SubExecutionDepth());
        try {
            result = tool->execute(input, context);
        } catch (const std::exception& e) {
            result = tools::Tool::Result::Error(std::string("工具执行抛异常: ") + e.what());
            result.outcome = "tool_exception";
        } catch (...) {
            result = tools::Tool::Result::Error("工具执行抛未知异常");
            result.outcome = "tool_exception";
        }
    }
    record.duration_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at)
            .count());
    record.result_summary = ResultSummary(result, options_.result_preview_bytes);

    // 结果正文回 Lua(截到 preview 帽)。
    out.admitted = true;
    out.is_error = result.is_error;
    if (result.content.size() > options_.result_preview_bytes) {
        out.content = result.content.substr(0, static_cast<std::size_t>(options_.result_preview_bytes));
        out.content_truncated = true;
    } else {
        out.content = result.content;
    }
    out.details = result.details;
    if (!result.outcome.empty()) {
        out.error_code = result.outcome == "tool_error" && !result.error_code.empty() ? result.error_code
                                                                                     : result.outcome;
    }
    if (!result.error_code.empty() && result.outcome != "tool_error") {
        out.error_code = result.error_code;
    }

    // ---- 终态分型(§5.1/§六:超时/断连/取消不证明远端未执行 -> unknown;
    // 不自动重试副作用。终态与错误码照实记,uncertain 标远端不确定)。
    const std::string_view outcome = ToolOutcome(result);
    const bool cancel_observed = cancel != nullptr && cancel->load();
    if (!result.is_error) {
        out.status = "finished";
        record.terminal = "finished";
        if (ledger_session != nullptr) {
            ledger_session->Started(ToolIdentityOf(options_.registry, tool_name), ArgsRef(input));
            ledger_session->Finish(std::nullopt);
        }
    } else if (cancel_observed || outcome == "cancelled_during_run") {
        out.status = "cancelled";
        record.terminal = "cancelled";
        record.execution_uncertain = true;
        record.error_code = result.error_code.empty() ? std::string(outcome) : result.error_code;
        if (ledger_session != nullptr) {
            ledger_session->Started(ToolIdentityOf(options_.registry, tool_name), ArgsRef(input));
            ledger_session->Cancel(result.content);
        }
    } else if (outcome == "tool_error") {
        // 工具自己报错:执行已发生、结果已知——执行终态是 finished,工具级
        // 失败走 is_error/error_code(与 RunOneTool 的 Result 语义同拍)。
        out.status = "finished";
        record.terminal = "finished";
        record.error_code = result.error_code.empty() ? std::string(outcome) : result.error_code;
        if (ledger_session != nullptr) {
            ledger_session->Started(ToolIdentityOf(options_.registry, tool_name), ArgsRef(input));
            ledger_session->Finish(std::nullopt);
        }
    } else if (outcome == "tool_exception" || outcome.empty()) {
        // 本地执行抛异常/无 outcome 的裸失败:执行器失败,failed。
        out.status = "failed";
        record.terminal = "failed";
        record.error_code = result.error_code.empty() ? std::string(outcome) : result.error_code;
        if (ledger_session != nullptr) {
            ledger_session->Started(ToolIdentityOf(options_.registry, tool_name), ArgsRef(input));
            ledger_session->Fail(record.error_code);
        }
    } else {
        // timed_out / transport_error / protocol_error:响应没到或到而不可
        // 信——远端可能已执行,unknown,不重试(§5.1)。
        out.status = "unknown";
        record.terminal = "unknown";
        record.execution_uncertain = true;
        record.error_code = result.error_code.empty() ? std::string(outcome) : result.error_code;
        if (ledger_session != nullptr) {
            ledger_session->Started(ToolIdentityOf(options_.registry, tool_name), ArgsRef(input));
            ledger_session->MarkUnknown(record.error_code);
        }
    }
    out.record = record.ToJson();
    return out;
}

// ---------------------------------------------------------------------------
// 服务中心
// ---------------------------------------------------------------------------

std::unique_ptr<LuaHookServices> HookHostServiceCenter::Build(
    const std::vector<std::string>& requested_capabilities, const hooks::middleware::InvocationCtx& ctx,
    const std::string& package_id) {
    auto services = std::make_unique<LuaHookServices>();
    services->package_id = package_id;
    const auto requested = [&requested_capabilities](std::string_view cap) {
        return std::find(requested_capabilities.begin(), requested_capabilities.end(), cap) !=
               requested_capabilities.end();
    };

    // HTTP/Secret:申请 ∩ 宿主授权;取消旗灌 invocation 的那根(与 guard
    // 同一真值,§8.4 同款纪律)。
    if (requested(kHookCapHttp) && grants_.http.has_value()) {
        services->http_granted = true;
        services->http = *grants_.http;
        services->http.cancel = ctx.cancel;
    }

    // 文件:读/写分别申请;授权根按申请裁(read-only 申请不附写根)。
    if (grants_.fs.has_value() && (requested(kHookCapFsRead) || requested(kHookCapFsWrite))) {
        services->fs_granted = true;
        services->fs = *grants_.fs;
        if (!requested(kHookCapFsWrite)) {
            services->fs.write_roots.clear();
        }
        if (!requested(kHookCapFsRead)) {
            services->fs.read_roots.clear();
        }
    }

    if (requested(kHookCapState) && grants_.state != nullptr) {
        services->state = grants_.state.get();
    }

    if (requested(kHookCapContext)) {
        services->context_granted = true;  // 采用仍走效果管道的宿主校验
    }

    if (requested(kHookCapLog) && grants_.log) {
        services->log = grants_.log;
    }

    if (requested(kHookCapTools) && grants_.tool_registry != nullptr) {
        HookToolExecutionService::Options tool_options;
        tool_options.registry = grants_.tool_registry;
        tool_options.allow_tools = grants_.allow_tools;
        if (writer_.load() != nullptr) {
            tool_options.ledger = std::make_shared<V3HookSubExecutionLedger>(*writer_.load());
        }
        services->tools = std::make_unique<HookToolExecutionService>(std::move(tool_options));
    }
    return services;
}

HookHostServiceCenter& DefaultHookServiceCenter() {
    // 故意 leak:退出路径上 Lua state/会话析构次序争不过正确性。
    static HookHostServiceCenter* center = new HookHostServiceCenter();
    return *center;
}

}  // namespace lubancode::runtime
