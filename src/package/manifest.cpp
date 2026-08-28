// PackageManifest 严格解析的实现(统一 Package 封装单阶段 1)。yaml-cpp 出
// 节点树,这里逐字段验收:类型、必填、未知键、值域。错误一律指到字段路径
// 与行号——"缺必填都报错并指到字段"是单子原话。
#include "package/manifest.hpp"

#include <yaml-cpp/yaml.h>

namespace lubancode::package {

namespace {

// yaml-cpp 的 Mark.line 0 起,人看的行号 1 起;拿不到 mark(合成节点按
// is_null() 约定)给 0。
int LineOf(const YAML::Node& node) {
    const YAML::Mark mark = node.Mark();
    return mark.is_null() ? 0 : static_cast<int>(mark.line) + 1;
}

ManifestError Err(std::string field, int line, std::string detail) {
    return ManifestError{std::move(field), line, std::move(detail)};
}

std::string NodeTypeText(const YAML::Node& node) {
    switch (node.Type()) {
        case YAML::NodeType::Scalar: return "标量";
        case YAML::NodeType::Sequence: return "序列";
        case YAML::NodeType::Map: return "映射";
        case YAML::NodeType::Null: return "空值";
        default: return "未定义";
    }
}

// 标量字符串:节点是标量才收;数字/布尔标量按 yaml 规则可转字符串,但清单
// 字段语义都是文本,别的类型(序列/映射)当场报错。
std::expected<std::string, ManifestError> ScalarString(const YAML::Node& node,
                                                        const std::string& field) {
    if (!node.IsScalar()) {
        return std::unexpected(
            Err(field, LineOf(node), "应为字符串,实际是 " + NodeTypeText(node)));
    }
    return node.Scalar();
}

}  // namespace

std::string ManifestError::Format() const {
    std::string out = "package.yaml";
    if (line > 0) {
        out += ":" + std::to_string(line);
    }
    out += " 字段 ";
    out += field.empty() ? "(根)" : field;
    out += ": ";
    out += detail;
    return out;
}

bool IsValidPackageId(std::string_view id) {
    if (id.empty() || id.size() > 128) return false;
    const auto ok_char = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
    };
    const char front = id.front();
    const char back = id.back();
    if (front == '.' || front == '-' || back == '.' || back == '-') return false;
    for (const char c : id) {
        if (!ok_char(c)) return false;
    }
    return true;
}

