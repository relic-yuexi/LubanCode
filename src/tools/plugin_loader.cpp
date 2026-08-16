#include "tools/plugin_loader.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace lubancode::tools {

namespace {

// std::filesystem::path -> UTF-8 字符串(跟 main.cpp 的 CurrentDirUtf8 同款)。
// 只有 Windows 的 DLL 扫描分支用到;POSIX 下动态库插件未实现(见
// LoadDirectory),maybe_unused 免得 g++ -Wall 报 unused-function。
[[maybe_unused]] std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
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
    out.content = raw.content != nullptr ? std::string(raw.content) : std::string();
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
#ifdef _WIN32
    for (auto& plugin : plugins_) {
        if (plugin.module != nullptr) {
            FreeLibrary(static_cast<HMODULE>(plugin.module));
            plugin.module = nullptr;
        }
    }
#endif
}

std::vector<std::string> PluginHost::LoadDirectory(const std::filesystem::path& dir) {
    std::vector<std::string> warnings;
#ifdef _WIN32
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return warnings;  // 没有 plugins 目录是常态,不算错
    }
    // 先按文件名排序再加载:目录枚举次序随文件系统心情,不钉死的话跨进程
    // /resume 时插件工具的注册次序会换——schema 内容虽一样,请求前缀也对
    // 不上(前缀缓存守恒单第七期)。Lua 插件那条路早就是这么干的。
    std::vector<std::filesystem::path> dll_files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        // 扩展名大小写不敏感(Windows 下 .DLL 也常见)。
        std::string ext = lubancode::tools::PathToUtf8(entry.path().extension());
        for (char& c : ext) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (ext == ".dll") {
            dll_files.push_back(entry.path());
        }
    }
    std::sort(dll_files.begin(), dll_files.end(), [](const auto& left, const auto& right) {
        return lubancode::tools::PathToUtf8(left.filename()) < lubancode::tools::PathToUtf8(right.filename());
    });
    for (const auto& path : dll_files) {
        const std::string file_name = PathToUtf8(path.filename());

        // 坏 DLL(不是合法 PE)默认可能弹系统错误对话框,先把线程错误模式
        // 调成静默,加载完再还原——坏文件只该换来一行警告,不该卡个弹窗。
        DWORD old_error_mode = 0;
        SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &old_error_mode);
        const HMODULE module = LoadLibraryW(path.c_str());
        const DWORD load_error = module == nullptr ? GetLastError() : 0;
        SetThreadErrorMode(old_error_mode, nullptr);
        if (module == nullptr) {
            warnings.push_back("[plugin] " + file_name + ": LoadLibrary 失败(错误码 " +
                               std::to_string(load_error) + "),跳过");
            continue;
        }

        using EntryFn = const luban_plugin_manifest* (*)(void);
        const auto entry_fn =
            reinterpret_cast<EntryFn>(GetProcAddress(module, "luban_plugin_entry"));
        if (entry_fn == nullptr) {
            // 插件常把自己的依赖 DLL 跟主 DLL 放在同一目录。没有入口的库
            // 只是依赖，不算坏插件，静默略过；真坏 PE 仍在 LoadLibrary 处告警。
            FreeLibrary(module);
            continue;
        }

        const luban_plugin_manifest* manifest = entry_fn();
        if (manifest == nullptr) {
            warnings.push_back("[plugin] " + file_name + ": luban_plugin_entry 返回了空指针,跳过");
            FreeLibrary(module);
            continue;
        }
        if (manifest->api_version != LUBAN_PLUGIN_API_VERSION) {
            warnings.push_back("[plugin] " + file_name + ": api_version=" +
                               std::to_string(manifest->api_version) + " 跟宿主(" +
                               std::to_string(LUBAN_PLUGIN_API_VERSION) + ")不合,跳过");
            FreeLibrary(module);
            continue;
        }
        if (manifest->tool_count <= 0 || manifest->tools == nullptr) {
            warnings.push_back("[plugin] " + file_name + ": manifest 里没有工具,跳过");
            FreeLibrary(module);
            continue;
        }

        LoadedPlugin loaded;
        loaded.stem = PathToUtf8(path.stem());
        loaded.path = PathToUtf8(path);
        loaded.module = module;
        loaded.manifest = manifest;
        plugins_.push_back(std::move(loaded));
    }
#else
    (void)dir;  // 非 Windows 平台没实现 DLL 插件,静默空操作
#endif
    return warnings;
}

std::vector<WrappedPlugin> PluginHost::WrapTools(std::vector<std::string>& warnings) const {
    std::vector<WrappedPlugin> out;
    for (const auto& plugin : plugins_) {
        WrappedPlugin wrapped;
        wrapped.stem = plugin.stem;
        for (int i = 0; i < plugin.manifest->tool_count; ++i) {
            const luban_tool_def* def = &plugin.manifest->tools[i];
            if (def->name == nullptr || def->name[0] == '\0' || def->execute == nullptr) {
                warnings.push_back("[plugin] " + plugin.stem + ": 第 " + std::to_string(i + 1) +
                                   " 个工具缺 name 或 execute,跳过这一个");
                continue;
            }
            wrapped.tools.push_back(std::make_unique<PluginTool>(plugin.stem, def));
        }
        if (!wrapped.tools.empty()) {
            out.push_back(std::move(wrapped));
        }
    }
    return out;
}

}  // namespace lubancode::tools
