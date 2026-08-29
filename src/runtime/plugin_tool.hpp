// PluginToolAdapter(plugins 单第 1/2 步的装配层):manifest 里一件工具的
// 中立定义裹成 Tool 挂进 ToolRegistry。
//
// 模型可见性铁律(单子「Schema 的方向不能倒」/「验收」):name()/
// description()/input_schema() 三样之外,language/command/env/path/
// timeout 等宿主元数据一个字节都不进模型侧——这里直接由 Tool 基类的
// 接口形状保证(没有别的出口),单测另有断言钉死。
//
// execute:入参先过 ValidateArgumentsAgainstSchema(manifest 是合同,调用
// 前统一验),再交给选中的 runtime(v1 只有 process;embedded-lua 在第 4
// 批接进来)。needs_confirm 恒真:外部代码,一律先问。
#pragma once

#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "config/plugin_trust.hpp"
#include "runtime/plugin_contract.hpp"
#include "tools/tool.hpp"

namespace lubancode::runtime {

// 插件日志的去处(process 插件的 stderr 尾巴、Lua 的加载警告等)。stdout
// 是模型的结果专线,日志进 sink——Terminal 走 stderr/界面日志,app-server
// 走事件流,Runtime 不自己写 stdout(单子「Runtime 代码边界」)。
using PluginLogSink = std::function<void(const std::string& line)>;

class PluginToolAdapter : public tools::Tool {
public:
    // manifest:整份插件清单(argv/timeout 从这里来);definition 指向
    // manifest.tools 里的一项,manifest 须活得比本对象久(shared_ptr 钉住)。
    PluginToolAdapter(std::shared_ptr<const PluginManifest> manifest, const PluginDefinition* definition);

    std::string name() const override;         // plugin__<id>__<tool>
    std::string description() const override;  // 模型可见说明(不带宿主元数据)
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return true; }   // process 插件默认确认
    bool deferred() const override { return true; }        // tool_search:外挂工具延迟挂载
    tools::Tool::Result execute(const nlohmann::json& input) override;
    // 子代理 x 停止失效单:取消旗随调用递进(context 优先,SetCancel 兜底)
    // ——进程插件的子进程树跟着收。
    tools::Tool::Result execute(const nlohmann::json& input,
                                const tools::ToolExecutionContext& context) override;

    // ESC 取消链(与 PtcTool 同款:每轮由装配层灌指针,不设 = 不取消)。
    void SetCancel(const std::atomic<bool>* cancel) { cancel_ = cancel; }
    // 项目根(进程 cwd 缺省值)。会话启动时设一次。
    void SetCwd(std::string cwd_utf8) { cwd_utf8_ = std::move(cwd_utf8); }
    // 日志去处(不设 = 静默;插件 stderr 本来就主要是诊断)。
    void SetLogSink(PluginLogSink sink) { log_sink_ = std::move(sink); }

    const PluginManifest& manifest() const { return *manifest_; }
    const PluginDefinition& definition() const { return *definition_; }
    const std::string& cwd() const { return cwd_utf8_; }

private:
    // 公共实现:effective_cancel 是本调用真用的取消旗;artifact_dir 是
    // v2 图片块的落盘地(空 = 无落盘地,带回图的调用按 image_rejected 收口)。
    tools::Tool::Result Run(const nlohmann::json& input, const std::atomic<bool>* effective_cancel,
                            const std::string& artifact_dir);

    std::shared_ptr<const PluginManifest> manifest_;
    const PluginDefinition* definition_;
    std::string cwd_utf8_;
    const std::atomic<bool>* cancel_ = nullptr;
    PluginLogSink log_sink_;
};

// 目录扫描:扫 <dir> 下每个子目录的 plugin.json(推荐一插件一目录,单子
// 「Manifest v1」),解析 + 强校验,产出 adapter 列表与警告。
//   - 目录不存在:静默空(插件本就可选)。
//   - 单个插件坏(manifest 解析失败/校验不过/工具重名):一条警告跳过,
//     不连累其余(单子「验收」:错插件只拒目标插件)。
//   - 跨插件重名:由调用方在注册前查重(Register 之前对 full_name 查表,
//     撞了拒后到的,警告点名两方)——工具表绝无两枚同名。
struct PluginScanResult {
    std::vector<std::shared_ptr<const PluginManifest>> manifests;
    std::vector<std::string> warnings;
};

PluginScanResult ScanPluginDirectories(const std::filesystem::path& dir);

// 项目插件的内容指纹:插件目录里全部常规文件(排序稳定)的相对路径 +
// 字节,过 SHA-256。任一文件改了指纹就变,信任失效须重审(单子「零配置
// 与兼容」:首次见到须按 manifest + 文件 hash 信任)。实现是
// platform/dir_fingerprint 的 PluginDirFingerprintV1——统一封装单阶段 4 起
// Package 与 Plugin 同一把尺,材料格式各自冻结。
std::expected<std::string, std::string> ComputePluginContentHash(const std::filesystem::path& plugin_dir);

