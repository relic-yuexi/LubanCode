// 更新器引擎册的假 EXE 靶子(批三第①单):--version 印 "lubancode <版本>",
// 版本取环境变量 LUBANCODE_PROBE_STUB_VERSION(缺省 0.0.0-stub)。
// 引擎册(tests/integration/updater/test_updater_engine.cpp)的坏包/端到端
// 矩阵用它充当包内 EXE——小、快、不初始化任何大件;真 EXE 的三平台探针
// 另有专测,直接用 LUBANCODE_BINARY_DIR 的主程序(python 时代假 exe 只能
// POSIX,C++ 化后这是红利)。
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    const char* version = std::getenv("LUBANCODE_PROBE_STUB_VERSION");
    std::printf("lubancode %s\n", version != nullptr ? version : "0.0.0-stub");
    return 0;
}
