// workspace_commands.hpp 的实现:工具清单/插件/MCP/LSP/worktree 命令的函数体。
#include "app/commands/workspace_commands.hpp"
#include "app/commands/command_registry.hpp"  // SlashDispatchContext(分派注册制)
#include "cli/todo_render.hpp"                // /todos 的排版
#include "config/project_instructions.hpp"    // /init 的建档
#include "cli/terminal_port.hpp"  // TermOut/TermErr:散打 std::cout 清零,统一走输出端口
#include "cli/format_utils.hpp"   // WrapStatusRows:doctor 状态行的宽度折行(P3-3)

using lubancode::cli::TermOut;
using lubancode::cli::TermErr;

#include <iostream>

#include "cli/console_input.hpp"
#include "tools/background_tasks.hpp"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "app/tool_runtime.hpp"
#include "cli/i18n.hpp"
#include "cli/theme.hpp"
#include "runtime/worktree.hpp"
#include "lsp/manager.hpp"
#include "platform/process.hpp"
#include "platform/text_encoding.hpp"
#include "runtime/plugin_contract.hpp"
#include "runtime/plugin_lua_manifest.hpp"  // v2 doctor:Lua 编译与 handler 对账探针(§10.4)
#include "runtime/plugin_tool.hpp"  // 信任流 UI 的账务动作(TrustProjectPluginById 一族)
#include "runtime/secret_resolver.hpp"  // v2 的 inspect/doctor:Secret 状态探针(§10.3/§10.4)
#include "net/http_transport.hpp"  // v2 doctor:DNS 安全检查的 seam(§10.4)
#include "tools/path_utils.hpp"
#include "tools/registry.hpp"

namespace lubancode::app {


using lubancode::cli::tr;
using lubancode::cli::trf;

// 各带计数。没启用延迟机制(总数没超阈值,或阈值是 0)时说明一句,不摆
// 三态的空架子。
// 统一 Package 封装单阶段 5:带 ToolOrigin 来源账的工具,行尾加
// "(包 <canonical 组件名> <版本>)"——wire 名给人看费眼,canonical 名 +
// 包版本才是对得上的账(契约 packages.md §6.1:展示名用 canonical)。
void PrintToolsCommand(const lubancode::tools::ToolRegistry& registry, const std::set<std::string>& loaded,
                        bool deferral_enabled, int threshold) {
    const auto print_tool_line = [](const lubancode::tools::Tool& tool,
                                    const lubancode::tools::ToolRegistry& reg) {
        const lubancode::tools::ToolRegistration* registration = reg.RegistrationOf(tool.name());
        TermOut() << "  - " << tool.name();
        if (registration != nullptr && registration->package_origin.has_value()) {
            TermOut() << "  (包 " << registration->package_origin->component_id << " "
                      << registration->package_origin->package_version << ")";
        }
        TermOut() << "\n";
    };
    std::vector<const lubancode::tools::Tool*> core;
    std::vector<const lubancode::tools::Tool*> loaded_deferred;
    std::vector<const lubancode::tools::Tool*> pending_deferred;
    for (const auto& tool : registry.All()) {
        if (!tool->deferred()) {
            core.push_back(tool.get());
        } else if (loaded.count(tool->name()) != 0) {
            loaded_deferred.push_back(tool.get());
        } else {
            pending_deferred.push_back(tool.get());
        }
    }
    if (!deferral_enabled) {
        TermOut() << trf("cmd.tools.no_deferral", registry.All().size(),
                          threshold == 0 ? tr("cmd.tools.threshold_zero")
                                          : trf("cmd.tools.below_threshold", threshold))
                   << "\n";
        for (const auto& tool : registry.All()) {
            print_tool_line(*tool, registry);
        }
        return;
    }
    TermOut() << trf("cmd.tools.enabled", threshold) << "\n";
    TermOut() << trf("cmd.tools.core", core.size()) << "\n";
    for (const auto* tool : core) {
        print_tool_line(*tool, registry);
    }
    TermOut() << trf("cmd.tools.loaded", loaded_deferred.size()) << "\n";
    for (const auto* tool : loaded_deferred) {
        print_tool_line(*tool, registry);
    }
    if (loaded_deferred.empty()) {
        TermOut() << tr("cmd.tools.none_loaded") << "\n";
    }
    TermOut() << trf("cmd.tools.pending", pending_deferred.size()) << "\n";
    for (const auto* tool : pending_deferred) {
        print_tool_line(*tool, registry);
    }
}

std::string PathToUtf8(const std::filesystem::path& path);
bool SameFilesystemPath(const std::filesystem::path& left, const std::filesystem::path& right);

// /worktree 的显示层只拿 i18n 键说话。Git 调用和目录状态都在 cli/worktree
// 里，main 只管给交互会话报结果、在脏树删除前收一声确认。
void PrintWorktreeResult(const lubancode::cli::WorktreeResult& result) {
    namespace worktree = lubancode::cli;
    switch (result.code) {
        case worktree::WorktreeResultCode::Created:
            TermOut() << trf("cmd.worktree.created", PathToUtf8(result.path), result.branch) << "\n";
            break;
        case worktree::WorktreeResultCode::Listed:
            TermOut() << tr("cmd.worktree.list_header") << "\n";
            for (const auto& entry : result.entries) {
                const bool current = SameFilesystemPath(entry.path, result.path);
                TermOut() << "  " << (current ? "* " : "- ") << PathToUtf8(entry.path);
                if (!entry.branch.empty()) {
                    TermOut() << " [" << entry.branch << "]";
                } else if (entry.detached) {
                    TermOut() << " " << tr("cmd.worktree.detached");
                }
                if (current) {
                    TermOut() << " " << tr("cmd.worktree.current");
                }
                TermOut() << "\n";
            }
            break;
        case worktree::WorktreeResultCode::Kept:
            TermOut() << trf("cmd.worktree.kept", PathToUtf8(result.path)) << "\n";
            break;
        case worktree::WorktreeResultCode::Removed:
            TermOut() << trf("cmd.worktree.removed", result.branch) << "\n";
            break;
        case worktree::WorktreeResultCode::NeedsRemoveConfirmation:
            TermOut() << trf("cmd.worktree.dirty", PathToUtf8(result.path)) << "\n";
            break;
        case worktree::WorktreeResultCode::NeedsUserConfirmation:
            TermOut() << trf("cmd.worktree.outside_confirm", PathToUtf8(result.path)) << "\n";
            break;
        case worktree::WorktreeResultCode::VerificationFailed:
            TermOut() << trf("cmd.worktree.verify_failed", result.detail) << "\n";
            break;
        case worktree::WorktreeResultCode::NotRepository:
            TermOut() << tr("cmd.worktree.not_repo") << "\n";
            break;
        case worktree::WorktreeResultCode::InvalidArgument:
            TermOut() << tr("cmd.worktree.usage") << "\n";
            break;
        case worktree::WorktreeResultCode::InvalidName:
            TermOut() << tr("cmd.worktree.invalid_name") << "\n";
            break;
        case worktree::WorktreeResultCode::AlreadyActive:
            TermOut() << tr("cmd.worktree.already_active") << "\n";
            break;
        case worktree::WorktreeResultCode::NoActiveWorktree:
            TermOut() << tr("cmd.worktree.no_active") << "\n";
            break;
        case worktree::WorktreeResultCode::GitError:
            TermOut() << trf("cmd.worktree.git_failed", result.detail) << "\n";
            break;
        case worktree::WorktreeResultCode::FilesystemError:
            TermOut() << trf("cmd.worktree.filesystem_failed", result.detail) << "\n";
            break;
    }
}

// 当前工作目录,转成 UTF-8 字符串(拼进系统提示词里给模型看)。
std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}
bool SameFilesystemPath(const std::filesystem::path& left, const std::filesystem::path& right) {
    std::error_code ec;
    return std::filesystem::equivalent(left, right, ec) && !ec;
}

