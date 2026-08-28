// PluginToolAdapter + 目录扫描的实现。
#include "runtime/plugin_tool.hpp"

#include <algorithm>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "agent/model_image_store.hpp"  // DecodeBase64Strict/SniffImageFormat/ReadImageDimensions
#include "hooks/hash.hpp"
#include "mcp/rich_result.hpp"          // kMaxImageBlockBytes/kMaxCallBinaryBytes/LandToolArtifact
#include "platform/paths.hpp"
#include "platform/process.hpp"  // RunProcessWithStdin(/plugin test 的自测进程)
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

// v2(工具结果图片回喂)的 image 块落账:与 MCP 富结果同一条规矩——
// 大小帽(mcp::kMaxImageBlockBytes 同源)、魔数复核(声明与字节对不上
// 即拒)、内容寻址落会话 artifact(mcp::LandToolArtifact,同图去重)。
// 来源两样:base64(协议帧里自带)或插件落好的文件(path,宿主读)。
// 失败给人话,调用方按 ImageRejected 整次收口——不拿半张图冒充成功。
std::expected<tools::ImageContent, std::string> LandResponseImage(
    const plugin_protocol::ResponseImage& source, const std::string& artifact_dir) {
    if (artifact_dir.empty()) {
        return std::unexpected("插件返回了图片,但本次调用没有 artifact 落盘地(未开会话/单发路)");
    }
    std::string bytes;
    if (!source.data_base64.empty()) {
        const auto decoded = agent::DecodeBase64Strict(source.data_base64, mcp::kMaxImageBlockBytes);
        if (!decoded.has_value()) {
            return std::unexpected("图片 base64 解码失败: " + decoded.error());
        }
        bytes = std::move(*decoded);
    } else {
        std::ifstream in(platform::Utf8ToPath(source.path), std::ios::binary);
        if (!in.is_open()) {
            return std::unexpected("图片文件读不到: " + source.path);
        }
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size < 0 || static_cast<std::uint64_t>(size) > mcp::kMaxImageBlockBytes) {
            return std::unexpected("图片文件超字节帽或读不出尺寸: " + source.path);
        }
        in.seekg(0, std::ios::beg);
        bytes.resize(static_cast<std::size_t>(size));
        if (size > 0) {
            in.read(bytes.data(), size);
        }
    }
    if (bytes.empty()) {
        return std::unexpected("图片字节为空");
    }
    // 魔数复核:认不出的格式(空 MIME)或与声明对不上的都是伪 MIME,拒。
    const agent::ImageFormat format = agent::SniffImageFormat(bytes);
    if (format.mime_type.empty() || format.mime_type != source.mime_type) {
        return std::unexpected("图片声明 " + source.mime_type + ",字节魔数认成 " +
                               (format.mime_type.empty() ? std::string("认不出") : format.mime_type));
    }
    const std::string sha = hooks::Sha256Hex(bytes);
    const std::string relative = mcp::LandToolArtifact(artifact_dir, bytes, format.extension);
    if (relative.empty()) {
        return std::unexpected("图片落盘失败: " + artifact_dir);
    }
    const agent::ImageDimensions dims = agent::ReadImageDimensions(bytes, format.mime_type);
    tools::ImageContent image;
    image.mime_type = format.mime_type;
    image.width = dims.width;
    image.height = dims.height;
    image.bytes = bytes.size();
    image.sha256 = sha;
    image.artifact.id = "art-" + sha.substr(0, 8);
    image.artifact.filename = "art-" + sha.substr(0, 8) + "." + format.extension;
    image.artifact.path = relative;
    image.artifact.mime_type = format.mime_type;
    image.artifact.bytes = bytes.size();
    image.artifact.sha256 = sha;
    image.artifact.stored = true;
    return image;
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
    return Run(input, cancel_, std::string());
}

tools::Tool::Result PluginToolAdapter::execute(const nlohmann::json& input,
                                               const tools::ToolExecutionContext& context) {
    // context 的取消旗优先(本次调用那根:主回合 ESC / 子代理 CancelChain
    // 合并旗);没递进来退回 SetCancel 灌的。artifact 目录同路递进(v2
    // 图片块的落盘地)。
    return Run(input, context.cancel != nullptr ? context.cancel : cancel_, context.artifact_dir);
}

