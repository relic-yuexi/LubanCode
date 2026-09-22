// 无策略 shell 词法(AR-07 合并单):把"一条命令拆成什么段、什么词、
// 有没有重定向/子表达式/脚本块"这类词法事实收成一份,供审批闸
// (command_safety 的 ClassifyCommand)与 Plan 只读闸(runtime/plan_mode
// 的 ClassifyPlanShell)共用。原先两处各写一套引号状态机与拆词,只有
// 函数名不同——同一份引号规则改动须同步两边,漏改一边就让同条命令在
// 两道门里得到不同词法事实(AR-07 病)。
//
// 边界:这里只有词法,没有判词。白名单/黑名单/NeedsConfirm/ReadOnly
// 这些策略各调用方自理,不许经本模块外借——Plan 不等于自动审批,两张
// 策略表必须分家(单子明令)。
//
// single_quotes 语义:单引号算不算引号。powershell 算,cmd 不算——cmd
// 的单引号不是引号(echo 'a && del x' 在 cmd 里真会跑 del),把它当
// 引号反倒会漏放。这是 shell 语法事实,不是策略。

#pragma once

#include <string>
#include <vector>

namespace lubancode::tools {

// ASCII 小写化。词形归一与查表前的共用辅件。
std::string ToLowerWord(std::string s);

// 简单引号状态机走一遍,把命令按引号外的分隔符(& | ; 换行)拆成段。
// &&/|| 连写会在中缝拆出空段,消费方按纯空白段跳过。
std::vector<std::string> SplitSegments(const std::string& command, bool single_quotes);

// 段内引号外有没有重定向字符(> >> <)。
bool HasUnquotedRedirection(const std::string& segment, bool single_quotes);

// 段内"单引号外"有没有 $((powershell 子表达式,双引号里照样执行;
// cmd 里 $( 是普通文本,也一并报——顶多多问,不会漏)。
bool HasSubexpression(const std::string& segment, bool single_quotes);

// 段内引号外有没有 PowerShell 脚本块起始 {(原是 plan_mode 的私有件,
// P2-3 单下沉到 command_safety;AR-07 随词法件迁到这儿,与分档逻辑同一
// 份,别写第三份):脚本块体内是任意代码(Where-Object { Remove-Item x }
// 照样逐条执行),静态证明不了无害。cmd 的 { } 没有执行语义,调用方
// 自行按 shell 分流。
bool HasUnquotedScriptBlock(const std::string& segment, bool single_quotes);

// 按引号外空白拆词,词身上的引号字符剥掉("C:\a b\git.exe" 是一个词)。
std::vector<std::string> Tokenize(const std::string& segment, bool single_quotes);

// 词形归一:剥路径前缀取文件名、剥 .exe/.bat/.cmd/.com 扩展、小写化。
// 首词查表用,sudo/写盘 cmdlet 扫全段的时候也用同一套。
std::string NormalizeWord(const std::string& token);

}  // namespace lubancode::tools