// 项目级插件扫描(plugins 单第 8 步):扫 <project>/.lubancode/plugins/,
// 逐插件算 content hash,查 PluginTrustStore。未信任/被禁用的跳过并写
// 一条警告(点名怎么批准);信任的照常进 manifests。目录不存在静默空。
// trust 传 nullptr = 全部当未信任处理(测试用)。
PluginScanResult ScanProjectPluginDirectories(const std::filesystem::path& project_dir,
                                              const config::PluginTrustStore* trust);

// ---------------------------------------------------------------------------
// 信任流 UI(/plugin trust|untrust 的材料与账务,plugins 单第 8 步收口)
// ---------------------------------------------------------------------------

// 一枚项目插件的审批材料:manifest 加账本要的两样键料(目录、完整指纹)。
// 警告里的 12 位短指纹只是给人看的引子;真账本键是"完整目录路径 +
// 完整 sha256",全在这份材料里——手改 plugin-trust.json 凑不齐的路不必走。
struct ProjectPluginTrustInfo {
    std::shared_ptr<const PluginManifest> manifest;
    std::string dir_utf8;        // 账本键前半(canonical 后的插件目录)
    std::string content_hash;    // 完整 sha256(64 位十六进制)
    std::size_t file_count = 0;  // 插件目录里的常规文件数(概要亮给用户)
    bool trusted = false;        // 当前指纹是否已在信任账上
    bool disabled = false;       // 信任账里是否标了 disable
};

// 重扫 <project>/.lubancode/plugins/,逐插件算指纹、查账(只读,不改账)。
// 目录不存在 = 空。启动警告里被跳过的插件在这里拿得全材料。
std::vector<ProjectPluginTrustInfo> CollectProjectPluginTrustInfo(
    const std::filesystem::path& project_dir, const config::PluginTrustStore* trust);

// 账务动作的回执:结论逐行带出,命令层只管打印(不在启动路径加 y/n 问询
// ——管道模式没法答)。ok=false 时 error 有话,lines 为空。
struct PluginTrustActionResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> lines;
};

// /plugin trust <id> 的执行侧:亮概要(id、工具清单、文件数、完整指纹)
// 后落 PluginTrustStore(SetTrusted,内部自 Save)。指纹没变的重复批照旧
// 回执,幂等。v1 挂载口径与 reload 一致:重启后生效,不当场热挂。
PluginTrustActionResult TrustProjectPluginById(const std::filesystem::path& project_dir,
                                               config::PluginTrustStore* trust,
                                               const std::string& plugin_id);

// /plugin untrust <id> 的执行侧:按当前指纹从 trusted 销账(disabled 那套
// 既有标记照旧不动)。
PluginTrustActionResult UntrustProjectPluginById(const std::filesystem::path& project_dir,
                                                 config::PluginTrustStore* trust,
                                                 const std::string& plugin_id);

// ---------------------------------------------------------------------------
// /plugin test <id>(P3-1):找 manifest 声明的自测并起进程真跑。
// 自测的"声明"按 examples 与 scaffold 的共同约定:process 插件目录里同位
// 的 test_runner.py / test_runner.js / test_runner.mjs / test_runner.cjs 即
// 自测入口,用 manifest 自己的解释器(runtime.command)起。插件目录里没有
// 这类文件的,回执明说"未声明自测入口",不装样子。embedded-lua/native
// 没有自测约定,一律按未声明处理。
// ---------------------------------------------------------------------------

// 一次自测的执行计划:起哪条命令行、墙钟多少。
struct PluginSelfTestPlan {
    std::filesystem::path script;  // test_runner.* 的全路径(canonical 后的插件目录里)
    std::vector<std::string> argv; // [manifest 的解释器, 脚本 UTF-8 路径]
    int timeout_ms = 0;            // manifest.timeout_ms 原样;0 = 调用方给缺省
};

// 未声明自测入口 = nullopt。纯函数,不起进程。
std::optional<PluginSelfTestPlan> ResolvePluginSelfTest(const PluginManifest& manifest);

// 真跑一回:起进程、等它跑完或超时杀树,交回执行账。stdout/stderr 分开
// 捕获(原始字节;展示层的摘要与裁剪归命令层)。default_timeout_ms 在
// manifest.timeout_ms 为 0(显式不设墙)时兜底。
struct PluginSelfTestReport {
    bool spawn_failed = false;
    std::string spawn_error;
    bool timed_out = false;
    bool output_truncated = false;
    unsigned long exit_code = 0;
    long long elapsed_ms = 0;
    std::string stdout_text;  // 原始字节(多半是 UTF-8;坏编码原样带出)
    std::string stderr_text;
    std::vector<std::string> argv;  // 实际起的命令行(回执亮出来)
};
PluginSelfTestReport RunPluginSelfTest(const PluginSelfTestPlan& plan, int default_timeout_ms);

}  // namespace lubancode::runtime
