#include "tools/plugin_loader.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <utility>

#include "platform/dynamic_library.hpp"
#include "platform/paths.hpp"

namespace lubancode::tools {

namespace {

std::string PathToUtf8Local(const std::filesystem::path& path) {
    return lubancode::platform::PathToUtf8(path);
}

// 当前平台认的库扩展名(小写比较):Windows .dll、Linux .so、macOS .dylib。
std::string LibraryExtensionLower(const std::filesystem::path& path) {
    std::string ext = PathToUtf8Local(path.extension());
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

// host allocator 回调:宿主堆的 malloc/free 直通(luban_plugin.h 的
// buffer 契约)。static 函数指针,进程内一份。
void* HostAllocate(std::size_t bytes) { return std::malloc(bytes); }
void HostRelease(void* pointer) { std::free(pointer); }
luban_plugin_host_callbacks HostCallbacks() {
    luban_plugin_host_callbacks callbacks;
    callbacks.allocate = &HostAllocate;
    callbacks.release = &HostRelease;
    return callbacks;
}

}  // namespace

// ---------------------------------------------------------------------------
// PluginTool
// ---------------------------------------------------------------------------

PluginTool::PluginTool(std::string dll_stem, const luban_tool_def* def)
    : dll_stem_(std::move(dll_stem)), def_(def) {
    const std::string raw_name = def_->name != nullptr ? def_->name : "";
    full_name_ = "plugin__" + dll_stem_ + "__" + raw_name;
    description_ =
        "[plugin:" + dll_stem_ + "] " + (def_->description != nullptr ? def_->description : "");

    // schema 构造时解析一次就够(def 是静态数据,不会变)。插件给的不是
    // 合法 JSON,退化成最宽的对象 schema——工具还能用,只是模型没有字段
    // 提示,不值得为这个把整个工具毙掉。
    // (native v1 ABI 的既有宽化行为;native v2 的强校验在第 6 步的
    // loader 里另立,不动这条 legacy 路。)
    schema_ = nlohmann::json{{"type", "object"}};
    if (def_->input_schema_json != nullptr) {
        nlohmann::json parsed =
            nlohmann::json::parse(def_->input_schema_json, /*cb=*/nullptr, /*allow_exceptions=*/false);
        if (!parsed.is_discarded()) {
            schema_ = std::move(parsed);
        }
    }
}

std::string PluginTool::name() const { return full_name_; }

std::string PluginTool::description() const { return description_; }

nlohmann::json PluginTool::input_schema() const { return schema_; }

Tool::Result PluginTool::execute(const nlohmann::json& input) {
    if (def_->execute == nullptr) {
        return {"插件工具 " + full_name_ + " 没有提供 execute 函数", true};
    }
    const std::string input_json = input.dump();
    luban_tool_result raw = def_->execute(input_json.c_str());

    // 先拷进自己的堆,再立刻交还插件释放——content 是插件那边的堆分配的,
    // 这边绝不 free,也绝不留着指针慢慢用。
    Result out;
    out.SetText(raw.content != nullptr ? std::string(raw.content) : std::string());
    out.is_error = raw.is_error != 0;
    if (def_->free_result != nullptr) {
        def_->free_result(&raw);
    }
    return out;
}

// ---------------------------------------------------------------------------
// PluginHost
// ---------------------------------------------------------------------------

PluginHost::~PluginHost() {
    // v2 收尾钩子先走(shutdown 里插件可能还要用自己模块里的代码,必须
    // 在卸载前调);三平台同一把尺:registry 记账里的模块统一交平台层卸。
    // canonical path 幂等记账保证每枚路径只 Open 过一次,这里也只 Close 一次。
    for (auto& plugin : plugins_) {
        if (plugin.shutdown != nullptr) {
            plugin.shutdown();
            plugin.shutdown = nullptr;
        }
    }
    for (auto& plugin : plugins_) {
        if (plugin.module != nullptr) {
            platform::CloseModule(plugin.module);
            plugin.module = nullptr;
        }
    }
}

std::vector<std::string> PluginHost::LoadDirectory(const std::filesystem::path& dir) {
    std::vector<std::string> warnings;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return warnings;  // 没有 plugins 目录是常态,不算错
    }
    // 先按文件名排序再加载:目录枚举次序随文件系统心情,不钉死的话跨进程
    // /resume 时插件工具的注册次序会换——schema 内容虽一样,请求前缀也对
    // 不上(前缀缓存守恒单第七期)。Lua 插件那条路早就是这么干的。
    const std::string wanted_ext = platform::DynamicLibraryExtension();
    std::vector<std::filesystem::path> lib_files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        // 扩展名大小写不敏感(Windows 下 .DLL 也常见;POSIX 惯例小写)。
        if (LibraryExtensionLower(entry.path()) == wanted_ext) {
            lib_files.push_back(entry.path());
        }
    }
    std::sort(lib_files.begin(), lib_files.end(), [](const auto& left, const auto& right) {
        return PathToUtf8Local(left.filename()) < PathToUtf8Local(right.filename());
    });
    for (const auto& path : lib_files) {
        const std::string file_name = PathToUtf8Local(path.filename());

        // 主表与子代理表各装一份 PluginTool,底下却该共用同一枚已加载模块。
        // MountPlugins 会扫两遍同一目录；这里按 canonical path 做幂等
        // (三平台同一条规矩:Linux/macOS 的 dlopen 也有引用计数,重开两
        // 次 close 一次会漏一份引用),免得重加载、plugins_ 重记一份,
        // 继而给子表包出重名工具。
        std::error_code canon_ec;
        const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, canon_ec);
        const std::string canonical_utf8 =
            PathToUtf8Local(canon_ec ? path : canonical);
        if (std::any_of(plugins_.begin(), plugins_.end(),
                        [&canonical_utf8](const LoadedPlugin& loaded) {
                            return loaded.canonical_path == canonical_utf8;
                        })) {
            continue;
        }