tools::Tool::Result PluginToolAdapter::Run(const nlohmann::json& input,
                                           const std::atomic<bool>* effective_cancel,
                                           const std::string& artifact_dir) {
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
    ProcessCallOutcome outcome = RunProcessToolCall(*manifest_, request, cwd_utf8_, effective_cancel, limits);

    // v2(工具结果图片回喂):image 块先落账(帽/魔数/artifact,与 MCP
    // 富结果同一条规矩),落成了再谈成功——坏一块整次按 ImageRejected
    // 收口,不拿半张图冒充。落成的图翻成 ImageContent 进 payload,四家
    // wire 的原生图块路自动接上;投影文本里自带 artifact 路径短句。
    tools::ToolResultPayload rich_payload;
    bool has_rich_images = false;
    if (outcome.code == PluginErrorCode::Ok && !outcome.images.empty()) {
        std::size_t binary_total = 0;
        for (auto& source : outcome.images) {
            auto landed = LandResponseImage(source, artifact_dir);
            if (!landed.has_value()) {
                outcome.code = PluginErrorCode::ImageRejected;
                outcome.detail = landed.error();
                outcome.images.clear();
                break;
            }
            binary_total += landed->bytes;
            if (binary_total > mcp::kMaxCallBinaryBytes) {
                outcome.code = PluginErrorCode::ImageRejected;
                outcome.detail = "图片字节合计超帽(" + std::to_string(binary_total) + "B)";
                outcome.images.clear();
                break;
            }
            rich_payload.content.push_back(std::move(*landed));
            has_rich_images = true;
        }
    }

    // 日志分流:插件的 stderr 尾巴进 LogSink(终端/事件流各画各的),不进
    // 模型结果;模型只看 BuildResultText 的正文。
    if (log_sink_ && !outcome.stderr_tail.empty()) {
        log_sink_("[plugin " + manifest_->id + "] " +
                  platform::SanitizeExternalText(outcome.stderr_tail.substr(0, 1024)));
    }
    // 唯一终态:错误码 + 人话一起交上层(ItemCompleted 一笔)。
    // 逐枚追踪单:process 协议自己的稳定码(超时/取消/坏 JSON/输出超限)
    // 原样透传,trace 里 outcome/error_code 分得开;宿主不伪造插件内部
    // 细节(单子"native ABI 能回的只有 is_error 时,不伪造 exception 细节")。
    Tool::Result result{BuildResultText(outcome), outcome.code != PluginErrorCode::Ok};
    if (outcome.code == PluginErrorCode::TimedOut) {
        result.outcome = "timed_out";
        result.error_code = "plugin.timeout";
    } else if (outcome.code != PluginErrorCode::Ok) {
        result.outcome = "plugin_exception";
        result.error_code = "plugin.process_error";
        if (outcome.code == PluginErrorCode::ImageRejected) {
            result.error_code = "plugin.image_rejected";
        } else if (!outcome.plugin_error_code.empty()) {
            result.error_code = "plugin." + outcome.plugin_error_code;
        }
    } else if (has_rich_images) {
        // 富路:文本块(插件给的正文)在前,图片块按到达序在后;structured
        // (若有且是 object)原样随行。content 投影由 SetPayload 统一重算,
        // 图片以 artifact 短句露面。
        rich_payload.content.insert(rich_payload.content.begin(),
                                    tools::TextContent{outcome.text});
        if (outcome.structured.is_object()) {
            rich_payload.structured_content = outcome.structured;
        }
        result.SetPayload(std::move(rich_payload));
        result.outcome = "succeeded";
    } else {
        result.outcome = "succeeded";
    }
    result.details = nlohmann::json{{"plugin", manifest_->id}, {"tool", definition_->name}};
    if (outcome.exit_code >= 0) {
        result.details["exit_code"] = outcome.exit_code;
    }
    result.effect_summary = "plugin " + manifest_->id + "/" + definition_->name;
    return result;
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

namespace {

// 插件目录里的全部常规文件(相对路径,排序钉死顺序——枚举次序随文件系统
// 心情,指纹与文件数都要跨进程稳定)。指纹材料与概要的文件数共用这一次
// 收账,不各走各的。
std::expected<std::vector<std::filesystem::path>, std::string> CollectPluginFiles(
    const std::filesystem::path& plugin_dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(plugin_dir, ec)) {
        return std::unexpected("插件目录不存在: " + platform::PathToUtf8(plugin_dir));
    }
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
    return files;
}

}  // namespace

std::expected<std::string, std::string> ComputePluginContentHash(const std::filesystem::path& plugin_dir) {
    const auto files = CollectPluginFiles(plugin_dir);
    if (!files.has_value()) {
        return std::unexpected(files.error());
    }
    // 相对路径 + 字节全部喂进同一口锅:改名/改内容/添删文件都变指纹。
    std::string material;
    for (const auto& file : *files) {
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
                                      "批准:/plugin trust " + manifest->id +
                                      "(回执亮工具清单与完整指纹,重启后挂载)");
            continue;
        }
        result.manifests.push_back(manifest);
    }
    // 解析期警告照传。
    result.warnings.insert(result.warnings.end(), scanned.warnings.begin(), scanned.warnings.end());
    return result;
}