std::expected<PackageManifest, ManifestError> ParsePackageManifest(std::string_view yaml_text) {
    YAML::Node root;
    try {
        root = YAML::Load(std::string(yaml_text));
    } catch (const YAML::Exception& e) {
        // 语法层错(yaml-cpp 的 mark 在异常里):指不到字段,行号尽量带上。
        const YAML::Mark mark = e.mark;
        const int line = mark.is_null() ? 0 : static_cast<int>(mark.line) + 1;
        return std::unexpected(Err("(yaml)", line, "YAML 语法错: " + std::string(e.what())));
    }
    if (root.IsNull()) {
        return std::unexpected(Err("(yaml)", 0, "清单是空的"));
    }
    if (!root.IsMap()) {
        return std::unexpected(Err("(yaml)", LineOf(root), "根必须是映射,实际是 " + NodeTypeText(root)));
    }

    // 未知顶层字段:先扫一遍键,认得的才往下走。清单要薄,多写的键多半是
    // 想塞不该塞的账(权限、命令),当场拦。
    static const char* kKnown[] = {"schema", "id", "version", "name", "description", "authors",
                                   "license", "homepage", "repository", "compatibility"};
    for (const auto& kv : root) {
        const std::string key = kv.first.Scalar();
        bool known = false;
        for (const char* candidate : kKnown) {
            if (key == candidate) {
                known = true;
                break;
            }
        }
        if (!known) {
            return std::unexpected(Err(key, LineOf(kv.first),
                                       "未知字段(schema 1 不认;清单只写身份、版本与兼容)"));
        }
    }

    PackageManifest out;

    // ---- schema:必填,只认 1 ----
    const YAML::Node schema = root["schema"];
    if (!schema.IsDefined() || schema.IsNull()) {
        return std::unexpected(Err("schema", LineOf(root), "缺必填字段(只认 1)"));
    }
    if (!schema.IsScalar()) {
        return std::unexpected(Err("schema", LineOf(schema), "应为整数 1,实际是 " + NodeTypeText(schema)));
    }
    const std::string schema_text = schema.Scalar();
    if (schema_text != "1") {
        return std::unexpected(
            Err("schema", LineOf(schema), "只认 1,实际是 \"" + schema_text + "\""));
    }

    // ---- id:必填,字符规矩见 IsValidPackageId ----
    const YAML::Node id = root["id"];
    if (!id.IsDefined() || id.IsNull()) {
        return std::unexpected(Err("id", LineOf(root), "缺必填字段"));
    }
    const auto id_text = ScalarString(id, "id");
    if (!id_text.has_value()) return std::unexpected(id_text.error());
    if (!IsValidPackageId(*id_text)) {
        return std::unexpected(Err("id", LineOf(id),
                                   "只认小写字母、数字、点与连字符,首尾不许点/连字符: \"" + *id_text + "\""));
    }
    out.id = *id_text;

    // ---- version:必填,SemVer ----
    const YAML::Node version = root["version"];
    if (!version.IsDefined() || version.IsNull()) {
        return std::unexpected(Err("version", LineOf(root), "缺必填字段"));
    }
    const auto version_text = ScalarString(version, "version");
    if (!version_text.has_value()) return std::unexpected(version_text.error());
    const auto parsed_version = ParseSemVer(*version_text);
    if (!parsed_version.has_value()) {
        return std::unexpected(Err("version", LineOf(version),
                                   "不是合法 SemVer(MAJOR.MINOR.PATCH): \"" + *version_text + "\""));
    }
    out.version = *parsed_version;

    // ---- name / description:必填非空 ----
    for (const char* field : {"name", "description"}) {
        const YAML::Node node = root[field];
        if (!node.IsDefined() || node.IsNull()) {
            return std::unexpected(Err(field, LineOf(root), "缺必填字段"));
        }
        const auto text = ScalarString(node, field);
        if (!text.has_value()) return std::unexpected(text.error());
        if (text->empty()) {
            return std::unexpected(Err(field, LineOf(node), "不许为空"));
        }
        if (field == std::string("name")) {
            out.name = *text;
        } else {
            out.description = *text;
        }
    }

    // ---- license / homepage / repository:可省标量 ----
    for (const char* field : {"license", "homepage", "repository"}) {
        const YAML::Node node = root[field];
        if (!node.IsDefined() || node.IsNull()) continue;
        const auto text = ScalarString(node, field);
        if (!text.has_value()) return std::unexpected(text.error());
        if (field == std::string("license")) {
            out.license = *text;
        } else if (field == std::string("homepage")) {
            out.homepage = *text;
        } else {
            out.repository = *text;
        }
    }

    // ---- authors:可省,列表 of {name, url} ----
    const YAML::Node authors = root["authors"];
    if (authors.IsDefined() && !authors.IsNull()) {
        if (!authors.IsSequence()) {
            return std::unexpected(
                Err("authors", LineOf(authors), "应为列表,实际是 " + NodeTypeText(authors)));
        }
        for (std::size_t i = 0; i < authors.size(); ++i) {
            const YAML::Node item = authors[i];
            const std::string field = "authors[" + std::to_string(i) + "]";
            if (!item.IsMap()) {
                return std::unexpected(
                    Err(field, LineOf(item), "应为映射 {name, url},实际是 " + NodeTypeText(item)));
            }
            for (const auto& kv : item) {
                const std::string key = kv.first.Scalar();
                if (key != "name" && key != "url") {
                    return std::unexpected(Err(field + "." + key, LineOf(kv.first), "未知字段(只认 name/url)"));
                }
            }
            const YAML::Node author_name = item["name"];
            if (!author_name.IsDefined() || author_name.IsNull()) {
                return std::unexpected(Err(field + ".name", LineOf(item), "缺必填字段"));
            }
            const auto name_text = ScalarString(author_name, field + ".name");
            if (!name_text.has_value()) return std::unexpected(name_text.error());
            if (name_text->empty()) {
                return std::unexpected(Err(field + ".name", LineOf(author_name), "不许为空"));
            }
            PackageAuthor author;
            author.name = *name_text;
            const YAML::Node author_url = item["url"];
            if (author_url.IsDefined() && !author_url.IsNull()) {
                const auto url_text = ScalarString(author_url, field + ".url");
                if (!url_text.has_value()) return std::unexpected(url_text.error());
                author.url = *url_text;
            }
            out.authors.push_back(std::move(author));
        }
    }

    // ---- compatibility:可省 {lubancode, platforms} ----
    const YAML::Node compatibility = root["compatibility"];
    if (compatibility.IsDefined() && !compatibility.IsNull()) {
        if (!compatibility.IsMap()) {
            return std::unexpected(Err("compatibility", LineOf(compatibility),
                                       "应为映射,实际是 " + NodeTypeText(compatibility)));
        }
        for (const auto& kv : compatibility) {
            const std::string key = kv.first.Scalar();
            if (key != "lubancode" && key != "platforms") {
                return std::unexpected(Err("compatibility." + key, LineOf(kv.first),
                                           "未知字段(只认 lubancode/platforms)"));
            }
        }
        const YAML::Node range_node = compatibility["lubancode"];
        if (range_node.IsDefined() && !range_node.IsNull()) {
            const auto range_text = ScalarString(range_node, "compatibility.lubancode");
            if (!range_text.has_value()) return std::unexpected(range_text.error());
            const auto range = ParseVersionRange(*range_text);
            if (!range.has_value()) {
                return std::unexpected(Err("compatibility.lubancode", LineOf(range_node),
                                           "版本范围非法(例 \">=0.27.0 <0.28.0\"): \"" + *range_text + "\""));
            }
            out.compatibility_lubancode = *range;
        }
        const YAML::Node platforms = compatibility["platforms"];
        if (platforms.IsDefined() && !platforms.IsNull()) {
            if (!platforms.IsSequence()) {
                return std::unexpected(Err("compatibility.platforms", LineOf(platforms),
                                           "应为列表,实际是 " + NodeTypeText(platforms)));
            }
            for (std::size_t i = 0; i < platforms.size(); ++i) {
                const YAML::Node item = platforms[i];
                const std::string field = "compatibility.platforms[" + std::to_string(i) + "]";
                const auto text = ScalarString(item, field);
                if (!text.has_value()) return std::unexpected(text.error());
                if (*text != "windows" && *text != "linux" && *text != "macos") {
                    return std::unexpected(
                        Err(field, LineOf(item), "平台只认 windows/linux/macos: \"" + *text + "\""));
                }
                out.compatibility_platforms.push_back(*text);
            }
        }
    }

    return out;
}

}  // namespace lubancode::package
