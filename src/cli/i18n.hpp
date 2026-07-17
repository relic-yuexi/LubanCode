// 界面多语言(i18n):字符串表 + 语言包加载 + 系统语言探测。
//
// 只管"界面上给人看的文案"——发给模型的一切(系统提示、工具描述、
// tool_result)不走这里,那是模型的事。
//
// 机制三句话说清:
//   1. tr(key):查当前语言表(用户语言包盖内置)→ 回退 zh-CN → 回退 key
//      本身。内置两种语言编译进程序:zh-CN(全量)、en(P0 全量,P1 渐进,
//      缺键回退 zh-CN)。
//   2. 语言包:<主目录>/.lubancode/languages/*.json,平面键值对
//      {"help.slash.sessions": "..."},文件名即语言码(ja.json → ja)。
//      启动扫一遍;文件名撞内置语言码(zh-CN.json / en.json)就按键覆盖
//      内置同键——用户改措辞的口子。坏 JSON 警告跳过,不拦人。
//   3. 选择:config 字段 language(四级合并,LUBANCODE_LANG env),空 =
//      跟系统(GetUserDefaultUILanguage,zh 前缀→zh-CN,en 前缀→en,
//      认不出→zh-CN)。/language 会话级即时切换。
//
// 键名规矩:分层小写点号,如 help.slash.sessions、status.mode.confirm、
// confirm.prompt、error.api_key_missing。占位符 {0} {1} ... 由 trf 按序填。
//
// 这个模块是零依赖的叶子(只用标准库 + nlohmann/json),谁都可以引——
// config 层引它不算破坏"依赖只许单向"的层次(它不反过来牵扯任何层)。
//
// 线程注意:SetLanguage/LoadLanguagePacksFromDir 只在主线程(启动、命令
// 提示符之间)调;tr 内部有锁,监听线程打 "[已打断]" 这类提示时并发读安全。

#pragma once

#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace lubancode::cli {

// 查表:当前语言(用户包 → 内置)→ zh-CN(用户包 → 内置)→ key 本身。
// 返回引用在下一次 LoadLanguagePacksFromDir 之前一直有效。
const std::string& tr(std::string_view key);

// 占位符替换:把 tr(key) 里的 {0} {1} ... 换成 args 对应项。没有对应项的
// 占位符原样保留(比错序悄悄吞掉好排查)。
std::string TrFormat(std::string_view key, const std::vector<std::string>& args);

namespace i18n_detail {
inline std::string ToArgString(std::string s) { return s; }
inline std::string ToArgString(const char* s) { return std::string(s); }
inline std::string ToArgString(std::string_view s) { return std::string(s); }
template <typename T>
    requires std::is_arithmetic_v<T>
inline std::string ToArgString(T v) {
    return std::to_string(v);
}
}  // namespace i18n_detail

// trf("cmd.model.switched", name) —— 数字/字符串都认,数字走 std::to_string
// (浮点别直接传,先自己格式化好)。
template <typename... Args>
std::string trf(std::string_view key, Args&&... args) {
    return TrFormat(key, std::vector<std::string>{i18n_detail::ToArgString(std::forward<Args>(args))...});
}

// 当前语言码。初始值 "zh-CN"(测试/未初始化场景的现状默认)。
const std::string& CurrentLanguage();
void SetLanguage(const std::string& code);

// 可选语言 = 内置两种(zh-CN、en)+ 扫到的用户语言包(去重,内置在前)。
std::vector<std::string> AvailableLanguages();
bool HasLanguage(const std::string& code);

// 语言的展示名:内置有固定说法("中文(zh-CN)"/"English (en)");用户包
// 认可选键 "language.name"(包里自报家门),没有就用语言码本身。
std::string LanguageDisplayName(const std::string& code);

// 扫描 <dir>/*.json 加载语言包,整体替换上一次扫描的结果(重复调安全,
// 测试用临时目录反复扫)。返回坏文件的警告(坏 JSON/顶层不是 object/
// 值不是字符串——整文件跳过),每条一行,调用方决定打不打。
std::vector<std::string> LoadLanguagePacksFromDir(const std::string& dir);

// 纯函数:系统 locale 字符串(如 "zh-CN"/"en-US"/"ja-JP")映射到内置语言。
// zh 前缀 → zh-CN,en 前缀 → en,认不出 → zh-CN。
std::string MapLocaleToLanguage(std::string_view locale);

// Windows 下按 GetUserDefaultUILanguage 探测,其余平台看 $LANG;都过
// MapLocaleToLanguage 收口。
std::string DetectSystemLanguage();

}  // namespace lubancode::cli
