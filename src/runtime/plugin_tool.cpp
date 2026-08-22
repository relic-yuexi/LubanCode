// PluginToolAdapter + 目录扫描的实现。
#include "runtime/plugin_tool.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <set>

#include "platform/paths.hpp"
#include "runtime/plugin_process.hpp"

namespace lubancode::runtime {

namespace {

// 结果文本:成功时给响应正文;插件自报失败时给错误说明(带插件侧错误码,
// 模型看得见才能改参重试);宿主侧协议错给"错误码 + detail + exit",
// 诊断信息一次给足。
std::string BuildResultText(const ProcessCallOutcome& outcome) {
    if (outcome.code == PluginErrorCode::Ok) {
        return outcome.text;
    }
    if (outcome.code == PluginErrorCode::PluginReportedError) {
        std::string text = outcome.detail;
        if (!outcome.plugin_error_code.empty()) {
            text = "[" + outcome.plugin_error_code + "] " + text;
        }
        return text;
    }
    std::string text = std::string("[") + std::string(PluginErrorCodeName(outcome.code)) + "] " + outcome.detail;
    if (outcome.exit_code >= 0) {
        text += "(exit=" + std::to_string(outcome.exit_code) + ")";
    }
    return text;
}

}  // namespace

PluginToolAdapter::PluginToolAdapter(std::shared_ptr<const PluginManifest> manifest,
                                     const PluginDefinition* definition)
    : manifest_(std::move(manifest)), definition_(definition) {}

std::string PluginToolAdapter::name() const { return definition_->full_name; }

std::string PluginToolAdapter::description() const {
    // 模型可见文本:只有 description 本身。language/command/env/path/
    // timeout 等宿主元数据一个字节不进 prompt(单子「Schema 的方向不能倒」)。
    return definition_->description;
}

nlohmann::json PluginToolAdapter::input_schema() const { return definition_->input_schema; }

tools::Tool::Result PluginToolAdapter::execute(const nlohmann::json& input) {
    // 调用前统一验参(manifest 是合同)。实现层(process 协议)仍各自
    // 防御——Schema 不是内存安全。
    if (auto problem = ValidateArgumentsAgainstSchema(input, definition_->input_schema); problem.has_value()) {
        return {*problem, true};
    }

    if (manifest_->kind != RuntimeKind::Process) {
        // embedded-lua/native-library 在后续批次接入;manifest 校验期已把
        // native-library 拒了,这里兜 embedded-lua。
        return {"插件运行时 " + std::string(RuntimeKindName(manifest_->kind)) + " 尚未接入本宿主", true};
    }

    plugin_protocol::ProcessRequest request;
    request.plugin = manifest_->id;
    request.tool = definition_->name;
    request.entry = definition_->entry;
    request.arguments = input;
    request.context_cwd = cwd_utf8_;
    // call_id:宿主生成的非敏感串(协议只要求回显对上;与 turn/item 的
    // 对账在第 7 批接 Runtime/app-server 时换成真实 item id)。单调序号,
    // 可读可对账,不泄任何上下文。
    static std::atomic<std::uint64_t> call_seq{0};
    request.call_id = "call_" + std::to_string(call_seq.fetch_add(1));

    ProcessCallLimits limits;
    limits.timeout_ms = manifest_->timeout_ms;
    const auto outcome = RunProcessToolCall(*manifest_, request, cwd_utf8_, cancel_, limits);
    // 唯一终态:错误码 + 人话一起交上层(ItemCompleted 一笔)。
    return {BuildResultText(outcome), outcome.code != PluginErrorCode::Ok};
}

PluginScanResult ScanPluginDirectories(const std::filesystem::path& dir) {
    PluginScanResult result;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return result;  // 没有 plugins 目录是常态
    }
    // 先按目录名排序再解析:枚举次序随文件系统心情,钉死顺序跨进程/resume
    // 时注册次序才稳(前缀缓存守恒,与 DLL/Lua 扫描同一条规矩)。
    std::vector<std::filesystem::path> subdirs;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        subdirs.push_back(entry.path());
    }
    std::sort(subdirs.begin(), subdirs.end(), [](const auto& left, const auto& right) {
        return platform::PathToUtf8(left.filename()) < platform::PathToUtf8(right.filename());
    });

    std::set<std::string> seen_tool_names;  // 跨插件重名在注册前拒
    for (const auto& subdir : subdirs) {
        const std::filesystem::path manifest_path = subdir / "plugin.json";
        if (!std::filesystem::is_regular_file(manifest_path, ec)) {
            continue;  // 没有 plugin.json 的目录不是插件(可能是 venv/bin 之类),静默略过
        }
        std::ifstream in(manifest_path, std::ios::binary);
        if (!in.is_open()) {
            result.warnings.push_back("[plugin] " + platform::PathToUtf8(subdir.filename()) +
                                      ": plugin.json 读不到,跳过");
            continue;
        }
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto manifest = ParsePluginManifest(text, subdir);
        if (!manifest.has_value()) {
            result.warnings.push_back("[plugin] " + platform::PathToUtf8(subdir.filename()) + ": " +
                                      manifest.error() + ",跳过");
            continue;
        }
        // 跨插件重名:撞了拒后到的,点名两方,先到先得顺序稳定(扫描已排序,
        // "先到"是确定的)。
        bool rejected = false;
        for (const auto& tool : manifest->tools) {
            if (seen_tool_names.count(tool.full_name) != 0) {
                result.warnings.push_back("[plugin] " + manifest->id + ": 工具 " + tool.name +
                                          " 的完整名与先前插件撞了(" + tool.full_name + "),整件跳过");
                rejected = true;
                break;
            }
        }
        if (rejected) {
            continue;
        }
        for (const auto& tool : manifest->tools) {
            seen_tool_names.insert(tool.full_name);
        }
        result.manifests.push_back(std::make_shared<const PluginManifest>(std::move(*manifest)));
    }
    return result;
}

}  // namespace lubancode::runtime