// ---------------------------------------------------------------------------
// 信任流 UI(plugins 单第 8 步收口):材料收集 + trust/untrust 的账务动作
// ---------------------------------------------------------------------------

std::vector<ProjectPluginTrustInfo> CollectProjectPluginTrustInfo(
    const std::filesystem::path& project_dir, const config::PluginTrustStore* trust) {
    std::vector<ProjectPluginTrustInfo> out;
    std::error_code ec;
    const std::filesystem::path plugins_dir = project_dir / ".lubancode" / "plugins";
    if (!std::filesystem::is_directory(plugins_dir, ec)) {
        return out;  // 项目没配项目级插件是常态
    }
    // 复用主扫描(解析 + 强校验 + 跨插件查重):manifest 坏的插件进不了这份
    // 材料,trust 侧的"找不到"错误里另案说明。
    for (const auto& manifest : ScanPluginDirectories(plugins_dir).manifests) {
        ProjectPluginTrustInfo info;
        info.manifest = manifest;
        info.dir_utf8 = platform::PathToUtf8(manifest->plugin_dir);
        const auto files = CollectPluginFiles(manifest->plugin_dir);
        const auto content_hash = ComputePluginContentHash(manifest->plugin_dir);
        if (!files.has_value() || !content_hash.has_value()) {
            continue;  // 指纹算不出(目录刚被删/文件读不到):材料不全,不进账
        }
        info.file_count = files->size();
        info.content_hash = *content_hash;
        if (trust != nullptr) {
            info.trusted = trust->IsTrusted(info.dir_utf8, info.content_hash);
            info.disabled = trust->IsDisabled(info.dir_utf8, info.content_hash);
        }
        out.push_back(std::move(info));
    }
    return out;
}

namespace {

// 按 id 找审批材料;找不到给人话(指路 /plugins 与启动警告)。
std::expected<ProjectPluginTrustInfo, std::string> FindTrustCandidate(
    const std::filesystem::path& project_dir, const config::PluginTrustStore* trust,
    const std::string& plugin_id) {
    for (auto& info : CollectProjectPluginTrustInfo(project_dir, trust)) {
        if (info.manifest->id == plugin_id) {
            return info;
        }
    }
    return std::unexpected("项目插件目录(.lubancode/plugins/)里没有 " + plugin_id +
                           ":trust/untrust 只管项目级插件,用户级 ~/.lubancode/plugins/ 不经"
                           "信任门。若启动警告点名过它(manifest 坏/读不到),先修插件本身,"
                           "审批材料出不来就批不了。");
}

// 概要行:id、版本、runtime、目录、工具清单、文件数、完整指纹。用户批的
// 是看得见的东西——指纹全量打出,不拿 12 位短码让人抄。
void AppendTrustSummaryLines(const ProjectPluginTrustInfo& info, std::vector<std::string>& lines) {
    const PluginManifest& manifest = *info.manifest;
    lines.push_back("插件 " + manifest.id + " v" + manifest.version + "(" +
                    std::string(RuntimeKindName(manifest.kind)) + ", " +
                    (manifest.language.empty() ? std::string("-") : manifest.language) + ")");
    lines.push_back("目录: " + info.dir_utf8);
    std::string tools;
    for (const auto& tool : manifest.tools) {
        tools += tools.empty() ? tool.full_name : ("、" + tool.full_name);
    }
    lines.push_back("工具 " + std::to_string(manifest.tools.size()) + " 件:" + tools);
    lines.push_back("文件 " + std::to_string(info.file_count) + " 个,完整内容指纹:");
    lines.push_back("  " + info.content_hash);
}

}  // namespace

PluginTrustActionResult TrustProjectPluginById(const std::filesystem::path& project_dir,
                                               config::PluginTrustStore* trust,
                                               const std::string& plugin_id) {
    PluginTrustActionResult result;
    if (trust == nullptr) {
        result.error = "信任账不可用(找不到用户主目录),记不了账。";
        return result;
    }
    const auto found = FindTrustCandidate(project_dir, trust, plugin_id);
    if (!found.has_value()) {
        result.error = found.error();
        return result;
    }
    result.ok = true;
    AppendTrustSummaryLines(*found, result.lines);
    if (found->trusted) {
        result.lines.push_back("这份指纹已在信任账上,不用重批;重启后挂载。");
        return result;
    }
    trust->SetTrusted(found->dir_utf8, found->content_hash, plugin_id);
    result.lines.push_back("已信任,重启后挂载(插件文件改一个字节指纹就变,须重批)。");
    if (found->disabled) {
        result.lines.push_back("注意:信任账里这份指纹还标着 disable,挂载照旧跳过——先销掉 "
                               "disable 标记再重启。");
    }
    // 同名让位:用户主目录(~/.lubancode/plugins/)有同 id 的 process 插件时,
    // 挂载扫描让主目录优先,这份批了也不挂——如实说,别让用户批完还疑惑。
    if (const auto home = platform::HomeDir(); home.has_value()) {
        const std::filesystem::path home_plugins = platform::Utf8ToPath(*home) / "plugins";
        for (const auto& manifest : ScanPluginDirectories(home_plugins).manifests) {
            if (manifest->id == plugin_id) {
                result.lines.push_back("注意:用户主目录里有同名插件 " + plugin_id +
                                       ",项目级让位——重启后挂载的还是主目录那份。");
                break;
            }
        }
    }
    return result;
}