// 打不打,这个函数本身不做 is_console 判断。

// /plugins 命令:列已挂载的插件工具(完整工具名 + runtime)和启动时的加载
// 警告;一个都没有时打印目录约定,顺带说明三路插件各自怎么写(plugins 单
// 第 8 步:不再是"只列 Lua/DLL",process 插件一并入账)。
void PrintPluginsCommand(const std::vector<PluginMountInfo>& mounted, const std::vector<std::string>& warnings) {
    if (mounted.empty() && warnings.empty()) {
        const auto home_dir = lubancode::config::HomeLubancodeDir();
        const std::string dir =
            (home_dir.has_value() ? *home_dir : tr("path.no_home") + "/.lubancode") + "/plugins";
        TermOut() << trf("cmd.plugins.empty", dir) << "\n";
        return;
    }
    if (!mounted.empty()) {
        TermOut() << trf("cmd.plugins.mounted", mounted.size()) << "\n";
        for (const auto& info : mounted) {
            TermOut() << "  - " << info.tool_name << "  (" << info.kind;
            // 阶段 5:packaged 插件带 ToolOrigin 来源账(canonical 名 + 包版本)。
            if (info.package_origin.has_value()) {
                TermOut() << ",包 " << info.package_origin->package_id << " "
                          << info.package_origin->package_version;
            }
            TermOut() << ")\n";
        }
    }
    if (!warnings.empty()) {
        TermOut() << tr("cmd.plugins.warnings") << "\n";
        for (const auto& warning : warnings) {
            TermOut() << "  - " << warning << "\n";
        }
    }
}