        platform::ModuleError module_error;
        const platform::ModuleHandle module = platform::OpenModule(path, module_error);
        if (module == nullptr) {
            warnings.push_back("[plugin] " + file_name + ": " + module_error.message + ",跳过");
            continue;
        }

        // 入口符号两版 ABI 共用(luban_plugin_entry 返回 void*,首 int 判版本)。
        platform::ModuleError symbol_error;
        const auto entry_fn = reinterpret_cast<const void* (*)(void)>(
            platform::FindSymbol(module, "luban_plugin_entry", symbol_error));
        if (entry_fn == nullptr) {
            // 插件常把自己的依赖库跟主库放在同一目录。没有入口的库只是
            // 依赖,不算坏插件,静默略过;真坏库仍在 OpenModule 处告警。
            platform::CloseModule(module);
            continue;
        }

        const void* entry = entry_fn();
        if (entry == nullptr) {
            warnings.push_back("[plugin] " + file_name + ": luban_plugin_entry 返回了空指针,跳过");
            platform::CloseModule(module);
            continue;
        }
        // 版本分派:首字段的值就是 ABI tag。1 = v1 legacy(兼容读取,加载行
        // 明报);2 = v2;别的明说拒,不静默拿错结构体(单子第 6 步)。
        const int abi_tag = *static_cast<const int*>(entry);
        LoadedPlugin loaded;
        loaded.stem = PathToUtf8Local(path.stem());
        loaded.path = PathToUtf8Local(path);
        loaded.canonical_path = canonical_utf8;
        loaded.module = module;
        loaded.abi_tag = abi_tag;
        if (abi_tag == LUBAN_PLUGIN_ABI_V1) {
            const auto* manifest_v1 = static_cast<const luban_plugin_manifest_v1*>(entry);
            if (manifest_v1->tool_count <= 0 || manifest_v1->tools == nullptr) {
                warnings.push_back("[plugin] " + file_name + ": manifest 里没有工具,跳过(legacy ABI v1)");
                platform::CloseModule(module);
                continue;
            }
            loaded.tool_count = manifest_v1->tool_count;
            loaded.tools = manifest_v1->tools;
            loaded.legacy_v1 = true;
        } else if (abi_tag == LUBAN_PLUGIN_ABI_V2) {
            const auto* manifest_v2 = static_cast<const luban_plugin_manifest_v2*>(entry);
            // struct_size 前向兼容:插件写的比宿主认识的小(老插件新宿主)
            // 或大(新插件老宿主)都按"宿主认得的字段"读,这里校验下界:
            // 连宿主已定稿的头一段都盖不满的结构体,版本谈判无从谈起。
            if (manifest_v2->struct_size < static_cast<int>(sizeof(luban_plugin_manifest_v2))) {
                warnings.push_back("[plugin] " + file_name + ": struct_size=" +
                                   std::to_string(manifest_v2->struct_size) + " 比宿主认得的 v2 结构体(" +
                                   std::to_string(static_cast<int>(sizeof(luban_plugin_manifest_v2))) +
                                   ")小,版本协商无从谈起,跳过");
                platform::CloseModule(module);
                continue;
            }
            if (manifest_v2->api_min > LUBAN_PLUGIN_V2_API_MAX ||
                manifest_v2->api_max < LUBAN_PLUGIN_V2_API_MIN) {
                warnings.push_back("[plugin] " + file_name + ": api 范围 [" +
                                   std::to_string(manifest_v2->api_min) + "," +
                                   std::to_string(manifest_v2->api_max) +
                                   "] 跟宿主 [" + std::to_string(LUBAN_PLUGIN_V2_API_MIN) + "," +
                                   std::to_string(LUBAN_PLUGIN_V2_API_MAX) + "] 没有交集,跳过");
                platform::CloseModule(module);
                continue;
            }
            if (manifest_v2->tool_count <= 0 || manifest_v2->tools == nullptr) {
                warnings.push_back("[plugin] " + file_name + ": manifest 里没有工具,跳过(ABI v2)");
                platform::CloseModule(module);
                continue;
            }
            loaded.tool_count = manifest_v2->tool_count;
            loaded.tools = manifest_v2->tools;
            loaded.legacy_v1 = false;
            loaded.plugin_id = manifest_v2->plugin_id != nullptr ? manifest_v2->plugin_id : "";
            loaded.plugin_version = manifest_v2->plugin_version != nullptr ? manifest_v2->plugin_version : "";
            loaded.capability_flags = manifest_v2->capability_flags;
            loaded.shutdown = manifest_v2->shutdown;
            // host allocator 灌回调:在模块句柄记下,首次 execute 前插件就
            // 拿得到(这里直接改的是库里 manifest 的可写副本——abi_tag/flags
            // 是值拷进 LoadedPlugin,回调表是插件结构体里的一块,加载期灌
            // 一次即成)。
            const_cast<luban_plugin_manifest_v2*>(manifest_v2)->host_callbacks = HostCallbacks();
        } else {
            warnings.push_back("[plugin] " + file_name + ": abi_tag=" + std::to_string(abi_tag) +
                               " 宿主不认得(认 1=legacy v1 / 2=v2),跳过——不静默拿错结构体");
            platform::CloseModule(module);
            continue;
        }
        plugins_.push_back(std::move(loaded));
    }
    return warnings;
}

