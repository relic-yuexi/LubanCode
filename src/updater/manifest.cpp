// 实现与合同注释见 manifest.hpp(四方契约,口径出处 generate_manifest.py
// / install_plan.py)。
#include "updater/manifest.hpp"

#include <algorithm>

#include "updater/paths.hpp"

namespace lubancode::updater {
namespace {

constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

// generate_manifest.py L172:sha256 必须是 64 位小写十六进制。
bool IsLowercaseHex64(std::string_view value) {
    if (value.size() != 64) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

void AddProblem(std::vector<std::string>* problems, const std::string& what) {
    if (problems != nullptr) {
        problems->push_back(what);
    }
}

}  // namespace

std::optional<Manifest> ParseManifestText(std::string_view text,
                                          std::vector<std::string>* problems) {
    // utf-8-sig 容错:剥开头 BOM(PowerShell 5.1 写出的清单常带)。
    if (text.starts_with(kUtf8Bom)) {
        text.remove_prefix(kUtf8Bom.size());
    }

    // parse(..., false):不抛异常,坏 JSON 以 discarded 收场(范式同
    // cli/update_command.cpp DigestFromUpdateMeta:nlohmann const json
    // 缺键 operator[] 是 UB,一律 contains()+is_* 先行)。
    const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
    if (parsed.is_discarded()) {
        AddProblem(problems, "清单不是合法 JSON");
        return std::nullopt;
    }
    if (!parsed.is_object()) {
        AddProblem(problems, "清单不是 JSON 对象");
        return std::nullopt;
    }

    bool ok = true;
    const bool schema_is_one = parsed.contains("schema") &&
                               parsed["schema"].is_number_integer() &&
                               (parsed["schema"].is_number_unsigned()
                                    ? parsed["schema"].get<std::uint64_t>() == 1
                                    : parsed["schema"].get<std::int64_t>() == 1);
    if (!schema_is_one) {
        AddProblem(problems, "schema 不认(期望 1)");
        ok = false;
    }
    if (!parsed.contains("algo") || !parsed["algo"].is_string() ||
        parsed["algo"].get<std::string>() != "sha256") {
        AddProblem(problems, "algo 不认(期望 sha256)");
        ok = false;
    }
    if (!parsed.contains("files") || !parsed["files"].is_array()) {
        AddProblem(problems, "files 不是数组");
        return std::nullopt;
    }

    Manifest manifest;
    for (const auto& entry : parsed["files"]) {
        if (!entry.is_object()) {
            AddProblem(problems, "files 成员不是对象");
            ok = false;
            continue;
        }
        if (!entry.contains("path") || !entry["path"].is_string()) {
            AddProblem(problems, "path 不是字符串");
            ok = false;
            continue;
        }
        const std::string path = entry["path"].get<std::string>();
        if (!ValidRelpath(path)) {
            AddProblem(problems, "路径不合法: " + path);
            ok = false;
            continue;
        }
        if (!entry.contains("size") || !entry["size"].is_number_unsigned()) {
            AddProblem(problems, "size 不对: " + path);
            ok = false;
            continue;
        }
        if (!entry.contains("sha256") || !entry["sha256"].is_string() ||
            !IsLowercaseHex64(entry["sha256"].get<std::string>())) {
            AddProblem(problems, "sha256 不是 64 位小写十六进制: " + path);
            ok = false;
            continue;
        }
        ManifestEntry record;
        record.path = path;
        record.size = entry["size"].get<std::uint64_t>();
        record.sha256 = entry["sha256"].get<std::string>();
        // 碰撞判定按 FoldKey 平台口径(install_plan.py manifest_to_map):
        // Windows/macOS 折叠后撞键即拒;Linux 盘面大小写敏感,不算撞。
        const std::string key = FoldKey(path);
        if (!manifest.files.emplace(key, record).second) {
            AddProblem(problems, "大小写碰撞: " + path);
            ok = false;
            continue;
        }
    }

    if (!parsed.contains("file_count") || !parsed["file_count"].is_number_unsigned() ||
        parsed["file_count"].get<std::uint64_t>() != parsed["files"].size()) {
        AddProblem(problems, "file_count 与 files 长度不合");
        ok = false;
    }

    if (!ok) {
        return std::nullopt;
    }
    manifest.file_count = static_cast<std::uint64_t>(manifest.files.size());
    return manifest;
}

std::string CanonicalJsonDump(const nlohmann::json& value) {
    // dump(缩进 2, 空格缩进, ensure_ascii=true):nlohmann v3.11 的转义口径
    // 与 python ensure_ascii 同源(控制字符短转义、>=0x7F 一律 \uXXXX 小写、
    // BMP 外拆代理对);对象键按 std::map 字典序,与 sort_keys=True 一致
    // (UTF-8 字节序即 codepoint 序)。尾换行对齐 python 侧 + "\n"。
    return value.dump(2, ' ', true) + "\n";
}

}  // namespace lubancode::updater