// /plugin 子命令(plugins 单第 8 步)的实现。三路插件的账都从调用方递
// 进来:process 走 manifests,native/Lua 走 mounted(完整工具名前缀对
// 得上插件 id)。doctor 只探环境不执行 tool;test(P3-1)真跑插件自带的
// test_runner 自测脚本(发现与执行在 runtime 侧:ResolvePluginSelfTest/
// RunPluginSelfTest),这里只拆参数、打回执——跑的是作者自己的测试,不经
// 模型调用链,与 doctor 探解释器同一待遇,没有确认流。
// trust/untrust(信任流)另收项目根与信任账:账务与概要在 runtime 侧
// (TrustProjectPluginById 一族),这里只递材料、打回执。
void HandlePluginCommand(const std::string& args,
                         const std::vector<PluginMountInfo>& mounted,
                         const std::vector<std::shared_ptr<const lubancode::runtime::PluginManifest>>& manifests,
                         const std::string& project_root_utf8,
                         lubancode::config::PluginTrustStore* project_trust) {
    // 拆子命令与目标 id。
    std::string sub = args;
    std::string rest;
    const std::size_t space = args.find_first_of(" \t");
    if (space != std::string::npos) {
        sub = args.substr(0, space);
        rest = args.substr(space + 1);
    }
    // 去掉 rest 两端空白。
    const auto begin = rest.find_first_not_of(" \t");
    rest = begin == std::string::npos ? std::string() : rest.substr(begin);
    const auto end = rest.find_last_not_of(" \t");
    if (end != std::string::npos) {
        rest = rest.substr(0, end + 1);
    }

    if (sub.empty()) {
        TermOut() << tr("cmd.plugin.usage") << "\n";
        TermOut() << "另有信任流:/plugin trust <id>(批准项目级插件,重启后挂载)| "
                     "/plugin untrust <id>(销账)。\n";
        return;
    }

    // 形态一:/plugin inspect <id>(sub 是子动词,target 在 rest)。
    // 形态二:/plugin <id>(裸 id,视同 inspect)。
    const bool sub_is_verb = sub == "inspect" || sub == "doctor" || sub == "reload" || sub == "enable" ||
                             sub == "disable" || sub == "test" || sub == "trust" || sub == "untrust";
    const std::string target_id = sub_is_verb ? rest : sub;
    const std::string action = sub_is_verb ? sub : std::string("inspect");
    const lubancode::runtime::PluginManifest* manifest = nullptr;
    if (!target_id.empty()) {
        for (const auto& m : manifests) {
            if (m->id == target_id) {
                manifest = m.get();
            }
        }
    }

    if (action == "inspect") {
        if (manifest != nullptr) {
            TermOut() << trf("cmd.plugin.inspect.header", manifest->id, manifest->version,
                             std::string(lubancode::runtime::RuntimeKindName(manifest->kind)),
                             manifest->language.empty() ? std::string("-") : manifest->language)
                      << "\n";
            TermOut() << trf("cmd.plugin.inspect.dir", lubancode::tools::PathToUtf8(manifest->plugin_dir)) << "\n";
            if (manifest->kind == lubancode::runtime::RuntimeKind::Process) {
                std::string argv_text;
                for (const auto& a : manifest->argv) {
                    argv_text += argv_text.empty() ? a : (" " + a);
                }
                TermOut() << trf("cmd.plugin.inspect.argv", argv_text) << "\n";
                TermOut() << trf("cmd.plugin.inspect.timeout", manifest->timeout_ms) << "\n";
                if (!manifest->env_allowlist.empty()) {
                    std::string env_names;
                    for (const auto& name : manifest->env_allowlist) {
                        env_names += env_names.empty() ? name : (", " + name);
                    }
                    TermOut() << trf("cmd.plugin.inspect.env", env_names) << "\n";
                }
            }
            TermOut() << trf("cmd.plugin.inspect.tools", manifest->tools.size()) << "\n";
            for (const auto& tool : manifest->tools) {
                TermOut() << "  - " << tool.full_name << "\n";
            }
            // v2(manifest-backed Lua)的六行权限真账(§10.3:runtime/entry/
            // profile/network/secrets/limits)。只展示声明与状态:Secret 只报
            // 名字与来源类别,不写值、长度、前缀与 fingerprint。
            if (manifest->manifest_version == 2) {
                TermOut() << trf("cmd.plugin.inspect.runtime",
                                 std::string(lubancode::runtime::RuntimeKindName(manifest->kind)))
                          << "\n";
                TermOut() << trf("cmd.plugin.inspect.entry", manifest->runtime_entry) << "\n";
                TermOut() << trf("cmd.plugin.inspect.profile", "pure + host-http") << "\n";
                if (!manifest->network_permissions.empty()) {
                    std::string network_text;
                    for (const auto& permission : manifest->network_permissions) {
                        for (const auto& method : permission.methods) {
                            if (!network_text.empty()) {
                                network_text += ", ";
                            }
                            network_text += method + " " + permission.scheme + "://" + permission.host + ":" +
                                            std::to_string(permission.port);
                        }
                    }
                    TermOut() << trf("cmd.plugin.inspect.network", network_text) << "\n";
                } else {
                    TermOut() << trf("cmd.plugin.inspect.network", "(未声明,禁网)") << "\n";
                }
                if (!manifest->secret_declarations.empty()) {
                    // standalone 插件的数据目录(<home>/.lubancode/plugin-data/
                    // <id>);源码树 .env 永不进这条路(§7.2)。
                    lubancode::runtime::SecretResolverOptions options;
                    options.plugin_data_dir = lubancode::runtime::StandalonePluginDataDir(manifest->id);
                    options.declarations = manifest->secret_declarations;
                    lubancode::runtime::EnvDotEnvSecretResolver resolver(std::move(options));
                    std::string secrets_text;
                    for (const auto& declaration : manifest->secret_declarations) {
                        if (!secrets_text.empty()) {
                            secrets_text += "; ";
                        }
                        secrets_text += resolver.Describe(declaration).Format();
                    }
                    TermOut() << trf("cmd.plugin.inspect.secrets", secrets_text) << "\n";
                }
                const auto limits = lubancode::runtime::ApplyHttpLimits(manifest->http_limits);
                TermOut() << trf("cmd.plugin.inspect.limits",
                                 "request " + std::to_string(limits.request_body_bytes / 1024) + " KiB, response " +
                                     std::to_string(limits.response_body_bytes / 1024) + " KiB, timeout " +
                                     std::to_string(limits.timeout_ms / 1000) + " s")
                          << "\n";
            }
            return;
        }
        // native/Lua 的 inspect:mounted 里按前缀找。
        const std::string prefix = "plugin__" + target_id + "__";
        bool found = false;
        for (const auto& info : mounted) {
            if (info.tool_name.rfind(prefix, 0) == 0) {
                if (!found) {
                    TermOut() << trf("cmd.plugin.inspect.legacy_header", target_id, info.kind) << "\n";
                    found = true;
                }
                TermOut() << "  - " << info.tool_name << "\n";
            }
        }
        if (found) {
            return;
        }
        TermOut() << trf("cmd.plugin.not_found", target_id) << "\n";
        return;
    }

    if (action == "doctor") {
        // doctor:process 查解释器起不起得来;v2 manifest-backed Lua 走只读
        // 探针(编译/对账/DNS/Secret 状态/帽,§10.4);legacy Lua/native 只报
        // 在不在账上。
        if (manifest != nullptr) {
            if (manifest->kind == lubancode::runtime::RuntimeKind::Process) {
                const auto result = lubancode::platform::RunProcess({manifest->argv[0], "--version"}, 15000);
                // 版本串取输出首个非空行、剥首尾空白:--version 的尾巴多半
                // 带换行,原样塞进格式串会把右括号顶到下一行(P3-3 的病根)。
                const auto first_line_trimmed = [](const std::string& output) {
                    std::size_t start = 0;
                    std::size_t end = output.size();
                    while (start < end) {
                        const std::size_t nl = output.find('\n', start);
                        const std::size_t line_end = nl == std::string::npos ? end : nl;
                        std::size_t b = start;
                        std::size_t e = line_end;
                        while (b < e && (output[b] == ' ' || output[b] == '\t' || output[b] == '\r')) {
                            ++b;
                        }
                        while (e > b && (output[e - 1] == ' ' || output[e - 1] == '\t' || output[e - 1] == '\r')) {
                            --e;
                        }
                        if (b < e) {
                            return output.substr(b, e - b);
                        }
                        if (nl == std::string::npos) {
                            break;
                        }
                        start = nl + 1;
                    }
                    return std::string();
                };
                const int wrap_width = lubancode::cli::DetectConsoleWidth().value_or(80);
                if (result.spawn_failed || result.exit_code != 0) {
                    const std::string line =
                        trf("cmd.plugin.doctor.command_bad", manifest->argv[0],
                            result.spawn_failed ? result.spawn_error : std::to_string(result.exit_code));
                    for (const std::string& row : lubancode::cli::WrapStatusRows(line, wrap_width)) {
                        TermOut() << row << "\n";
                    }
                } else {
                    std::string version = first_line_trimmed(result.output);
                    if (version.size() > 80) {
                        version = version.substr(0, 80) + "...";
                    }
                    // 折行口径(P3-3):宽度算 ANSI 与中文宽字,"node(v24.0.0)"
                    // 整段留在本行或整段挪下一行,右括号不独自掉行。
                    const std::string line = trf("cmd.plugin.doctor.command_ok", manifest->argv[0], version);
                    for (const std::string& row : lubancode::cli::WrapStatusRows(line, wrap_width)) {
                        TermOut() << row << "\n";
                    }
                }
            } else {
                // v2(manifest-backed Lua)的 doctor(§10.4):默认只读,不带
                // Secret 发网。清单:Lua 编译与 handler 对账(顶层零副作用
                // 探针)、Pure 画像、网络目的地 DNS 安全检查、Secret 声明
                // (只报名字与来源)、生效帽。真网自测不在此做——doctor 不拿
                // 用户 Key 偷打一枪。
                TermOut() << trf("cmd.plugin.doctor.embedded_lua", manifest->runtime_entry,
                                 lubancode::tools::PathToUtf8(manifest->plugin_dir))
                          << "\n";
                const auto probe = lubancode::runtime::DoctorProbeManifestLua(*manifest);
                if (!probe.has_value()) {
                    TermOut() << "  - Lua 编译与 handler 对账: 通过(顶层零副作用探针,未触发网络与 Secret 解析)\n";
                } else {
                    TermOut() << "  - Lua 编译与 handler 对账: 失败——" << *probe << "\n";
                }
                TermOut() << "  - profile: pure(io/os.execute 关门)+ host-http(仅声明目的地)\n";
                for (const auto& permission : manifest->network_permissions) {
                    // DNS 安全检查(§10.4):只解析不定连接,不带 Secret。
                    lubancode::net::SystemDnsResolver dns;
                    const auto addresses = dns.Resolve(permission.host);
                    if (!addresses.has_value()) {
                        TermOut() << "  - network " << permission.scheme << "://" << permission.host << ":"
                                  << permission.port << " DNS 解析失败(" << addresses.error()
                                  << ";网络不可用时此项无结论,doctor 不发请求)\n";
                        continue;
                    }
                    std::string address_text;
                    bool blocked = false;
                    for (const auto& address : *addresses) {
                        if (!address_text.empty()) {
                            address_text += ", ";
                        }
                        address_text += address;
                        if (const auto range = lubancode::net::BlockedAddressRange(address);
                            range.has_value()) {
                            blocked = true;
                            address_text += "(落禁连段 " + *range + ")";
                        }
                    }
                    TermOut() << "  - network " << permission.scheme << "://" << permission.host << ":"
                              << permission.port << " -> " << address_text
                              << (blocked ? " [禁连段:调用期会被拦]" : "") << "\n";
                }
                if (manifest->network_permissions.empty()) {
                    TermOut() << "  - network: 未声明(luban.http.request 一律 network_not_declared)\n";
                }
                if (!manifest->secret_declarations.empty()) {
                    lubancode::runtime::SecretResolverOptions options;
                    options.plugin_data_dir = lubancode::runtime::StandalonePluginDataDir(manifest->id);
                    options.declarations = manifest->secret_declarations;
                    lubancode::runtime::EnvDotEnvSecretResolver resolver(std::move(options));
                    for (const auto& declaration : manifest->secret_declarations) {
                        TermOut() << "  - " << resolver.Describe(declaration).Format() << "\n";
                    }
                    if (!resolver.dotenv_healthy()) {
                        TermOut() << "  - .env: " << resolver.dotenv_diagnostic() << "\n";
                    }
                }
                const auto limits = lubancode::runtime::ApplyHttpLimits(manifest->http_limits);
                TermOut() << "  - limits: request " << limits.request_body_bytes / 1024 << " KiB, response "
                          << limits.response_body_bytes / 1024 << " KiB, timeout " << limits.timeout_ms / 1000
                          << " s(解析期已核,不越宿主硬帽)\n";
            }
            return;
        }
        for (const auto& info : mounted) {
            const std::string prefix = "plugin__" + target_id + "__";
            if (info.tool_name.rfind(prefix, 0) == 0) {
                TermOut() << trf("cmd.plugin.doctor.legacy_ok", info.kind) << "\n";
                return;
            }
        }
        TermOut() << trf("cmd.plugin.not_found", target_id) << "\n";
        return;
    }

    if (action == "test") {
        // test 的口径(P3-1):找 manifest 声明的自测入口(examples 与 scaffold
        // 的约定:插件目录里同位的 test_runner.py/.js),起进程真跑,交回
        // exit code、耗时、stdout/stderr 摘要;失败按层定位(起不来/超时/
        // 非零退出)。未声明自测入口的明说,不装样子。跑的是插件作者自己
        // 的测试脚本(不经模型调用链),所以没有确认流——同 /plugin doctor
        // 探解释器一个待遇:只读式诊断动作。
        if (target_id.empty()) {
            TermOut() << "用法:/plugin test <id>(id 看 /plugins)\n";
            return;
        }
        if (manifest == nullptr) {
            // legacy Lua/native:分派面在 mounted 里按前缀认;自测约定 v1 只有
            // process 插件有。
            const std::string prefix = "plugin__" + target_id + "__";
            for (const auto& info : mounted) {
                if (info.tool_name.rfind(prefix, 0) == 0) {
                    TermOut() << tr("cmd.plugin.test.legacy_no_entry") << "\n";
                    return;
                }
            }
            TermOut() << trf("cmd.plugin.not_found", target_id) << "\n";
            return;
        }
        const auto plan = lubancode::runtime::ResolvePluginSelfTest(*manifest);
        if (!plan.has_value()) {
            TermOut() << tr("cmd.plugin.test.no_entry") << "\n";
            return;
        }
        std::string argv_text;
        for (const auto& a : plan->argv) {
            argv_text += argv_text.empty() ? a : (" " + a);
        }
        TermOut() << trf("cmd.plugin.test.header", lubancode::tools::PathToUtf8(plan->script), argv_text)
                  << "\n";
        // 自测墙钟:manifest.timeout_ms 是单次工具调用的预算,测试整包可以
        // 比它慢;manifest 没设(0)时给 120s 兜底,设了就照它的来。
        const lubancode::runtime::PluginSelfTestReport report =
            lubancode::runtime::RunPluginSelfTest(*plan, 120000);
        if (report.spawn_failed) {
            TermOut() << trf("cmd.plugin.test.spawn_failed", report.spawn_error, target_id) << "\n";
            return;
        }
        if (report.timed_out) {
            TermOut() << trf("cmd.plugin.test.timed_out",
                             plan->timeout_ms > 0 ? std::to_string(plan->timeout_ms) : std::string("120000"))
                      << "\n";
        } else if (report.exit_code == 0) {
            TermOut() << trf("cmd.plugin.test.ok", report.exit_code, report.elapsed_ms) << "\n";
        } else {
            TermOut() << trf("cmd.plugin.test.failed", report.exit_code, report.elapsed_ms) << "\n";
        }
        if (report.output_truncated) {
            TermOut() << tr("cmd.plugin.test.truncated") << "\n";
        }
        // stdout/stderr 摘要:留末尾 15 行、每路至多 1600 字节(测试输出的
        // 败因总在尾巴上:unittest 的 FAILED 段、node:test 的汇总)。
        const auto print_summary = [](const char* key, const std::string& text) {
            if (text.empty()) {
                return;
            }
            TermOut() << tr(key) << "\n";
            std::vector<std::string> lines;
            std::size_t begin = 0;
            while (begin <= text.size()) {
                const std::size_t nl = text.find('\n', begin);
                const std::size_t end = nl == std::string::npos ? text.size() : nl;
                lines.push_back(text.substr(begin, end - begin));
                if (nl == std::string::npos) {
                    break;
                }
                begin = nl + 1;
            }
            while (!lines.empty() && lines.back().empty()) {
                lines.pop_back();  // 尾部空行不占摘要
            }
            if (lines.size() > 15) {
                TermOut() << "    …(前面还有 " << (lines.size() - 15) << " 行)\n";
                lines.erase(lines.begin(), lines.end() - 15);
            }
            std::size_t shown = 0;
            for (const std::string& line : lines) {
                if (shown + line.size() > 1600) {
                    TermOut() << "    …(字节帽到这儿,后面的截去)\n";
                    break;
                }
                shown += line.size();
                TermOut() << "    " << line << "\n";
            }
        };
        print_summary("cmd.plugin.test.stdout", report.stdout_text);
        print_summary("cmd.plugin.test.stderr", report.stderr_text);
        return;
    }

    if (action == "trust" || action == "untrust") {
        // 信任流 UI(plugins 单第 8 步收口):账务动作在 runtime 侧,这里只
        // 拆参数、打回执。子命令式,不在启动路径加 y/n 问询——管道模式
        // 没法答。
        if (target_id.empty()) {
            TermOut() << "用法:/plugin " << action << " <id>(id 看启动警告或 /plugins)\n";
            return;
        }
        if (project_trust == nullptr || project_root_utf8.empty()) {
            TermOut() << "信任账不可用(找不到用户主目录或会话没有项目根),这条子命令记不了账。\n";
            return;
        }
        const std::filesystem::path project_root = lubancode::tools::Utf8ToPath(project_root_utf8);
        const auto report = action == "trust"
                                ? lubancode::runtime::TrustProjectPluginById(project_root, project_trust, target_id)
                                : lubancode::runtime::UntrustProjectPluginById(project_root, project_trust, target_id);
        if (!report.ok) {
            TermOut() << report.error << "\n";
            return;
        }
        for (const auto& line : report.lines) {
            TermOut() << line << "\n";
        }
        return;
    }

    if (action == "reload") {
        TermOut() << tr("cmd.plugin.reload.hint") << "\n";
        return;
    }
    if (action == "enable" || action == "disable") {
        TermOut() << tr("cmd.plugin.toggle.hint") << "\n";
        return;
    }

    TermOut() << trf("cmd.plugin.unknown_sub", sub) << "\n";
    TermOut() << tr("cmd.plugin.usage") << "\n";
}

