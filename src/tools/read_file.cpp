#include "tools/read_file.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace lubancode::tools {

namespace {

// path 是 UTF-8 编码的字符串;std::filesystem::path 的 char8_t 构造函数
// 明确把输入当 UTF-8 解码,内部再转成 native 编码(Windows 上是 UTF-16)。
// reinterpret_cast<const char8_t*> 在 char/char8_t 之间做字节重解读,
// C++20 里是合法用法(两者都是 1 字节的字符类型)。
// 正斜杠、反斜杠 std::filesystem 在 Windows 下都认得,不用额外转换。
std::filesystem::path Utf8ToPath(const std::string& utf8) {
    const std::u8string_view view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(view);
}

}  // namespace

std::string ReadFileTool::name() const {
    return "read_file";
}

std::string ReadFileTool::description() const {
    return "读取文件内容,每行前面带上行号(类似 cat -n)。参数 offset/limit 可以只读文件的一部分,"
           "省略的话就从头读到尾。路径可以是相对路径,也可以是绝对路径。";
}

nlohmann::json ReadFileTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();

    nlohmann::json path_prop = nlohmann::json::object();
    path_prop["type"] = "string";
    path_prop["description"] = "要读取的文件路径,相对或绝对均可";
    properties["path"] = path_prop;

    nlohmann::json offset_prop = nlohmann::json::object();
    offset_prop["type"] = "integer";
    offset_prop["description"] = "从第几行开始读(从 1 计数),不填就从第 1 行开始";
    properties["offset"] = offset_prop;

    nlohmann::json limit_prop = nlohmann::json::object();
    limit_prop["type"] = "integer";
    limit_prop["description"] = "最多读多少行,不填就读到文件末尾";
    properties["limit"] = limit_prop;

    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"path"});

    return schema;
}

Tool::Result ReadFileTool::execute(const nlohmann::json& input) {
    if (!input.contains("path") || !input.at("path").is_string()) {
        return {"缺少必填参数 path(字符串)", true};
    }
    const std::string path_str = input.at("path").get<std::string>();
    if (path_str.empty()) {
        return {"path 不能是空字符串", true};
    }

    const std::filesystem::path path = Utf8ToPath(path_str);

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return {"文件不存在: " + path_str, true};
    }
    if (std::filesystem::is_directory(path, ec)) {
        return {"这是个目录,不是文件: " + path_str, true};
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return {"打不开文件(权限不够或者被占用): " + path_str, true};
    }

    long long offset = 1;
    if (auto it = input.find("offset"); it != input.end() && !it->is_null()) {
        offset = it->get<long long>();
        if (offset < 1) {
            offset = 1;
        }
    }
    long long limit = -1;  // -1 表示不限制
    if (auto it = input.find("limit"); it != input.end() && !it->is_null()) {
        limit = it->get<long long>();
    }

    std::ostringstream out;
    std::string line;
    long long line_no = 0;
    long long emitted = 0;
    while (std::getline(file, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();  // Windows 换行(\r\n)留下的尾巴
        }
        if (line_no < offset) {
            continue;
        }
        if (limit >= 0 && emitted >= limit) {
            break;
        }
        out << std::setw(6) << line_no << "\t" << line << "\n";
        ++emitted;
    }

    if (emitted == 0) {
        if (line_no == 0) {
            return {"(空文件)", false};
        }
        return {"(offset 超过了文件总行数 " + std::to_string(line_no) + ")", false};
    }

    return {out.str(), false};
}

}  // namespace lubancode::tools
