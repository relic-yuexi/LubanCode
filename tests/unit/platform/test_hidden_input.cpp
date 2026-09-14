// 隐藏输入册(QQBot Windows 修复单 §5.1):CI 里 stdin 不是终端,隐藏输入
// 必须干净地报 hidden_input_no_tty,不得降级明文读、不得阻塞等键。
// 真终端上的回显关/复原(RAII、Ctrl+C 处理器)不进 ctest——要真控制台,
// 属手动验收(单内 §七"隐藏输入与取消后回显恢复"的真机半边)。
#include <doctest/doctest.h>

#include "platform/hidden_input.hpp"

using namespace lubancode::platform;

TEST_CASE("非交互终端:隐藏输入明报 no_tty,不读键盘不降级") {
    // ctest 的 stdin 不是控制台(管道/重定向);万一本地真终端里跑 ctest
    // 继承了 TTY,这里不许真去读一行——只在"确实非终端"时断言。
    if (StdinIsTerminal()) {
        INFO("stdin 是真终端(本地交互跑法):非交互断言跳过,不阻塞等键");
        return;
    }
    const auto line = ReadHiddenLine();
    REQUIRE_FALSE(line.has_value());
    CHECK(line.error().reason == "hidden_input_no_tty");
    // 错误文案指路自动化(secret_file/secret_env),不提任何密钥值。
    CHECK(line.error().detail.find("secret_file") != std::string::npos);

    // EchoOffScope 在非终端上 ok() 为假:没有"关了回显却读不了"的中间态。
    EchoOffScope echo_off;
    CHECK_FALSE(echo_off.ok());
}