// /mcp 命令:每个服务器一行状态(运行中/已退出)+ 工具数,底下缩进列出
// 完整工具名(mcp__服务器名__工具名,跟模型实际看到的名字一致)。
// 统一 Package 封装单阶段 5:packaged MCP 的服务器行换 canonical 名 + 包
// 版本,工具行换带点展示名(wire 名模型看,人看 canonical——契约 §6.1)。
void PrintMcpCommand(const std::vector<McpServerRuntime>& mcp_servers) {
    if (mcp_servers.empty()) {
        TermOut() << tr("cmd.mcp.empty") << "\n";
        return;
    }
    for (const auto& runtime : mcp_servers) {
        const bool alive = runtime.client != nullptr && runtime.client->Alive();
        if (runtime.package_origin.has_value()) {
            TermOut() << trf("cmd.mcp.package_line", runtime.package_origin->component_id,
                             runtime.package_origin->package_version,
                             alive ? tr("mcp.state.alive") : tr("mcp.state.dead"), runtime.tools.size())
                      << "\n";
        } else {
            TermOut() << trf("cmd.mcp.line", runtime.name, alive ? tr("mcp.state.alive") : tr("mcp.state.dead"),
                             runtime.tools.size())
                      << "\n";
        }
        const std::string display_server =
            runtime.package_origin.has_value()
                ? runtime.package_origin->package_id + "." +
                      runtime.package_origin->component_id.substr(
                          runtime.package_origin->component_id.find(':') + 1)
                : runtime.name;
        for (const auto& tool_info : runtime.tools) {
            TermOut() << "      mcp__" << display_server << "__" << tool_info.name << "\n";
        }
    }
}

