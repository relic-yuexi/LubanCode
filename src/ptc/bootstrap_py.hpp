// PTC Python 侧运行时(ptc_runtime.py / ptc_main.py)的嵌入源码。
//
// 宿主把这几份文本写进临时目录,连同生成的 luban_tools.py 与模型脚本
// ptc_script.py,一起喂给沙箱里的 python -I ptc_main.py。内嵌而不随包
// 散文件:装好的 exe 不多带一个资源目录,版本也跟宿主二进制一处走
// (画像指纹里的 "PTC prompt/stub 版本" 指这个)。
//
// ptc_runtime.py:framed RPC 通道 + 受限环境(import 白名单、危险内建封禁、
//   stdout 捕获)。可 import,无副作用。
// ptc_main.py:入口。读脚本 -> 握手 -> 上护栏 -> 跑脚本 -> emit/done/fail。
//
// 防线层次(如实交账):
//   1. Python 层护栏(这里):import 白名单 + sys.modules 清洗 + 内建
//      open/input 封禁。防君子——模型脚本乱 import 会被拒,但不防蓄意
//      逃逸(元编程可翻)。
//   2. OS 层沙箱(runner):Windows Job Object + 受限 token;POSIX rlimit
//      只限资源不限文件系统/网络,故 POSIX 无可靠沙箱,默认禁 PTC。

#pragma once

namespace lubancode::ptc {

// ptc_runtime.py 全文。
extern const char* kPtcRuntimePython;
// ptc_main.py 全文。
extern const char* kPtcMainPython;

}  // namespace lubancode::ptc
