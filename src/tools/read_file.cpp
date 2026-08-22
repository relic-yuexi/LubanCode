#include "tools/read_file.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

#include "platform/text_encoding.hpp"  // IsValidUtf8:文件编码的明规矩,见 execute 尾部
#include "tools/tool_text.hpp"         // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/

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
    // 文案在 src/prompts/tools/<语言>/read_file.md,兜底是迁移前的原文。
    return ToolText("read_file", "description",
                    "读取文件内容,每行前面带上行号(类似 cat -n)。参数 offset/limit 可以只读文件的一部分;"
                    "limit 省略时默认最多读 2000 行,单次输出至多约 1MB,超出会截断并标注,可以用 offset "
                    "从截断处继续翻页。路径可以是相对路径,也可以是绝对路径。只收 UTF-8 文本(带不带 BOM "
                    "都行,BOM 不会混进正文);二进制文件或不是合法 UTF-8 的文件(比如 GBK 编码)会明确报错,"
                    "请先转存成 UTF-8 再读。");
}

nlohmann::json ReadFileTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();

    nlohmann::json path_prop = nlohmann::json::object();
    path_prop["type"] = "string";
    path_prop["description"] = ToolText("read_file", "param.path", "要读取的文件路径,相对或绝对均可");
    properties["path"] = path_prop;

    nlohmann::json offset_prop = nlohmann::json::object();
    offset_prop["type"] = "integer";
    offset_prop["description"] =
        ToolText("read_file", "param.offset", "从第几行开始读(从 1 计数),不填就从第 1 行开始");
    properties["offset"] = offset_prop;

    nlohmann::json limit_prop = nlohmann::json::object();
    limit_prop["type"] = "integer";
    limit_prop["description"] = ToolText("read_file", "param.limit", "最多读多少行,不填就读到文件末尾");
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
        if (!it->is_number_integer()) {
            return {"offset 得是整数", true};
        }
        offset = it->get<long long>();
        if (offset < 1) {
            offset = 1;
        }
    }
    // 上限兜底:模型不给 limit 就默认最多 2000 行,总字节也封顶——不设的话
    // 一个超大文件(日志、构建产物)一口气全读进上下文,内存和 token 双爆。
    constexpr long long kDefaultLimitLines = 2000;
    constexpr std::size_t kMaxOutputBytes = 1024 * 1024;  // 约 1MB
    long long limit = kDefaultLimitLines;
    if (auto it = input.find("limit"); it != input.end() && !it->is_null()) {
        if (!it->is_number_integer()) {
            return {"limit 得是整数", true};
        }
        limit = it->get<long long>();
        if (limit < 0) {
            limit = kDefaultLimitLines;
        }
    }

    std::ostringstream out;
    std::string line;
    long long line_no = 0;
    long long emitted = 0;
    long long last_emitted_line = 0;
    bool truncated_by_bytes = false;
    bool first_line = true;
    // 编码三查,查的都是"真要进输出"的行(offset/limit 窗口之外的坏字节
    // 不关本工具的事):
    //   has_nul      —— 输出里夹了 NUL 字节,按二进制拒绝;
    //   bad_utf8     —— 有行解不成合法 UTF-8(GBK、残缺多字节序列都会撞上);
    //   first_bad_line —— 第一处坏字节在第几行,报错指个准地方。
    // 合法 UTF-8 的多字节字符不会跨行(\n 是 ASCII,当不了续字节),逐行
    // 校验和整段校验等价,还顺手把定位省了。
    bool has_nul = false;
    bool bad_utf8 = false;
    long long first_bad_line = 0;
    std::size_t out_bytes = 0;
    while (std::getline(file, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();  // Windows 换行(\r\n)留下的尾巴
        }
        if (first_line) {
            first_line = false;
            // UTF-8 BOM:剥掉,不混进正文(行号前缀后面顶个 BOM,首行内容
            // 就被顶歪了)。
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
        }
        if (line_no < offset) {
            continue;
        }
        if (emitted >= limit) {
            break;
        }
        if (!has_nul && line.find('\0') != std::string::npos) {
            has_nul = true;
        }
        if (!bad_utf8 && !platform::IsValidUtf8(line)) {
            bad_utf8 = true;
            first_bad_line = line_no;
        }
        out << std::setw(6) << line_no << "\t" << line << "\n";
        out_bytes += 7 + line.size() + 1;
        ++emitted;
        last_emitted_line = line_no;
        if (out_bytes >= kMaxOutputBytes) {
            truncated_by_bytes = true;
            break;
        }
    }

    if (emitted == 0) {
        if (line_no == 0) {
            return {"(空文件)", false};
        }
        return {"(offset 超过了文件总行数 " + std::to_string(line_no) + ")", false};
    }

    // 编码处置(明规矩,不猜):
    //   - 合法 UTF-8(含剥过 BOM 的)照常返回,逐字节保真;
    //   - 二进制(NUL)拒绝;
    //   - 非法 UTF-8 返回明确工具错误,指明第一个坏字节在哪一行——绝不肯
    //     把坏字节原样塞进 Result.content(那会一路捅崩 JSON 序列化),也不
    //     猜个本机编码悄悄强转,把源码转得面目全非。
    if (has_nul) {
        return {"文件里有 NUL 字节,像是二进制文件,read_file 不读: " + path_str, true};
    }
    if (bad_utf8) {
        return {"文件不是合法 UTF-8(第 " + std::to_string(first_bad_line) +
                    " 行起有坏字节,可能是 GBK 等本机编码): 已拒绝原样读取,免得坏字节带崩会话。"
                    "请先把文件转存成 UTF-8 再读: " + path_str,
                true};
    }

    // 行数/字节任一上限触发,且文件后头确实还有内容,就标注截断并告知翻页办法。
    const bool hit_line_limit = emitted >= limit;
    if (truncated_by_bytes || hit_line_limit) {
        std::string peek;
        if (std::getline(file, peek) || line_no > last_emitted_line) {
            out << "[内容过长已截断,只读到第 " << last_emitted_line << " 行;继续读请用 offset="
                << (last_emitted_line + 1) << "]\n";
        }
    }

    return {out.str(), false};
}

}  // namespace lubancode::tools