// /lsp 命令:每个配置了的语言一行状态(未启动/运行中/已闲置关停/已退出)。
// StatusList() 要顺手收割闲置进程(改内部状态),所以入参是可变引用,
// 不装 const。
void PrintLspCommand(std::optional<lubancode::lsp::Manager>& lsp_manager) {
    if (!lsp_manager.has_value()) {
        TermOut() << tr("cmd.lsp.empty") << "\n";
        return;
    }
    const auto statuses = lsp_manager->StatusList();
    TermOut() << trf("cmd.lsp.header", statuses.size()) << "\n";
    for (const auto& status : statuses) {
        TermOut() << "  - " << status.language << " (" << status.command << "): " << status.state << "\n";
    }
}

// ---------------------------------------------------------------------------
// 工作区命令 handler:原样搬自会话主循环的 slash case,行为一字未改。
// ---------------------------------------------------------------------------

CommandFlow HandleWorktreeCommand(WorkspaceCommandState& state, const std::string& args,
                                  const lubancode::cli::Theme& theme) {
    const lubancode::cli::ParsedWorktreeCommand command = lubancode::cli::ParseWorktreeCommand(args);
    lubancode::cli::WorktreeResult result;
    switch (command.action) {
        case lubancode::cli::WorktreeAction::New:
            result = state.worktree.Create(command.name);
            break;
        case lubancode::cli::WorktreeAction::List:
            result = state.worktree.List();
            break;
        case lubancode::cli::WorktreeAction::Exit:
            result = state.worktree.Exit(command.exit_mode);
            break;
        case lubancode::cli::WorktreeAction::Invalid:
            result.code = lubancode::cli::WorktreeResultCode::InvalidArgument;
            break;
    }
    PrintWorktreeResult(result);
    if (result.code == lubancode::cli::WorktreeResultCode::NeedsRemoveConfirmation) {
        const std::optional<std::string> answer = lubancode::cli::ReadLine(
            theme.confirm + tr("cmd.worktree.remove_confirm") + theme.reset, theme,
            /*esc_rejects=*/true);
        if (answer.has_value() && (*answer == "y" || *answer == "Y")) {
            result = state.worktree.ConfirmRemove();
            PrintWorktreeResult(result);
        } else {
            TermOut() << tr("cmd.worktree.remove_cancelled") << "\n";
        }
    }
    // /worktree new 撞上园子外的已有房:跟模型工具同一道硬确认。
    if (result.code == lubancode::cli::WorktreeResultCode::NeedsUserConfirmation) {
        const std::optional<std::string> answer = lubancode::cli::ReadLine(
            theme.confirm + tr("cmd.worktree.outside_prompt") + theme.reset, theme,
            /*esc_rejects=*/true);
        if (answer.has_value() && (*answer == "y" || *answer == "Y")) {
            result = state.worktree.Enter(command.name, /*base=*/"head", /*confirmed_outside=*/true);
            PrintWorktreeResult(result);
        }
    }
    // std::filesystem::current_path 是工具层共同的相对路径基准。同步提示词
    // 和子代理那份 cwd,历史则原样保留。
    if (state.sync_worktree_directory) {
        state.sync_worktree_directory();
    }
    return CommandFlow::Continue;
}

