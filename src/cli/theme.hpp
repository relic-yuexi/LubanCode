// 终端配色。Theme 只存 ANSI 转义序列字符串,不知道自己会不会被真的打印
// 出来——是否真的着色由调用方(main.cpp)根据配置里的 theme 字段 +
// 运行时的控制台探测结果决定,这里只管"选中了某个主题名字之后,每个
// 语义色该是什么 ANSI 序列"。
//
// 三套内置主题:
//   dark  —— 默认,给深色背景终端用,颜色偏亮
//   light —— 给浅色背景终端用,颜色偏深,不刺眼
//   plain —— 全部字段是空串,等于不着色(管道模式 / 控制台不支持 ANSI /
//            用户显式选择时用这个)
//
// 模型正文(on_text_delta 打出来的那部分)故意不在 Theme 里配色——保持
// 原色,不受主题影响,这是任务要求的"模型正文保持原色"。

#pragma once

#include <string>

namespace lubancode::cli {

struct Theme {
    std::string banner;     // 启动横幅
    std::string prompt;     // `> ` 主提示符
    std::string tool_line;  // `[工具] ...` 那一行
    std::string confirm;    // 确认提示(y/a/N)
    std::string error;      // `[错误]`/`[工具出错]`
    std::string stats;      // token 统计行、cwd 提示这类淡色信息
    std::string spinner;    // "思考中" 转轮字符本身的颜色
    std::string reset;      // 恢复默认颜色;plain 主题这个也是空串
};

// 按名字取内置主题;不认得的名字(不是 dark/light/plain)按 dark 处理,
// 不报错——配置文件/环境变量里如果哪天手滑写错了主题名,不该让整个程序
// 起不来,退回默认主题是更友好的做法。
Theme BuiltinTheme(const std::string& name);

// 决定最终生效的主题:enable_colors 为假时,不管 name 是什么,强制返回
// 全空串的 plain 主题——这是"管道模式 / 控制台不支持 ANSI 时强制无色"
// 这条规矩的落地点。
Theme ResolveTheme(const std::string& name, bool enable_colors);

// 探测出来的控制台能力,DetectConsoleCapability() 的返回值。
struct ConsoleCapability {
    // stdout 是不是一个真控制台(不是管道、不是重定向的磁盘文件)。
    // 转轮(spinner)只认这个字段——管道模式下不管有没有强制开色,都绝不
    // 输出转轮字符。
    bool is_console = false;

    // 综合"是不是真控制台"“Windows 下开 VT 处理成不成功”
    // “LUBANCODE_FORCE_COLOR 有没有强制开”三者之后,颜色该不该真的打出来。
    bool colors_enabled = false;
};

// 纯函数,不碰任何 IO:根据三个已经探测好的原始信号,算出颜色该不该开。
// 单拎出来是为了让这条判断逻辑本身可以脱离真实控制台单测。
//   force_color 为真时,不管是不是控制台、VT 开没开成,一律返回 true
//   (给集成测试在管道场景下验证 ANSI 序列用,对应 LUBANCODE_FORCE_COLOR=1)。
//   否则只有"是真控制台 且 VT 虚拟终端处理开成功"才返回 true。
bool ComputeColorsEnabled(bool stdout_is_console, bool vt_processing_ok, bool force_color);

// 真正探测:Windows 下检查 stdout 是不是真控制台、尝试开
// ENABLE_VIRTUAL_TERMINAL_PROCESSING,再读 LUBANCODE_FORCE_COLOR 环境变量,
// 调 ComputeColorsEnabled 算出最终结果。非 Windows 平台用 isatty 顶替
// "是不是真控制台"这一步,VT 处理直接算作"不需要、天然支持"。
ConsoleCapability DetectConsoleCapability();

}  // namespace lubancode::cli
