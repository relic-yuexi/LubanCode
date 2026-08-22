#include "tools/write_file.hpp"

#include <filesystem>
#include <fstream>
#include <system_error>

#include "tools/isolation.hpp"
#include "tools/path_utils.hpp"
#include "tools/tool_text.hpp"  // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/

namespace lubancode::tools {

std::string WriteFileTool::name() const {
    return "write_file";
}

std::string WriteFileTool::description() const {
    // 文案在 src/prompts/tools/<语言>/write_file.md,兜底是迁移前的原文。
    return ToolText("write_file", "description",
                    "把内容写入文件(UTF-8 编码)。文件已存在就整个覆盖,父目录不存在会自动建好。"
                    "路径可以是相对路径,也可以是绝对路径。适合新建文件或整篇重写;小范围改动用 "
                    "edit_file 更精准。执行前需要用户确认。");
}

nlohmann::json WriteFileTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();

    nlohmann::json path_prop = nlohmann::json::object();
    path_prop["type"] = "string";
    path_prop["description"] = ToolText("write_file", "param.path", "要写入的文件路径,相对或绝对均可");
    properties["path"] = path_prop;

    nlohmann::json content_prop = nlohmann::json::object();
    content_prop["type"] = "string";
    content_prop["description"] =
        ToolText("write_file", "param.content", "要写入的文件内容(UTF-8),会整体覆盖原文件");
    properties["content"] = content_prop;

    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"path", "content"});

    return schema;
}

Tool::Result WriteFileTool::execute(const nlohmann::json& input) {
    if (!input.contains("path") || !input.at("path").is_string()) {
        return {"缺少必填参数 path(字符串)", true};
    }
    if (!input.contains("content") || !input.at("content").is_string()) {
        return {"缺少必填参数 content(字符串)", true};
    }
    const std::string path_str = input.at("path").get<std::string>();
    if (path_str.empty()) {
        return {"path 不能是空字符串", true};
    }
    const std::string content = input.at("content").get<std::string>();

    // 隔离文件闸:住在 worktree 房里的会话/子代理,写主 checkout 的文件一律拦。
    if (const IsolationScope* scope = IsolationGuard::Current();
        scope != nullptr && PathBlockedByIsolation(path_str, *scope)) {
        return {"[隔离] 会话正住在 worktree " + scope->name + " 里,不许写主 checkout 的文件: " + path_str +
                    "。请在房内操作,或先 worktree exit 出房。",
                true};
    }

    const std::filesystem::path path = Utf8ToPath(path_str);

    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        return {"这是个目录,不能当文件写: " + path_str, true};
    }
    const bool existed_before = std::filesystem::exists(path, ec);

    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return {"建父目录失败: " + ec.message(), true};
        }
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return {"打不开文件写(权限不够或者路径不对): " + path_str, true};
    }
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!file) {
        return {"写文件失败: " + path_str, true};
    }
    file.close();

    std::string message = "写入成功,共 " + std::to_string(content.size()) + " 字节: " + path_str;
    if (existed_before) {
        message += "(覆盖了原有文件)";
    }
    return {message, false};
}

}  // namespace lubancode::tools