// ---------------------------------------------------------------------------
// 命令分派注册制(会话终章):工作面域的分派位。case 体原样自
// interactive_session 的大 switch 搬来,材料经 SlashDispatchContext 递入。
// (/background 的清单与子命令挪去 background_commands.cpp——管理面单。)
// ---------------------------------------------------------------------------

CommandFlow HandleSlashInit(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    (void)parsed;
    const lubancode::cli::Theme& theme = *ctx.theme;
    const auto result = lubancode::config::InitializeProjectInstructions(std::filesystem::current_path());
    if (result.status == lubancode::config::InitProjectInstructionsStatus::Error) {
        TermOut() << theme.error << trf("cmd.init.failed", PathToUtf8(result.path), result.error)
                  << theme.reset << "\n";
        return CommandFlow::Continue;
    }
    ctx.refresh_project_instructions();
    const char* key = result.status == lubancode::config::InitProjectInstructionsStatus::Created
                          ? "cmd.init.created"
                          : "cmd.init.exists";
    TermOut() << trf(key, PathToUtf8(result.path)) << "\n";
    return CommandFlow::Continue;
}

// /instructions(AGENTS.md 作用域单 P1-1):逐 source 亮账。裸敲看 cwd
// 基线;path <路径> 看目标链(嵌套 AGENTS.md 从仓库根也能查);reload
// 显式重载(与 /init 同一条 refresh 线)后亮新基线。Resolver 用会话那只
// (与写前闸同一份账);没接(旧装配/单测)按 SessionResolverOptions 现
// 起一只,fallback 名单从当前配置来。
CommandFlow HandleSlashInstructions(SlashDispatchContext& ctx,
                                    const lubancode::cli::ParsedSlashCommand& parsed) {
    const lubancode::cli::Theme& theme = *ctx.theme;
    const auto cmd = lubancode::cli::ParseInstructionsCommand(parsed.args);
    if (cmd.action == lubancode::cli::InstructionsCommandAction::Invalid) {
        TermOut() << theme.error
                  << "/instructions: 认不得的子命令" + (cmd.bad_word.empty() ? std::string() : " \"" + cmd.bad_word + "\"") +
                         "。用法: /instructions(看 cwd 基线)| /instructions path <路径>(看目标链)| "
                         "/instructions reload(重载后看新基线)\n"
                  << theme.reset;
        return CommandFlow::Continue;
    }

    // reload:先走 /init 同一条刷新线(重建提示、重新预登记基线),再亮
    // 新账;刷新线没接(单测/旧装配)就只亮当前账。
    if (cmd.action == lubancode::cli::InstructionsCommandAction::Reload) {
        if (ctx.refresh_project_instructions) {
            ctx.refresh_project_instructions();
            TermOut() << "已重载,本会话立即采用。\n";
        } else {
            TermOut() << "(本装配没接刷新线,只显示当前账)\n";
        }
    }

    std::unique_ptr<const lubancode::config::ProjectInstructionResolver> local_resolver;
    const lubancode::config::ProjectInstructionResolver* resolver = ctx.instruction_resolver;
    if (resolver == nullptr) {
        local_resolver = std::make_unique<const lubancode::config::ProjectInstructionResolver>(
            lubancode::config::SessionResolverOptions(
                ctx.config != nullptr ? ctx.config->project_doc_fallback_filenames
                                      : std::vector<std::string>()));
        resolver = local_resolver.get();
    }

    std::filesystem::path target = std::filesystem::current_path();
    if (cmd.action == lubancode::cli::InstructionsCommandAction::Path) {
        target = lubancode::tools::Utf8ToPath(cmd.target);
        if (target.is_relative()) {
            target = std::filesystem::current_path() / target;
        }
    }

    const lubancode::config::InstructionChain chain = resolver->ResolveForPath(target);
    TermOut() << theme.stats << "AGENTS.md 指令链" << theme.reset << "\n";
    for (const std::string& line : lubancode::config::FormatInstructionChainLines(chain, resolver->max_bytes())) {
        TermOut() << line << "\n";
    }
    const std::vector<std::string> diagnostics = lubancode::config::FormatInstructionDiagnosticLines(chain);
    if (!diagnostics.empty()) {
        TermOut() << "诊断:\n";
        for (const std::string& line : diagnostics) {
            TermOut() << line << "\n";
        }
    }
    TermOut().flush();
    return CommandFlow::Continue;
}

