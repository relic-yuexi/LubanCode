#include "tools/skill_tool.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

#include "platform/sha256.hpp"
#include "tools/path_utils.hpp"
#include "tools/tool_text.hpp"  // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/

namespace lubancode::tools {

namespace {

// §六 受控资源读取的单文件字节帽(与 read_file 的 kMaxOutputBytes 同量级:
// 技能引用材料是给人/模型读的文本,不是搬运通道)。
constexpr std::size_t kMaxResourceBytes = 1024 * 1024;

// path 形如 "<scheme>://" 的外链(http/https/file/ftp 一律算)——作为
// 数据源请求另行处理,skill 工具不自动 fetch 任意地址(§六)。
bool LooksLikeExternalLink(const std::string& path) {
    const std::size_t scheme = path.find("://");
    if (scheme == std::string::npos || scheme == 0) {
        return false;
    }
    for (std::size_t i = 0; i < scheme; ++i) {
        const unsigned char ch = static_cast<unsigned char>(path[i]);
        const bool scheme_char = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                                 (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.';
        if (!scheme_char) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string SkillTool::name() const {
    return "skill";
}

std::string SkillTool::description() const {
    // 文案在 src/prompts/tools/<语言>/skill.md,兜底是迁移前的原文。
    return ToolText("skill", "description",
                    "按名字加载一份已发现的技能(SKILL.md),拿到它的完整使用说明。技能是预先写好的一套具体做法"
                    "(比如某种文体的写作规范、某类任务的固定流程),系统提示里列出的技能名/说明跟当前任务对得上时,"
                    "先调用这个工具把说明读进来,再照着做。");
}

nlohmann::json SkillTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();
    nlohmann::json name_prop = nlohmann::json::object();
    name_prop["type"] = "string";
    name_prop["description"] = ToolText("skill", "param.name", "要加载的技能名,跟系统提示里列出的名字一致");
    if (!skills_.empty()) {
        name_prop["enum"] = nlohmann::json::array();
        for (const auto& skill : skills_) {
            name_prop["enum"].push_back(skill.name);
        }
    }
    properties["name"] = name_prop;

    // §六:技能相对引用的受控读取口。缺省只读 SKILL.md;给了 path 才读
    // 技能目录内的其他材料(只读获准根内,越根/外链明拒)。
    nlohmann::json path_prop = nlohmann::json::object();
    path_prop["type"] = "string";
    path_prop["description"] =
        ToolText("skill", "param.path",
                 "可选。技能目录内的相对路径(如 references/style.md),只读该技能目录内的材料;"
                 "绝对路径、../ 越根、符号链接绕出与 http 外链一律拒绝,外链请作为数据源另行请求");
    properties["path"] = path_prop;

    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"name"});

    return schema;
}

Tool::Result SkillTool::ReadSkillResource(const SkillMeta& meta, const std::string& relative_path) {
    if (relative_path.empty()) {
        return {"path 不能为空:要读技能内材料,给技能目录内的相对路径", true};
    }
    if (LooksLikeExternalLink(relative_path)) {
        return {"外链不自动读取(应用Worker接入单 §六): " + relative_path +
                    " ——外链是数据源请求,另走相应工具/流程,skill 工具只读技能目录内的材料",
                true};
    }
    // 绝对路径/盘符/UNC 一律拒绝:受控读取只认"技能目录内的相对引用"。
    if (relative_path.front() == '/' || relative_path.front() == '\\') {
        return {"拒绝绝对路径(受控读取只认技能目录内的相对路径): " + relative_path, true};
    }
    if (relative_path.size() >= 2 && relative_path[1] == ':' &&
        std::isalpha(static_cast<unsigned char>(relative_path[0])) != 0) {
        return {"拒绝盘符绝对路径(受控读取只认技能目录内的相对路径): " + relative_path, true};
    }
    if (relative_path.rfind("\\\\", 0) == 0) {
        return {"拒绝 UNC 路径(受控读取只认技能目录内的相对路径): " + relative_path, true};
    }

    // 段拆分:/ 与 \ 都当分隔符;空段(双斜杠)、. 与 .. 一律拒——不靠
    // lexically_normal 事后纠,先在源头上把越根写法堵死。段里带 ':' 也拒
    //(Windows 上 "C:" 这类盘符段会顶掉整条前缀路径)。
    std::vector<std::string> segments;
    {
        std::string segment;
        std::istringstream stream(relative_path);
        while (std::getline(stream, segment, '/')) {
            // Windows 侧反斜杠也拆开。
            std::size_t begin = 0;
            while (begin <= segment.size()) {
                const std::size_t back = segment.find('\\', begin);
                const std::string piece = back == std::string::npos ? segment.substr(begin)
                                                                    : segment.substr(begin, back - begin);
                if (piece.empty() || piece == "." || piece == ".." ||
                    piece.find(':') != std::string::npos) {
                    return {"拒绝越根或不可解析的相对路径(只认技能目录内的普通相对路径): " + relative_path,
                            true};
                }
                segments.push_back(piece);
                if (back == std::string::npos) {
                    break;
                }
                begin = back + 1;
            }
        }
        if (segments.empty()) {
            return {"拒绝不可解析的相对路径: " + relative_path, true};
        }
    }

    const std::filesystem::path skill_dir = Utf8ToPath(meta.dir_path);
    std::filesystem::path target = skill_dir;
    for (const std::string& segment : segments) {
        target /= Utf8ToPath(segment);
    }

    // 链接绕过检查:解析真实路径(符号链接在既有前缀上会被展开),解析
    // 结果必须仍落在技能目录的真实路径之内——目录内放链接指向外头,在这
    // 拒。目标不存在时 weakly_canonical 对不存在尾段按字面拼,同样受
    // 越根判定约束。
    std::error_code ec;
    const std::filesystem::path real_dir = std::filesystem::weakly_canonical(skill_dir, ec);
    if (ec) {
        return {"技能目录解析失败: " + meta.dir_path, true};
    }
    const std::filesystem::path real_target = std::filesystem::weakly_canonical(target, ec);
    if (ec) {
        return {"材料路径解析失败: " + relative_path, true};
    }
    const std::string real_dir_utf8 = PathToUtf8(real_dir);
    const std::string real_target_utf8 = PathToUtf8(real_target);
    if (real_target_utf8.rfind(real_dir_utf8, 0) != 0 ||
        (real_target_utf8.size() > real_dir_utf8.size() &&
         real_target_utf8[real_dir_utf8.size()] !=
             static_cast<char>(std::filesystem::path::preferred_separator)) ||
        real_target_utf8.size() <= real_dir_utf8.size()) {
        return {"拒绝越出技能目录的引用(路径规范化与链接检查不过): " + relative_path + " -> " +
                    real_target_utf8,
                true};
    }

    if (!std::filesystem::exists(real_target, ec) || ec) {
        return {"技能内材料不存在: " + relative_path + "(技能目录 " + meta.dir_path + ")", true};
    }
    if (!std::filesystem::is_regular_file(real_target, ec) || ec) {
        return {"技能内引用不是普通文件(目录/设备不读): " + relative_path, true};
    }
    const auto size = std::filesystem::file_size(real_target, ec);
    if (ec) {
        return {"读不了技能内材料的大小: " + relative_path, true};
    }
    if (size > kMaxResourceBytes) {
        return {"技能内材料超过读取上限(1MiB): " + relative_path, true};
    }
    std::ifstream file(real_target, std::ios::binary);
    if (!file.is_open()) {
        return {"技能内材料打不开: " + relative_path, true};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string content = buffer.str();
    return {"技能材料 " + meta.name + "/" + relative_path + ":\n" + content, false};
}

Tool::Result SkillTool::execute(const nlohmann::json& input) {
    if (!input.contains("name") || !input.at("name").is_string()) {
        return {"缺少必填参数 name(字符串)", true};
    }
    const std::string name = input.at("name").get<std::string>();
    std::string relative_path;
    if (input.contains("path") && !input.at("path").is_null()) {
        if (!input.at("path").is_string()) {
            return {"参数 path 须是字符串(技能目录内的相对路径)", true};
        }
        relative_path = input.at("path").get<std::string>();
    }

    const auto it = std::find_if(skills_.begin(), skills_.end(),
                                  [&](const SkillMeta& meta) { return meta.name == name; });
    if (it == skills_.end()) {
        if (skills_.empty()) {
            return {"没有名叫 " + name + " 的技能——当前没有扫描到任何技能。", true};
        }
        std::string available;
        for (const auto& meta : skills_) {
            if (!available.empty()) {
                available += "、";
            }
            available += meta.name;
        }
        return {"没有名叫 " + name + " 的技能,可用的有: " + available, true};
    }

    // §六 依赖声明消费:requires-tools 只作声明,声明的工具不在本场冻结
    // 面上,加载即回 capability_unavailable——不为满足技能文字自动挂
    // shell、装包或提权(面上没有就是没有,如实报)。
    if (available_tools_.has_value()) {
        for (const std::string& required : it->requires_tools) {
            if (available_tools_->count(required) == 0) {
                std::string message = "capability_unavailable: 技能 " + name + " 声明依赖工具 " + required +
                                      ",本场工具面上没有这枚(依赖声明不自动授予执行工具;获准后再来)";
                Tool::Result unavailable{std::move(message), true};
                unavailable.error_code = "capability_unavailable";
                unavailable.outcome = "unavailable";
                return unavailable;
            }
        }
    }

    // 受控资源读取(§六:path 给了就读技能目录内的相对材料,不碰正文)。
    if (!relative_path.empty()) {
        return ReadSkillResource(*it, relative_path);
    }

    const std::filesystem::path skill_md = Utf8ToPath(it->dir_path) / "SKILL.md";
    std::ifstream file(skill_md, std::ios::binary);
    if (!file.is_open()) {
        return {"技能目录还在,但 SKILL.md 读不到了: " + it->dir_path, true};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string content = buffer.str();

    // 漂移校验(§六):扫描时刻的指纹 vs 现在。对不上 = 同场中途改过文件,
    // 拒读——清单(进提示)与正文必须同源,新会话才会用新材料。
    if (!it->content_hash.empty()) {
        const std::string current_hash = platform::Sha256Hex(content);
        if (current_hash != it->content_hash) {
            Tool::Result drifted{"技能材料在会话中途被改动,拒绝读取(漂移): " + name +
                                     "(启动时指纹 " + it->content_hash.substr(0, 12) + "…,现在 " +
                                     current_hash.substr(0, 12) + "…;新会话才会采用新材料)",
                                 true};
            drifted.error_code = "skill.drifted";
            drifted.outcome = "unavailable";
            return drifted;
        }
    }

    const auto parsed = ParseSkillMarkdown(content);
    const std::string body = parsed.has_value() ? parsed->body : content;

    const std::string result = "技能目录: " + it->dir_path + "(技能内相对路径以此为基准)\n" + body;
    return {result, false};
}

}  // namespace lubancode::tools
