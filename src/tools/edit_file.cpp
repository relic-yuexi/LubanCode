#include "tools/edit_file.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "tools/path_utils.hpp"

namespace lubancode::tools {

namespace {

std::size_t CountOccurrences(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

std::string ReplaceAllOccurrences(const std::string& text, const std::string& old_s, const std::string& new_s) {
    std::string result;
    result.reserve(text.size());
    std::size_t pos = 0;
    while (true) {
        const std::size_t found = text.find(old_s, pos);
        if (found == std::string::npos) {
            result.append(text, pos, std::string::npos);
            break;
        }
        result.append(text, pos, found - pos);
        result.append(new_s);
        pos = found + old_s.size();
    }
    return result;
}

}  // namespace

std::string EditFileTool::name() const {
    return "edit_file";
}

std::string EditFileTool::description() const {
    return "对已有文件做精确字符串替换:old_string 必须在文件里逐字唯一出现(除非把 "
           "replace_all 设成 true),找不到或者出现多次都会报错并说明原因(出现了几次)。"
           "适合小范围、精准的改动,不适合整篇重写(整篇重写用 write_file)。执行前需要用户确认。";
}

nlohmann::json EditFileTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();

    nlohmann::json path_prop = nlohmann::json::object();
    path_prop["type"] = "string";
    path_prop["description"] = "要修改的文件路径,相对或绝对均可";
    properties["path"] = path_prop;

    nlohmann::json old_prop = nlohmann::json::object();
    old_prop["type"] = "string";
    old_prop["description"] = "要被替换掉的原文,必须在文件里逐字唯一出现(除非 replace_all=true)";
    properties["old_string"] = old_prop;

    nlohmann::json new_prop = nlohmann::json::object();
    new_prop["type"] = "string";
    new_prop["description"] = "替换成的新内容";
    properties["new_string"] = new_prop;

    nlohmann::json replace_all_prop = nlohmann::json::object();
    replace_all_prop["type"] = "boolean";
    replace_all_prop["description"] = "true 就把所有出现的地方都替换掉,不填默认 false(要求唯一命中)";
    properties["replace_all"] = replace_all_prop;

    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"path", "old_string", "new_string"});

    return schema;
}

Tool::Result EditFileTool::execute(const nlohmann::json& input) {
    if (!input.contains("path") || !input.at("path").is_string()) {
        return {"缺少必填参数 path(字符串)", true};
    }
    if (!input.contains("old_string") || !input.at("old_string").is_string()) {
        return {"缺少必填参数 old_string(字符串)", true};
    }
    if (!input.contains("new_string") || !input.at("new_string").is_string()) {
        return {"缺少必填参数 new_string(字符串)", true};
    }
    const std::string path_str = input.at("path").get<std::string>();
    if (path_str.empty()) {
        return {"path 不能是空字符串", true};
    }
    const std::string old_string = input.at("old_string").get<std::string>();
    const std::string new_string = input.at("new_string").get<std::string>();
    if (old_string.empty()) {
        return {"old_string 不能是空字符串", true};
    }

    bool replace_all = false;
    if (auto it = input.find("replace_all"); it != input.end() && !it->is_null()) {
        replace_all = it->get<bool>();
    }

    const std::filesystem::path path = Utf8ToPath(path_str);

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return {"文件不存在: " + path_str, true};
    }
    if (std::filesystem::is_directory(path, ec)) {
        return {"这是个目录,不是文件: " + path_str, true};
    }

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return {"打不开文件(权限不够或者被占用): " + path_str, true};
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string original = buf.str();
    in.close();

    const std::size_t occurrences = CountOccurrences(original, old_string);
    if (occurrences == 0) {
        return {"文件里找不到 old_string,一次都没匹配上: " + path_str, true};
    }
    if (!replace_all && occurrences > 1) {
        return {"old_string 在文件里出现了 " + std::to_string(occurrences) +
                     " 次,不唯一,没法确定该改哪一处。要么把 old_string 写得更具体(带上前后文),"
                     "要么显式传 replace_all=true 把所有出现的地方都换掉。",
                true};
    }

    std::string updated;
    std::size_t replaced_count = 0;
    if (replace_all) {
        updated = ReplaceAllOccurrences(original, old_string, new_string);
        replaced_count = occurrences;
    } else {
        const std::size_t pos = original.find(old_string);
        updated = original;
        updated.replace(pos, old_string.size(), new_string);
        replaced_count = 1;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return {"打不开文件写(权限不够或者被占用): " + path_str, true};
    }
    out.write(updated.data(), static_cast<std::streamsize>(updated.size()));
    if (!out) {
        return {"写回文件失败: " + path_str, true};
    }
    out.close();

    return {"替换了 " + std::to_string(replaced_count) + " 处: " + path_str, false};
}

}  // namespace lubancode::tools