CommandFlow HandleSlashWorktree(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    WorkspaceCommandState worktree_state{*ctx.worktree_session, ctx.sync_worktree_directory};
    return HandleWorktreeCommand(worktree_state, parsed.args, *ctx.theme);
}

CommandFlow HandleSlashMcp(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    (void)parsed;
    PrintMcpCommand(*ctx.mcp_servers);
    return CommandFlow::Continue;
}

CommandFlow HandleSlashLsp(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    (void)parsed;
    PrintLspCommand(*ctx.lsp_manager);
    return CommandFlow::Continue;
}

CommandFlow HandleSlashTodos(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    (void)parsed;
    TermOut() << lubancode::cli::FormatTodoList((*ctx.todo_state)->items, *ctx.theme);
    return CommandFlow::Continue;
}

CommandFlow HandleSlashPlugins(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    (void)parsed;
    PrintPluginsCommand(*ctx.plugin_mounted, *ctx.plugin_warnings);
    return CommandFlow::Continue;
}

CommandFlow HandleSlashPlugin(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    HandlePluginCommand(parsed.args, *ctx.plugin_mounted,
                        ctx.tool_runtime != nullptr
                            ? ctx.tool_runtime->process_manifests()
                            : std::vector<std::shared_ptr<const lubancode::runtime::PluginManifest>>{},
                        ctx.tool_runtime != nullptr ? ctx.tool_runtime->project_root_utf8() : std::string(),
                        ctx.tool_runtime != nullptr ? ctx.tool_runtime->project_plugin_trust() : nullptr);
    return CommandFlow::Continue;
}

CommandFlow HandleSlashTools(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    (void)parsed;
    PrintToolsCommand(*ctx.registry, **ctx.loaded_tools, ctx.main_deferral, ctx.tool_search_threshold);
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
