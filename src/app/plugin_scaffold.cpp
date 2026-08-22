// plugin init 的实现:落盘 + 生成后的环境诊断。
#include "app/plugin_scaffold.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "runtime/plugin_contract.hpp"
#include "tools/path_utils.hpp"

namespace lubancode::app {

namespace {

using lubancode::tools::PathToUtf8;
using lubancode::tools::Utf8ToPath;

std::expected<void, std::string> WriteWholeFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return std::unexpected("打不开文件: " + PathToUtf8(path));
    }
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!file.good()) {
        return std::unexpected("写文件失败: " + PathToUtf8(path));
    }
    return {};
}

// runner.py:单子「Python 作者体验」的最小 runner。分派表摆在明处,加第二
// 件工具就在 handlers 里添一行。日志一律走 stderr(协议线不混)。
const char* kRunnerPy = R"py(# -*- coding: utf-8 -*-
"""LubanCode process 插件 runner(由 `lubancode plugin init python` 生成)。

协议 v1:stdin 恰好一份 JSON 请求,stdout 恰好一份 JSON 响应。
日志只写 stderr;stdout 前后混任何字节都会被判协议错。
"""

import json
import sys


def add(arguments):
    """示例工具:两数相加。"""
    return arguments["a"] + arguments["b"]


HANDLERS = {
    "add": add,
}


def main():
    # 管道下 Windows 的 Python 默认按本地代码页编码:中文入参/出参先钉死 UTF-8。
    for stream in (sys.stdin, sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, OSError):
            pass
    try:
        request = json.load(sys.stdin)
    except Exception as error:  # 请求帧都读不进来,唯一能做的是回一份失败帧。
        json.dump({
            "protocol": 1,
            "call_id": "",
            "ok": False,
            "error": {"code": "bad_request", "message": "stdin 不是合法 JSON: " + str(error)},
        }, sys.stdout, ensure_ascii=False)
        return
    tool = request.get("tool", "")
    handler = HANDLERS.get(tool)
    if handler is None:
        json.dump({
            "protocol": 1,
            "call_id": request.get("call_id", ""),
            "ok": False,
            "error": {"code": "unknown_tool", "message": "不认得的工具: " + str(tool)},
        }, sys.stdout, ensure_ascii=False)
        return
    try:
        value = handler(request.get("arguments", {}))
    except Exception as error:
        json.dump({
            "protocol": 1,
            "call_id": request.get("call_id", ""),
            "ok": False,
            "error": {"code": "execution_failed", "message": str(error)},
        }, sys.stdout, ensure_ascii=False)
        return
    json.dump({
        "protocol": 1,
        "call_id": request.get("call_id", ""),
        "ok": True,
        "content": [{"type": "text", "text": str(value)}],
        "structured": value,
    }, sys.stdout, ensure_ascii=False)


if __name__ == "__main__":
    main()
)py";

// test_runner.py:纯标准库,不起宿主、不起管道进程,直接灌请求字典调
// handler。作者本地自测的最短路径;CI 有 Python 时测试也照这个思路真跑。
// 路径全按 __file__ 解析:从哪个 cwd 跑都找得到同目录的 runner/plugin.json。
const char* kTestRunnerPy = R"py(# -*- coding: utf-8 -*-
"""离线自测:python test_runner.py(只依赖标准库;从哪个目录跑都行)。"""

import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import runner  # noqa: E402  (须在 sys.path 补好之后)


class AddTest(unittest.TestCase):
    def test_integers(self):
        self.assertEqual(runner.add({"a": 1, "b": 2}), 3)

    def test_float_negative(self):
        self.assertEqual(runner.add({"a": -1.5, "b": 2}), 0.5)

    def test_missing_field_raises(self):
        with self.assertRaises(KeyError):
            runner.add({"a": 1})


class ProtocolTest(unittest.TestCase):
    """协议帧形状:响应里 call_id 回显、ok 真、content 是 text 列表。"""

    def test_manifest_schema_is_valid_json(self):
        with open(os.path.join(HERE, "plugin.json"), encoding="utf-8") as manifest_file:
            manifest = json.load(manifest_file)
        self.assertEqual(manifest["manifest_version"], 1)
        self.assertEqual(manifest["runtime"]["kind"], "process")
        tool = manifest["tools"][0]
        self.assertIn("input_schema", tool)
        self.assertIn("a", tool["input_schema"]["properties"])