std::vector<WrappedPlugin> PluginHost::WrapTools(std::vector<std::string>& warnings) const {
    std::vector<WrappedPlugin> out;
    for (const auto& plugin : plugins_) {
        WrappedPlugin wrapped;
        // v2 插件自报了 id 就用它当前缀(单子第 6 步:plugin id 进 ABI);
        // v1/没报的照旧拿文件名 stem。legacy 的加载行在 LoadDirectory 的
        // warnings 里已明报,这里不重复。
        wrapped.stem = !plugin.plugin_id.empty() ? plugin.plugin_id : plugin.stem;
        wrapped.legacy_v1 = plugin.legacy_v1;
        for (int i = 0; i < plugin.tool_count; ++i) {
            const luban_tool_def* def = &plugin.tools[i];
            if (def->name == nullptr || def->name[0] == '\0' || def->execute == nullptr) {
                warnings.push_back("[plugin] " + plugin.stem + ": 第 " + std::to_string(i + 1) +
                                   " 个工具缺 name 或 execute,跳过这一个");
                continue;
            }
            wrapped.tools.push_back(std::make_unique<PluginTool>(wrapped.stem, def));
        }
        if (!wrapped.tools.empty()) {
            out.push_back(std::move(wrapped));
        }
    }
    return out;
}

}  // namespace lubancode::tools
