// PluginToolAdapter + 目录扫描的实现。
#include "runtime/plugin_tool.hpp"

#include <algorithm>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <set>
#include <vector>

#include "hooks/hash.hpp"
#include "platform/paths.hpp"
#include "platform/text_encoding.hpp"
#include "runtime/id_authority.hpp"
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
        // embedded-lua 走 runtime::EmbeddedLuaRuntime(legacy .lua 归宿),
        // native-library 走 tools::PluginHost(ABI v1/v2);plugin.json 里写
        // 这两种 kind 的 manifest 这里兜底明说,不静默瞎跑。
        return {"插件运行时 " + std::string(RuntimeKindName(manifest_->kind)) +
                    " 不走 process 通道(legacy .lua / 原生库各有各的挂载路)",
                true};
    }

    plugin_protocol::ProcessRequest request;
    request.plugin = manifest_->id;
    request.tool = definition_->name;
    request.entry = definition_->entry;
    request.arguments = input;
    request.context_cwd = cwd_utf8_;
    // call_id:进程级发号局的 req 号(plugins 单第 7 步:call_id 换
    // runtime::ProcessIdAuthority() 的真 id,与 turn/item 的对账走同一本
    // 账,不各处再造第二套)。非敏感串,可读可对账。
    request.call_id = ProcessIdAuthority().NextRequestId();

    ProcessCallLimits limits;
    limits.timeout_ms = manifest_->timeout_ms;
    const auto outcome = RunProcessToolCall(*manifest_, request, cwd_utf8_, cancel_, limits);
    // 日志分流:插件的 stderr 尾巴进 LogSink(终端/事件流各画各的),不进
    // 模型结果;模型只看 BuildResultText 的正文。
    if (log_sink_ && !outcome.stderr_tail.empty()) {
        log_sink_("[plugin " + manifest_->id + "] " +
                  platform::SanitizeExternalText(outcome.stderr_tail.substr(0, 1024)));
    }
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

// ---------------------------------------------------------------------------
// 项目插件:内容指纹 + 信任门(plugins 单第 8 步)
// ---------------------------------------------------------------------------

std::expected<std::string, std::string> ComputePluginContentHash(const std::filesystem::path& plugin_dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(plugin_dir, ec)) {
        return std::unexpected("插件目录不存在: " + platform::PathToUtf8(plugin_dir));
    }
    // 先收全部常规文件的相对路径,排序钉死顺序(枚举次序随文件系统心情,
    // 指纹要跨进程稳定)。
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(plugin_dir, ec)) {
        if (ec) {
            break;
        }
        std::error_code file_ec;
        if (!entry.is_regular_file(file_ec) || file_ec) {
            continue;  // 子目录/坏项:只哈希文件字节
        }
        files.push_back(std::filesystem::relative(entry.path(), plugin_dir, file_ec));
    }
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return platform::PathToUtf8(left) < platform::PathToUtf8(right);
    });

    // 相对路径 + 字节全部喂进同一口锅:改名/改内容/添删文件都变指纹。
    std::string material;
    for (const auto& file : files) {
        material += platform::PathToUtf8(file);
        material += '\0';
        std::ifstream in(plugin_dir / file, std::ios::binary);
        if (!in.is_open()) {
            return std::unexpected("读不到文件: " + platform::PathToUtf8(plugin_dir / file));
        }
        material.append((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        material += '\0';
    }
    return hooks::Sha256Hex(material);
}

PluginScanResult ScanProjectPluginDirectories(const std::filesystem::path& project_dir,
                                              const config::PluginTrustStore* trust) {
    PluginScanResult result;
    std::error_code ec;
    const std::filesystem::path plugins_dir = project_dir / ".lubancode" / "plugins";
    if (!std::filesystem::is_directory(plugins_dir, ec)) {
        return result;  // 项目没配插件是常态
    }
    // 复用主扫描(解析 + 强校验 + 跨插件查重),再叠信任门。
    const PluginScanResult scanned = ScanPluginDirectories(plugins_dir);
    for (const auto& manifest : scanned.manifests) {
        const auto content_hash = ComputePluginContentHash(manifest->plugin_dir);
        if (!content_hash.has_value()) {
            result.warnings.push_back("[plugin] " + manifest->id + ": " + content_hash.error() + ",跳过");
            continue;
        }
        const std::string dir_utf8 = platform::PathToUtf8(manifest->plugin_dir);
        if (trust != nullptr && trust->IsDisabled(dir_utf8, *content_hash)) {
            result.warnings.push_back("[plugin] " + manifest->id + ": 已被禁用(信任账里标了 disable),跳过");
            continue;
        }
        if (trust == nullptr || !trust->IsTrusted(dir_utf8, *content_hash)) {
            result.warnings.push_back("[plugin] " + manifest->id + ": 项目插件未经信任(内容指纹 " +
                                      hooks::DefinitionHashShort(*content_hash) +
                                      "),跳过——项目目录里的插件是外来代码,放进目录就是执行代码;"
                                      "批准后重载才挂(/plugin 的信任流后续批次接 UI,先手改 "
                                      "~/.lubancode/plugin-trust.json)");
            continue;
        }
        result.manifests.push_back(manifest);
    }
    // 解析期警告照传。
    result.warnings.insert(result.warnings.end(), scanned.warnings.begin(), scanned.warnings.end());
    return result;
}

}  // namespace lubancode::runtime