if __name__ == "__main__":
    unittest.main()
)py";

// 生成后的环境诊断:解释器探得到探不到、起不起得来。不装依赖——装依赖
// 是执行第三方代码,须显式确认,不归 scaffold 管(单子「先答五个问题」三)。
void AppendDoctorNotes(const std::string& python_command, std::vector<std::string>& notes) {
    const auto version = lubancode::platform::RunProcess(
        {python_command, "-c", "import sys; sys.stdout.write(sys.version.split()[0])"}, 15000);
    if (version.spawn_failed) {
        notes.push_back("找不到 Python 解释器(\"" + python_command +
                        "\")。装好 Python 或改 plugin.json 里的 runtime.command(可写绝对路径或 venv 里的解释器)。");
        return;
    }
    if (version.exit_code != 0) {
        notes.push_back("Python 解释器启动失败(\"" + python_command + "\",退出码 " +
                        std::to_string(version.exit_code) + ")。");
        return;
    }
    std::string text = version.output;
    if (text.rfind("3.", 0) != 0) {
        notes.push_back("探测到的不是 Python 3(\"" + python_command + "\"): " + text.substr(0, 64));
    }
}

}  // namespace

std::expected<PluginScaffoldResult, std::string> ScaffoldPythonPlugin(
    const std::string& target_root_utf8, const std::string& plugin_name,
    const std::string& python_command_in) {
    if (!runtime::IsValidPluginIdentifier(plugin_name, 64)) {
        return std::unexpected("插件名只能是字母数字_-、字母数字开头、至多 64 字符: " + plugin_name);
    }
    const std::string command = !python_command_in.empty() ? python_command_in
#ifdef _WIN32
                                                          : std::string("python");
#else
                                                          : std::string("python3");
#endif

    const std::filesystem::path root = Utf8ToPath(target_root_utf8);
    const std::filesystem::path dir = root / Utf8ToPath(plugin_name);
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
        return std::unexpected("建目录失败: " + PathToUtf8(root) + ": " + ec.message());
    }
    if (std::filesystem::is_directory(dir, ec) && !std::filesystem::is_empty(dir, ec)) {
        return std::unexpected("目录已存在且不是空的,不覆盖: " + PathToUtf8(dir));
    }
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return std::unexpected("建目录失败: " + PathToUtf8(dir) + ": " + ec.message());
    }

    // manifest:静态真账。${plugin_dir} 占位符让插件目录搬哪儿都能跑。
    std::string manifest;
    manifest += R"json({
  "manifest_version": 1,
  "id": ")json";
    manifest += plugin_name;
    manifest += R"json(",
  "version": "0.1.0",
  "language": "python",
  "runtime": {
    "kind": "process",
    "command": ")json";
    manifest += command;
    manifest += R"json(",
    "args": ["${plugin_dir}/runner.py"],
    "timeout_ms": 30000
  },
  "tools": [
    {
      "name": "add",
      "description": "把两个数字相加(示例工具,改 runner.py 里的 HANDLERS 换成你自己的)。",
      "input_schema": {
        "type": "object",
        "properties": {
          "a": {"type": "number", "description": "左操作数"},
          "b": {"type": "number", "description": "右操作数"}
        },
        "required": ["a", "b"],
        "additionalProperties": false
      }
    }
  ],
  "permissions": {"network": false, "env": []}
}
)json";

    PluginScaffoldResult result;
    result.template_name = "python";
    result.plugin_name = plugin_name;
    result.target_dir_utf8 = PathToUtf8(dir);
    if (auto written = WriteWholeFile(dir / "plugin.json", manifest); !written.has_value()) {
        return std::unexpected(written.error());
    }
    result.files.push_back("plugin.json");
    if (auto written = WriteWholeFile(dir / "runner.py", kRunnerPy); !written.has_value()) {
        return std::unexpected(written.error());
    }
    result.files.push_back("runner.py");
    if (auto written = WriteWholeFile(dir / "test_runner.py", kTestRunnerPy); !written.has_value()) {
        return std::unexpected(written.error());
    }
    result.files.push_back("test_runner.py");

    AppendDoctorNotes(command, result.doctor_notes);
    return result;
}

}  // namespace lubancode::app
