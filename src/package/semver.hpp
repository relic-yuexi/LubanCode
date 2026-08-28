// Package 域的 SemVer 解析与比较(统一 Package 封装单阶段 1)。根清单的
// version 与 compatibility.lubancode 都按真 SemVer 算,不拿字符串硬比。
// 规矩来自 semver.org 2.0.0:主.次.补丁[-预发布][+构建];构建元数据只作
// 标识,不参与比较;预发布按点分段,数字段比数值,字母段比 ASCII,数字段
// 低于字母段,段多的大,无预发布的大于有预发布的。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lubancode::package {

struct SemVer {
    std::int64_t major = 0;
    std::int64_t minor = 0;
    std::int64_t patch = 0;
    std::string prerelease;  // "alpha.1";空 = 正式版
    std::string build;       // "001";不参与比较,只随原文保留
    std::string text;        // 原文(去掉两端空白后的)

    bool is_prerelease() const { return !prerelease.empty(); }
};

// 解析一个版本号。数字段超 int64、前导零(01 这种 SemVer 不许)、空段、
// 非法字符都算失败。输入先剥两端空白。
std::optional<SemVer> ParseSemVer(std::string_view text);

// 全序比较:负数 = a<b,0 = 相等,正数 = a>b。构建元数据不参与。
int CompareSemVer(const SemVer& a, const SemVer& b);

// ---------------------------------------------------------------------------
// 版本范围:根清单 compatibility.lubancode 的写法,单子上就这一种——
// 空格分隔的比较子列表,全部满足才算满足:
//   ">=0.27.0 <0.28.0"
// 比较子支持 >= > <= < = 五种算符,裸版本号等价 "=x.y.z"。预发布号照
// SemVer 全序参与比较(0.27.0-rc.1 不满足 ">=0.27.0"),这是有意的:
// 兼容门槛不拿候选版冒充正式版。
// ---------------------------------------------------------------------------
struct VersionComparator {
    enum class Op { Ge, Gt, Le, Lt, Eq };
    Op op = Op::Eq;
    SemVer version;
};

struct VersionRange {
    std::string text;  // 原文
    std::vector<VersionComparator> parts;
};

std::optional<VersionRange> ParseVersionRange(std::string_view text);

// 版本是否落在范围内。空范围(无比较子)视为满足——解析层已拦,双保险。
bool VersionSatisfies(const SemVer& version, const VersionRange& range);

}  // namespace lubancode::package
