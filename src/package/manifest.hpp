// PackageManifest:根清单 package.yaml(schema 1)的解析结果与严格解析器
// (统一 Package 封装单阶段 1)。清单要薄——只写身份、版本与兼容,不写组件
// 文件清单、权限与密钥(单子 §5.1"清单不写"节)。解析器要严:未知字段、
// 类型错、缺必填、非法值都报错,错误指到字段与行,不许静默吞。
#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "package/semver.hpp"

namespace lubancode::package {

struct PackageAuthor {
    std::string name;  // 必填
    std::string url;   // 可省,空 = 没写
};

struct PackageManifest {
    int schema = 1;
    std::string id;         // "moontide.browser-suite"
    SemVer version;
    std::string name;        // 展示名
    std::string description;  // 展示说明
    // 以下皆可省,只作说明与溯源。
    std::vector<PackageAuthor> authors;
    std::string license;      // 空 = 没写
    std::string homepage;
    std::string repository;
    std::optional<VersionRange> compatibility_lubancode;  // 空 = 没写,写了就严格检查
    std::vector<std::string> compatibility_platforms;     // windows/linux/macos
};

// 一处解析错:指到字段路径与 yaml 行(0 = 拿不到行号)。Format 出一句人话。
struct ManifestError {
    std::string field;  // "version" / "authors[1].url" / "(yaml)"(语法层)
    int line = 0;       // 1 起;0 = 未知
    std::string detail;

    std::string Format() const;
};

// 严格解析一份 package.yaml 文本。合法性门:
//   - 根必须是映射;顶层未知字段报错。
//   - schema 必填,只认 1(整数标量)。
//   - id 必填:小写字母、数字、点与连字符,首尾不许点/连字符。
//   - version 必填,按 SemVer 解析。
//   - name/description 必填非空字符串。
//   - authors 可省:列表,元素是映射{name 必填, url 可省},别的不认。
//   - license/homepage/repository 可省:字符串标量。
//   - compatibility 可省:映射{lubancode: 版本范围, platforms: 平台列表},
//     未知键报错;平台只认 windows/linux/macos。
std::expected<PackageManifest, ManifestError> ParsePackageManifest(std::string_view yaml_text);

// id 的字符规矩(与解析器同一份,盘点/扫描侧复用)。
bool IsValidPackageId(std::string_view id);

}  // namespace lubancode::package
