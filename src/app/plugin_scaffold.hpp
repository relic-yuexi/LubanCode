// `lubancode plugin init` 的脚手架(plugins 单第 3 步:Python scaffold)。
//
// 便利来自 scaffold,不来自危险反射:生成的 manifest 是静态真账(plugin.json
// 手写格式,扫描期不 import 任何 Python 模块);生成的 runner.py 是单子
// 「Python 作者体验」里那枚最小 runner 的成器版——读 stdin 一份 JSON、按
// tool 字段分派、回 stdout 一份 JSON。test_runner.py 用纯标准库(unittest)
// 起一份假请求直灌,不依赖宿主,作者本地 `python test_runner.py` 便能自测。
//
// 缺 Python/缺依赖的诊断不在这里猜:生成后当场跑一次 doctor 式探测
// (解释器探得到、模板脚本能起),把结论交回给调用方打印。CI 有 Python
// 便真跑,没有就明确 skip(单测侧的规矩,见 test_plugin_scaffold.cpp)。
#pragma once

#include <expected>
#include <string>
#include <vector>

namespace lubancode::app {

// 一次 init 的结果(给调用方打印;不自己动 std::cout——单测要能断言)。
struct PluginScaffoldResult {
    std::string template_name;       // "python"
    std::string plugin_name;         // 过了 IsValidPluginIdentifier 的名字
    std::string target_dir_utf8;     // 落盘目录(UTF-8)
    std::vector<std::string> files;  // 写出的文件名(不含目录)
    // 生成后的环境诊断(人话,可能多行;空 = 一切正常)。
    std::vector<std::string> doctor_notes;
};

// 生成 Python 插件脚手架。target_root 是插件根目录(通常是
// ~/.lubancode/plugins),插件落在 <target_root>/<plugin_name>/。
// 已有同名目录且非空:拒绝(不覆盖用户东西),错误人话说明冲突路径。
// python_command:manifest 里写的 command(缺省 python3/Windows python,
// 与 ProbePythonInterpreter 的候选一致;调用方探到别的解释器可以传进来)。
std::expected<PluginScaffoldResult, std::string> ScaffoldPythonPlugin(
    const std::string& target_root_utf8, const std::string& plugin_name,
    const std::string& python_command);

}  // namespace lubancode::app
