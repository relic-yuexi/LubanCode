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
    std::string mode_accept_edits; // 接受编辑:紫/品红
    std::string mode_yolo;         // YOLO:红色加粗
    std::string mode_auto;         // 自动模式:黄
    std::string mode_dont_ask;     // 不询问:橙
    std::string danger_mode;       // 旧兼容别名，值同 mode_yolo
    std::string reset;      // 恢复默认颜色;plain 主题这个也是空串

    // diff 预览(Claude Code Update 样式)专用色。前缀 diff_ 是跟兄弟
    // 分支(markdown 渲染)约好的命名空间,别撞名。
    std::string diff_add_bg;   // 新增行整行背景(256 色绿底);plain 空串
    std::string diff_del_bg;   // 删除行整行背景(256 色红底);plain 空串
    std::string diff_line_no;  // 上下文行的行号栏淡色;plain 空串

    // diff 正文的轻量语法色。每一枚只改前景色;diff_syntax_plain 恢复
    // 默认前景并关掉 dim,却不碰背景。如此 token 换色时,外层新增/删除
    // 行的绿底/红底还能一直铺到行尾。
    std::string diff_syntax_plain;
    std::string diff_syntax_keyword;
    std::string diff_syntax_string;
    std::string diff_syntax_number;
    std::string diff_syntax_comment;
    std::string diff_syntax_type;
    std::string diff_syntax_function;

    // 用户输入背景块(终端用户输入背景块单):已提交输入铺一层克制底色,
    // 整行承托、不只染字。语义 token 四枚,不在 renderer 里写死 ANSI——
    // plain/no-color 全空串,退化成 "> " 标记 + 块后空行,不夹一个转义字节。
    std::string surface_user_bg;      // 用户块整行底色(逐行开、逐行关)
    std::string surface_user_fg;      // 用户块正文前景(保对比;空 = 默认前景)
    std::string surface_user_marker;  // "> " 提示符前景(与正文区分)
    std::string surface_padding;      // 块左右留白格的底色(缺省同 bg,可空)
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
