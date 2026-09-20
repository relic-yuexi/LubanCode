// 更新器引擎册的假 EXE 靶子(批三第①单):--version 印 "lubancode <版本>",
// 版本取环境变量 LUBANCODE_PROBE_STUB_VERSION(缺省 0.0.0-stub)。
// 引擎册(tests/integration/updater/test_updater_engine.cpp)的坏包/端到端
// 矩阵用它充当包内 EXE——小、快、不初始化任何大件;真 EXE 的三平台探针
// 另有专测,直接用 LUBANCODE_BINARY_DIR 的主程序(python 时代假 exe 只能
// POSIX,C++ 化后这是红利)。
//
// 可选的"第 N 次故意失败"开关(健康检查失败 -> 回滚路的确定性触发器):
// 设 LUBANCODE_PROBE_STUB_FAIL_ON=<N> 与 LUBANCODE_PROBE_STUB_COUNTER=<文件>,
// 每次调用把计数文件 +1 写回,第 N 次印 "lubancode boom"(探针按版本不合
// 判失败),其余次照常。不设两变量时行为与老版完全一致。
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    const char* version = std::getenv("LUBANCODE_PROBE_STUB_VERSION");
    const char* fail_on = std::getenv("LUBANCODE_PROBE_STUB_FAIL_ON");
    const char* counter = std::getenv("LUBANCODE_PROBE_STUB_COUNTER");
    int call = 0;
    if (fail_on != nullptr && counter != nullptr) {
        long previous = 0;
        if (std::FILE* in = std::fopen(counter, "r")) {
            if (std::fscanf(in, "%ld", &previous) != 1) previous = 0;
            std::fclose(in);
        }
        call = static_cast<int>(previous) + 1;
        if (std::FILE* out = std::fopen(counter, "w")) {
            std::fprintf(out, "%d", call);
            std::fclose(out);
        }
    }
    const int fail = fail_on != nullptr ? std::atoi(fail_on) : 0;
    if (fail > 0 && call == fail) {
        std::printf("lubancode boom\n");
        return 0;
    }
    std::printf("lubancode %s\n", version != nullptr ? version : "0.0.0-stub");
    return 0;
}
