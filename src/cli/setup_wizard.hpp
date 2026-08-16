// 初次配置向导:交互模式启动时,base_url 或 api_key(或 model)解不出来,
// 就走这一套问答,问完直接可用,不用重启。
//
// 逻辑写成纯函数 RunSetupWizard,除了几个注入点(打印一行、读一行、拉模型
// 列表、单选菜单)不碰任何真实 IO——生产代码接 std::cout / cli::ReadLine /
// api::ListModels / cli::ReadChoiceMenu,单测接假的打印收集器 / 脚本化输入
// 序列 / 假的模型列表结果,不真发网络请求。

#pragma once

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "api/models.hpp"
#include "api/types.hpp"
#include "config/config.hpp"

namespace lubancode::cli {

// 打印一行文字(不含结尾换行,调用方自己决定怎么加)。
using WizardPrintFn = std::function<void(const std::string& line)>;

// 读一行输入,EOF 返回 std::nullopt。注意这里没有 prompt 参数——向导自己
// 通过 print 把提示语打出来,生产代码接的是
// `[]{ return lubancode::cli::ReadLine(""); }`(prompt 已经打过了,不用
// ReadLine 再打一遍)。
using WizardReadLineFn = std::function<std::optional<std::string>()>;

// 拉取模型列表的注入点,生产代码接 api::ListModels,单测注入假数据/假报错。
using WizardFetchModelsFn = std::function<std::expected<std::vector<api::ModelInfo>, api::Error>(
    config::Wire wire, const std::string& base_url, const std::string& api_key)>;

// 一道单选题的选项:label 是选项正文,description 是补充说明(可空)。
// 故意不直接复用 cli::ChoiceMenuItem——那玩意儿住在 cli/console_input.hpp,
// 拉进本头会连带拽入 line_editor / console 一大堆重依赖。向导是纯逻辑,不该
// 背这些。生产代码注入 choose 时再把 WizardChoiceItem 翻成 ChoiceMenuItem。
struct WizardChoiceItem {
    std::string label;
    std::string description;
};

// ---------------------------------------------------------------------------
// 向导重排单:可测试的导航事件 + 面板帧。往 WizardIO 上添的注入点,不动
// 原有三个——老调用方(单测、setup 向导)不注入,行为一字不变。
// ---------------------------------------------------------------------------

// 一次向导读取的导航语义。分清"提交(空串也算提交)""返回上一步""取消
// 整个向导""EOF"。不拿某个普通文本值冒充返回键。
struct WizardInputEvent {
    enum class Kind { Submitted, Back, Cancelled, Eof };
    Kind kind = Kind::Submitted;
    std::string text;  // Submitted 时有效;生产端已剥首尾空白
};

using WizardReadEventFn = std::function<WizardInputEvent()>;

// 一帧向导面板的内容。draw_frame 注入时 TTY 原地清旧画新(一块稳定交互
// 区,试填不落 transcript);没注入时退化为朴素逐行打印——步骤号、返回
// 键提示与错误仍在,脚本化输入照旧能用。
struct WizardFrame {
    std::string title;              // 标题,如"添 provider"
    std::string progress;           // 进度,如"第 3/8 步",可空
    std::vector<std::string> body;  // 说明/字段正文(不含错误)
    std::string error;              // 当前步骤错误,可空;换步/改正后清
    std::string prompt;             // 文本步骤的输入提示;选择步骤为空
    std::string footer;             // 按键提示,如"Esc 上一步  Ctrl+C 取消"
};

using WizardDrawFrameFn = std::function<void(const WizardFrame&)>;

// 单选注入点:给一堆选项和默认高亮项(0-based),返回选中的下标(0-based);
// 用户取消(Esc/EOF)返回 std::nullopt。cancel_kind(向导重排单)在返回
// nullopt 时分清是 Esc(返回上一步,Kind::Back)还是 Ctrl+C/EOF(Kind::
// Cancelled/Eof);不关心就传 nullptr,行为不变。生产代码接
// cli::ReadChoiceMenu(真终端方向键选择),单测不注入——ReadChoice 回落到
// 编号 read_line,现有脚本化输入序列照旧能用。
using WizardChooseFn = std::function<std::optional<std::size_t>(
    const std::vector<WizardChoiceItem>& items, std::size_t default_index, const std::string& hint,
    WizardInputEvent::Kind* cancel_kind)>;

struct WizardIO {
    WizardPrintFn print;
    WizardReadLineFn read_line;
    WizardFetchModelsFn fetch_models;
    // 单选注入点:真交互终端且已注入时,ReadChoice 走它(方向键菜单)。单测留空
    // 走编号回落。
    WizardChooseFn choose;
    // 向导重排单:导航事件注入点。非空时文本步骤走它(Esc=Back、
    // Ctrl+C=Cancelled、EOF=Eof、回车=Submitted);留空回落 read_line:
    // nullopt 算 Eof,其余算 Submitted。
    WizardReadEventFn read_event;
    // 面板帧注入点。非空时每步/每次重试把整帧交给它原地重画;留空走
    // 朴素逐行(print 每一行,含分隔线与步骤号)。draw_frame 与 reserve
    // 信息的协调见 cli/wizard_panel.hpp(生产端由 MakeInteractiveWizardIO
    // 接线)。
    WizardDrawFrameFn draw_frame;
    // 当前是不是真交互终端(stdin tty 且 stdout 是控制台)。生产代码(main.cpp)
    // 用 platform::StdinIsInteractive() && ProbeStdoutConsole().is_console 设。
    // 为假(管道/重定向/单测)时,ReadChoice 不走 choose,回落到编号 read_line,
    // 保住 echo | lubancode 自动化用法和脚本化单测。默认假。
    bool interactive = false;
    // 展示"保存到 XXX?"这一句用的路径文字,纯数据、不是 IO,调用方(main.cpp)
    // 自己拼好传进来(通常是 config::HomeDir() 拼出来的 .lubancode.json 路径)。
    std::string home_config_display_path = "<主目录>/.lubancode.json";
};

struct WizardOutcome {
    config::Config config;
    bool save_requested = false;  // 用户是否同意把配置写入 home_config_display_path
};

// 中途读到 EOF(read_line 返回 nullopt)按用户放弃处理,返回 std::nullopt。
std::optional<WizardOutcome> RunSetupWizard(WizardIO& io);

// ---------------------------------------------------------------------------
// 下面几个是向导用的通用小工具,原本关在 setup_wizard.cpp 的匿名命名空间里,
// /provider add 向导(cli/provider_wizard.cpp)复用同一套问答机器,所以搬出来
// 导出。行为不变,注释见 setup_wizard.cpp 里原来的位置。
// ---------------------------------------------------------------------------

// 剥掉尾部所有斜杠。
std::string StripTrailingSlashes(const std::string& s);

// 读一行、剥空白;EOF 时返回 std::nullopt。
std::optional<std::string> ReadTrimmed(WizardIO& io, const std::string& prompt);

// 反复问,直到读到非空字符串,或者 EOF(此时返回 std::nullopt)。
std::optional<std::string> ReadRequired(WizardIO& io, const std::string& prompt, const std::string& empty_hint);

// 单选:优先走注入点 io.choose(生产 = 方向键菜单);未注入时回落到编号列表 +
// read_line。items 为选项,default_index 是默认高亮项(0-based,空输入走它),
// hint 是菜单提示语(给方向键菜单用,如「↑/↓ 选择 · Enter 确认」)。超范围/
// 非数字重问,EOF 返回 std::nullopt。返回选中下标(0-based)。
std::optional<std::size_t> ReadChoice(WizardIO& io, const std::vector<WizardChoiceItem>& items,
                                       std::size_t default_index, const std::string& hint);

// model 这一步:输入非空就直接用;输入空就拉列表、编号选;拉取失败/列表为空
// 都回落到"手动输入,必须非空"。返回值为空表示 EOF,调用方原样往上传。
std::optional<std::string> ResolveModel(WizardIO& io, config::Wire wire, const std::string& base_url,
                                         const std::string& api_key);

// ---------------------------------------------------------------------------
// 向导重排单:导航事件与面板帧的回落实现。两个函数都先看注入点,没注入才
// 走朴素路——单测与管道场景因此完全确定。
// ---------------------------------------------------------------------------

// 画一帧:注入了 draw_frame 就交给它(TTY 原地重画);否则朴素逐行打印
// (分隔线 + 标题/步骤 + 正文 + 错误 + 提示 + footer)。
void DrawWizardFrame(WizardIO& io, const WizardFrame& frame);

// 读一个导航事件:注入了 read_event 就交给它;否则回落 read_line——
// nullopt 算 Eof,其余算 Submitted(剥首尾空白)。
WizardInputEvent ReadWizardEvent(WizardIO& io);

// 剥首尾空白(向导文本输入统一走这个;provider 向导与 setup 向导共用)。
std::string WizardTrim(const std::string& s);

}  // namespace lubancode::cli
