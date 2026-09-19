// 更新助手 C++ 化·批一第①单:安装路径契约的 C++ 侧唯一实现。
//
// 这套路径规则是四方契约——generate_manifest.py(打包记账)/
// install_plan.py(POSIX 安装决策)/ install.ps1(Windows 安装)/ 此处(C++
// 更新器)。语义逐条对齐 scripts/install_plan.py,别单边发明:
//   - valid_relpath  口径出 install_plan.py L71-89(与 generate_manifest.py
//     L54-80 同一契约):拒空、拒超长(>512)、拒绝对路径、拒反斜杠、拒冒号
//     (盘符/UNC/ADS 一并挡)、拒空段与 ./.. 段、拒段尾点/空格、拒 Windows
//     保留名(整段或 stem,大小写不敏感,连扩展名一起拒——com1.md 照样惹
//     祸)、拒非法字符(<>:"|?* 与 C0 控制字符);
//   - fold/大小写折叠 口径出 install_plan.py L63-68(CASE_FOLD 常量):
//     Windows/macOS 盘面大小写不敏感,路径键按折叠比较;Linux 不折;
//   - screen_member_name(归档成员名筛查)口径出 scripts/updater.py
//     L487-507:反斜杠折成正斜杠、'.' 与空段折叠、'..' 拒、再过
//     valid_relpath 整体验。
// 口径差(如实声明,不算单边发明):
//   - 长度上限按字节计,python len() 按 codepoint 计——UTF-8 非_ascii 路径
//     C++ 侧只会更严(更早拒),收紧方向,不放松;
//   - 大小写折叠按 ASCII 口径(A-Z 折小写,非 ASCII 字节原样),python
//     casefold() 是 Unicode 全折叠(É->é 之类)——清单路径实测 ASCII 域,
//     两边逐字节一致;键的生成与比较同用 FoldKey,自洽不漏判。
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace lubancode::updater {

// 当前平台是否做大小写折叠(编译期口径,对齐 install_plan.py 的 CASE_FOLD:
// os.name != "posix" or sys.platform == "darwin")。接口签名平台无关,平台
// 差异全部收在实现里;后续批次与测试册用这个常量断言平台口径,不各自 #if。
inline constexpr bool kCaseFoldingActive =
#if defined(_WIN32) || defined(__APPLE__)
    true;
#else
    false;
#endif

// 包内相对路径筛查(install_plan.py valid_relpath 同一契约,见文件头)。
// true = 可入清单/可落盘;false = 拒。不做成员名折叠——那是
// NormalizeMemberName 的活,这里只认已规整的相对路径。
bool ValidRelpath(std::string_view path);

// 路径比较键:Windows/macOS 折叠 ASCII 大小写,Linux 原样返回(口径见
// 文件头)。键只用于 map 查找与碰撞判定,生成与比较必须同走此口。
std::string FoldKey(std::string_view path);

// 归档成员名筛查(updater.py screen_member_name 同一契约):'\' 折成
// '/',空段与 '.' 段折叠,'..' 拒,折完再过 ValidRelpath 整体验。
// 通过返回规整后的相对路径;不通过(含折完为空)返回 nullopt。
std::optional<std::string> NormalizeMemberName(std::string_view name);

// 用户数据路径判定(安装根下不动产权的边界):config.toml 与 .env 是根级
// 用户文件,.lubancode/ 与 .agents/ 是用户目录前缀(目录名本身也算)。
// 其余(含 skills/config.toml 这类树内同名件)一律不是。
bool IsUserDataPath(std::string_view rel);

}  // namespace lubancode::updater
