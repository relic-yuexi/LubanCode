// hook 包目录发现与装载(LuaHook 单 P0-B,§三):只读清单,脚本正文
// 读进内存交 MiddlewarePool(编译/物化在 Publish;目录发现不做执行)。
//
// 目录形状(§三建议,首批冻结):
//   <root>/my-hook/hook.json    清单(schemaVersion 1;定义元数据集中于此)
//   <root>/my-hook/main.lua     入口(entry 字段指向它)
//
// 规矩:
//   - 相对 entry 以包根解析;越界(../ 逃出包根)、绝对路径、缺件一律拒绝
//     该包——不依赖当前工作目录恰好在哪;
//   - 一份清单解析失败整包拒绝(不带半个定义进池),别的包照装;
//   - 来源层级由调用方按发现位置给(清单不自报 source);
//   - 脚本顶层只构造函数与常量——这是作者合同,装载侧不执行脚本,首次
//     物化在 Publish 的 lua_factory。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "hooks/middleware.hpp"

namespace lubancode::hooks::middleware {

struct HookPackageLoadReport {
    int packages_loaded = 0;         // 成功入池的包数
    std::vector<std::string> errors; // 每包一条失败说明(包名 + 稳定原因)
};

// 扫 <hooks_root> 下的一级子目录:有 hook.json 的算一个包。root 不存在 =
// 零包零错(没配 hooks 的常态)。同层重复键由 Publish 裁决(同层冲突拒绝)。
HookPackageLoadReport LoadHookPackages(MiddlewarePool& pool, const std::filesystem::path& hooks_root,
                                       SourceLayer layer, const std::string& source_label);

// 单包装载(目录发现逐包调;测试直接造单包用)。package_dir 须含
// hook.json;entry 的路径越界/读不动整包拒绝。
std::expected<void, std::string> LoadHookPackage(MiddlewarePool& pool,
                                                 const std::filesystem::path& package_dir,
                                                 SourceLayer layer, const std::string& source_label);

}  // namespace lubancode::hooks::middleware
