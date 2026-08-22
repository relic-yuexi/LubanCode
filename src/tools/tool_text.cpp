#include "tools/tool_text.hpp"

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "cli/i18n.hpp"
#include "embedded_tool_text.hpp"  // 构建期生成:<build>/generated/embedded_tool_text.hpp

namespace lubancode::tools {

namespace {

// 全局查表:键 = tool + '\n' + key + '\n' + lang(tool 名、key、语言码都不含
// 换行,拼起来唯一)。装载一次(首次 ToolText 调用),之后只读。值是
// string_view,指向嵌入常量的静态存储,不复制。
struct ToolTextTable {
    std::map<std::string, std::string_view, std::less<>> keys;
    bool loaded = false;
};

ToolTextTable& Table() {
    static ToolTextTable table;
    return table;
}

std::mutex& TableMutex() {
    static std::mutex m;
    return m;
}

void EnsureLoaded() {
    std::lock_guard<std::mutex> lock(TableMutex());
    if (Table().loaded) {
        return;
    }
    for (const auto& entry : embedded_text::kAllKeys) {
        // 同 lang+tool+key 出现两次 = 目录里有撞名文件或撞名节。这里靠
        // emplace 的"先到先得"取第一份,后者静默让位。
        Table().keys.emplace(std::string(entry.tool) + "\n" + entry.key + "\n" + entry.lang, entry.text);
    }
    Table().loaded = true;
}

// 在某一语言里查一条。找不到给 nullopt。
std::optional<std::string_view> LookupInLang(std::string_view lang, std::string_view tool, std::string_view key) {
    static thread_local std::string probe;
    probe.assign(tool);
    probe += '\n';
    probe += key;
    probe += '\n';
    probe += lang;
    const auto it = Table().keys.find(probe);
    if (it == Table().keys.end()) {
        return std::nullopt;
    }
    return it->second;
}

}  // namespace

std::string ToolText(std::string_view tool, std::string_view key, std::string_view fallback) {
    EnsureLoaded();
    const std::string& current = cli::CurrentLanguage();
    if (const auto hit = LookupInLang(current, tool, key)) {
        return std::string(*hit);
    }
    if (current != "zh-CN") {
        if (const auto hit = LookupInLang("zh-CN", tool, key)) {
            return std::string(*hit);
        }
    }
    return std::string(fallback);
}

}  // namespace lubancode::tools
