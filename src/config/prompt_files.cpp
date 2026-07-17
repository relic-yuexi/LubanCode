#include "config/prompt_files.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace lubancode::config {

namespace {

namespace fs = std::filesystem;

// UTF-8 字符串 -> fs::path,不走系统 ANSI 代码页那条错路(跟 main.cpp 的
// 插件目录、tools 层各处同一套写法)。
fs::path Utf8Path(const std::string& utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

// fs::path -> UTF-8 字符串,同上,反方向。
std::string PathToUtf8(const fs::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// 整篇写入(binary + trunc)。成功 true。
bool WriteWholeFile(const fs::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    file << content;
    return file.good();
}

}  // namespace

std::string SystemPromptFilePath(const std::string& lubancode_dir) {
    return PathToUtf8(Utf8Path(lubancode_dir) / "system_prompt.md");
}

std::string SoulFilePath(const std::string& lubancode_dir) {
    return PathToUtf8(Utf8Path(lubancode_dir) / "SOUL.md");
}

std::string SoulsDirPath(const std::string& lubancode_dir) {
    return PathToUtf8(Utf8Path(lubancode_dir) / "souls");
}

std::string SoulPathByName(const std::string& lubancode_dir, const std::string& name) {
    return PathToUtf8(Utf8Path(lubancode_dir) / "souls" / Utf8Path(name + ".md"));
}

std::string DefaultSystemPromptFileContent(const std::string& default_persona) {
    return "<!-- 这是 lubancode 的系统提示词\"法\"——改它定制 lubancode 的行为;/prompt reset 一键还原为内置默认。 -->\n" +
           default_persona + "\n";
}

std::string DefaultSoulFileContent() {
    return "<!-- 这是\"魂\"——风格叠加层:在这里写风格指令(比如\"只用文言文答话\"),会注入在系统提示最后;"
           "留空 = 无效果;/soul 可切换 souls/ 目录里的其他魂。 -->\n";
}

std::string DefaultWenyanSoulFileContent() {
    return "<!-- 文言文风格示例:/soul wenyan 启用。 -->\n"
           "答话一律用文言,行文简古,如明清笔记之体。称用户为\"君\"。\n"
           "技术名词、代码、命令、文件路径皆存原文,不译。\n"
           "工具照常调用,义理为先,辞章为后。\n";
}

std::vector<std::string> EnsurePromptScaffold(const std::string& lubancode_dir, const std::string& default_persona) {
    std::vector<std::string> created;
    if (lubancode_dir.empty()) {
        return created;
    }

    const fs::path base = Utf8Path(lubancode_dir);
    const fs::path souls_dir = base / "souls";
    std::error_code ec;
    fs::create_directories(souls_dir, ec);  // 顺手把 base 也建了;失败就整个放弃(下面反正写不进)
    if (ec) {
        return created;
    }

    // 一个文件一条:不存在才写,写成了才记账;写失败不吭声(运行期有回退)。
    const auto ensure_file = [&created](const fs::path& path, const std::string& content) {
        std::error_code exists_ec;
        if (fs::exists(path, exists_ec) || exists_ec) {
            return;
        }
        if (WriteWholeFile(path, content)) {
            created.push_back(PathToUtf8(path));
        }
    };

    ensure_file(base / "system_prompt.md", DefaultSystemPromptFileContent(default_persona));
    ensure_file(base / "SOUL.md", DefaultSoulFileContent());
    ensure_file(souls_dir / "wenyan.md", DefaultWenyanSoulFileContent());
    return created;
}

std::optional<std::string> ReadTextFileIfExists(const std::string& path) {
    const fs::path fs_path = Utf8Path(path);
    std::error_code ec;
    if (!fs::exists(fs_path, ec) || ec || fs::is_directory(fs_path, ec)) {
        return std::nullopt;
    }
    std::ifstream file(fs_path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::expected<std::string, std::string> ResetSystemPromptFile(const std::string& lubancode_dir,
                                                                const std::string& default_persona) {
    if (lubancode_dir.empty()) {
        return std::unexpected(std::string("找不到 .lubancode 目录,没法还原 system_prompt.md"));
    }
    const fs::path base = Utf8Path(lubancode_dir);
    const fs::path prompt_path = base / "system_prompt.md";
    const fs::path bak_path = base / "system_prompt.md.bak";

    std::error_code ec;
    fs::create_directories(base, ec);
    if (ec) {
        return std::unexpected("建目录 " + PathToUtf8(base) + " 失败: " + ec.message());
    }

    std::string bak_result;
    ec.clear();
    if (fs::exists(prompt_path, ec) && !ec) {
        // 旧文件挪成 .bak,已有 .bak 就覆盖(先删再挪;rename 跨不过去时退化
        // 成 copy + remove,跟配置迁移一个路数)。
        std::error_code remove_ec;
        fs::remove(bak_path, remove_ec);  // 不存在也无妨
        ec.clear();
        fs::rename(prompt_path, bak_path, ec);
        if (ec) {
            ec.clear();
            fs::copy_file(prompt_path, bak_path, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                return std::unexpected("备份旧文件到 " + PathToUtf8(bak_path) + " 失败: " + ec.message());
            }
            std::error_code cleanup_ec;
            fs::remove(prompt_path, cleanup_ec);  // 尽力删,删不掉下面 trunc 重写也能盖住
        }
        bak_result = PathToUtf8(bak_path);
    }

    if (!WriteWholeFile(prompt_path, DefaultSystemPromptFileContent(default_persona))) {
        return std::unexpected("重写 " + PathToUtf8(prompt_path) + " 失败(检查一下权限)");
    }
    return bak_result;
}

std::vector<std::string> ListSouls(const std::string& lubancode_dir) {
    std::vector<std::string> names;
    if (lubancode_dir.empty()) {
        return names;
    }
    const fs::path souls_dir = Utf8Path(lubancode_dir) / "souls";
    std::error_code ec;
    if (!fs::exists(souls_dir, ec) || ec || !fs::is_directory(souls_dir, ec)) {
        return names;
    }
    for (const auto& entry : fs::directory_iterator(souls_dir, ec)) {
        if (ec) {
            break;
        }
        std::error_code file_ec;
        if (!entry.is_regular_file(file_ec) || file_ec) {
            continue;
        }
        const fs::path& path = entry.path();
        if (path.extension() != ".md") {
            continue;
        }
        names.push_back(PathToUtf8(path.stem()));
    }
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace lubancode::config