PluginTrustActionResult UntrustProjectPluginById(const std::filesystem::path& project_dir,
                                                 config::PluginTrustStore* trust,
                                                 const std::string& plugin_id) {
    PluginTrustActionResult result;
    if (trust == nullptr) {
        result.error = "信任账不可用(找不到用户主目录),记不了账。";
        return result;
    }
    const auto found = FindTrustCandidate(project_dir, trust, plugin_id);
    if (!found.has_value()) {
        result.error = found.error();
        return result;
    }
    result.ok = true;
    if (!found->trusted) {
        result.lines.push_back("插件 " + plugin_id + " 的当前指纹本就不在信任账上(可能改过文件已"
                               "失效),没有可销的账。");
        return result;
    }
    trust->Untrust(found->dir_utf8, found->content_hash);
    result.lines.push_back("插件 " + plugin_id + " 已销信任(完整指纹 " + found->content_hash + ")。");
    result.lines.push_back("重启后不再挂载;插件文件未动,要再挂 /plugin trust " + plugin_id +
                           " 重批即可。disable 标记(如有)照旧不动。");
    return result;
}

// ---------------------------------------------------------------------------
// /plugin test <id>(P3-1):自测入口的发现与执行。
// ---------------------------------------------------------------------------

// 自测脚本的同位约定名,按此次序找第一个命中的(scaffold 出 test_runner.py,
// Node 插件按同一形状出 test_runner.js;.mjs/.cjs 顺带认,免得 ESM 插件
// 非得改名)。名字是纯 ASCII,path 直接按窄串拼。
constexpr const char* kSelfTestScriptNames[] = {"test_runner.py", "test_runner.js", "test_runner.mjs",
                                                "test_runner.cjs"};

std::optional<PluginSelfTestPlan> ResolvePluginSelfTest(const PluginManifest& manifest) {
    // 自测约定只在 process 插件(Lua/native 无此说);解析器收进来的 manifest
    // 至少带一枚 argv[0](解释器)。
    if (manifest.kind != RuntimeKind::Process || manifest.argv.empty()) {
        return std::nullopt;
    }
    for (const char* name : kSelfTestScriptNames) {
        std::error_code ec;
        const std::filesystem::path candidate = manifest.plugin_dir / name;
        if (!std::filesystem::is_regular_file(candidate, ec)) {
            continue;
        }
        PluginSelfTestPlan plan;
        plan.script = candidate;
        plan.argv.push_back(manifest.argv[0]);
        plan.argv.push_back(platform::PathToUtf8(candidate));
        plan.timeout_ms = manifest.timeout_ms;
        return plan;
    }
    return std::nullopt;
}

PluginSelfTestReport RunPluginSelfTest(const PluginSelfTestPlan& plan, int default_timeout_ms) {
    PluginSelfTestReport report;
    report.argv = plan.argv;
    const int timeout_ms = plan.timeout_ms > 0 ? plan.timeout_ms : default_timeout_ms;
    const auto started = std::chrono::steady_clock::now();
    // 空 stdin = 立刻关写端:测试脚本本就不该等宿主喂请求帧。stdout/stderr
    // 分开捕获(诊断行多半在 stderr,unittest 的成绩单在 stdout,混作一锅
    // 就分不清"测试输出"与"日志")。cwd 不设:自测脚本按 __file__ 解析路径
    // (scaffold/示例都这个规矩),从哪个 cwd 跑都行。
    const platform::ProcessResult result =
        platform::RunProcessWithStdin(plan.argv, std::string(), timeout_ms);
    report.elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
            .count();
    report.spawn_failed = result.spawn_failed;
    report.spawn_error = result.spawn_error;
    report.timed_out = result.timed_out;
    report.output_truncated = result.output_truncated;
    report.exit_code = result.exit_code;
    report.stdout_text = result.stdout_bytes;
    report.stderr_text = result.stderr_bytes;
    return report;
}

}  // namespace lubancode::runtime
