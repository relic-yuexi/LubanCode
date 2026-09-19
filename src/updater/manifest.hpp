// 更新助手 C++ 化·批一第①单:manifest.json(发行包官方清单)的 C++ 侧
// 读/验/折与规范化输出。与 paths.hpp 同属四方契约的第四方,语义对齐
// scripts 侧三份实现,别单边发明:
//   - schema 1 的字段与验证口径出 generate_manifest.py L140-176
//     (validate_manifest):schema 必须 1、algo 必须 sha256、files 数组、
//     逐条 path 串且过 valid_relpath、size 非负整数、sha256 为 64 位
//     小写十六进制、file_count 与 files 长度合;
//   - 折成 map 的键用 FoldKey(paths.hpp),碰撞判定随 FoldKey 的平台
//     口径——对齐 install_plan.py L190-198(manifest_to_map,读侧)而非
//     generate_manifest.py 写侧的无条件 casefold:Linux 盘面大小写敏感,
//     "A.md" 与 "a.md" 是两个路径,读侧不越权加严;
//   - 读回容错 utf-8-sig:剥开头 BOM(install_plan.py L176-187
//     read_manifest_file 同款;PowerShell 5.1 写文件爱带 BOM);
//   - CanonicalJsonDump 与 python `json.dumps(obj, ensure_ascii=True,
//     sort_keys=True, indent=2) + "\n"` 逐字节等价(install_plan.py
//     L201-203 dump_json / generate_manifest.py L179-180 dump):纯 ASCII
//     输出非 ASCII 一律 \uXXXX 小写转义(0x7F 起,含代理对拆分)、键按
//     字典序、缩进两格、结尾一个换行。nlohmann v3.11 dump(2,' ',true)
//     的转义口径与 python ensure_ascii 实测同源(源码 serializer.hpp
//     dump_escaped)。合同域是整数与字符串(清单/安装账目);浮点表示
//     python 与 nlohmann 有已知差异域,本合同不收浮点。
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::updater {

// 清单里一个官方文件的账(generate_manifest.py 产出的 files 条目三件套:
// 路径/大小/sha256;role 是打包侧标签,C++ 读侧用不上,不收)。
struct ManifestEntry {
    std::string path;       // 原始路径(未折叠,留给人看与落盘)
    std::uint64_t size = 0;
    std::string sha256;     // 64 位小写十六进制
};

// 验过之后的清单:files 已折成 map,键 = FoldKey(path),值留原始 path。
struct Manifest {
    std::map<std::string, ManifestEntry> files;
    std::uint64_t file_count = 0;   // 声明的 file_count(验证时已与 files 数合)
};

// 读/验/折一体(剥 BOM -> 解析 -> validate -> 折 map):
//   - 坏 JSON/缺字段/字段不对/路径不合法/大小写碰撞(按 FoldKey 平台口径),
//     一律返回 nullopt,不合格原因逐条落 *problems(可为 nullptr);
//   - schema 1 / algo sha256 之外一概不认(对齐 generate_manifest.py)。
std::optional<Manifest> ParseManifestText(std::string_view text,
                                          std::vector<std::string>* problems = nullptr);

// 与 python json.dumps(..., ensure_ascii=True, sort_keys=True, indent=2)
// 追加一个换行逐字节等价(口径与合同域见文件头)。输入须是合法 UTF-8
// 的 json 值——含非法 UTF-8 字符串时 nlohmann 抛 type_error,调用方兜。
std::string CanonicalJsonDump(const nlohmann::json& value);

}  // namespace lubancode::updater
