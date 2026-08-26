// 终端接线收尾单:输出端口的合同测试——默认指 stdout/stderr、Redirect
// 改道、Reset 收回、便捷口同源。不碰真终端(借 ostringstream),单册
// 可裸跑。

#include <iostream>
#include <sstream>
#include <string>

#include <doctest/doctest.h>

#include "cli/terminal_port.hpp"

TEST_CASE("TerminalPort:默认与便捷口同指进程端口") {
    using lubancode::cli::TermPort;
    using lubancode::cli::TermOut;
    using lubancode::cli::TermErr;
    CHECK(&TermOut() == &TermPort().out());
    CHECK(&TermErr() == &TermPort().err());
}

TEST_CASE("TerminalPort:Redirect 改道后写入落进注入流,Reset 收回") {
    std::ostringstream out_capture;
    std::ostringstream err_capture;
    lubancode::cli::TermPort().Redirect(&out_capture, &err_capture);
    lubancode::cli::TermOut() << "正文一行"
                              << "\n";
    lubancode::cli::TermErr() << "告警一行"
                              << "\n";
    lubancode::cli::TermOut().flush();
    lubancode::cli::TermErr().flush();
    lubancode::cli::TermPort().Reset();
    CHECK(out_capture.str() == "正文一行\n");
    CHECK(err_capture.str() == "告警一行\n");
}

TEST_CASE("TerminalPort:Redirect 传 nullptr 的那只保持原样") {
    std::ostringstream out_capture;
    lubancode::cli::TermPort().Redirect(&out_capture, nullptr);
    CHECK(&lubancode::cli::TermPort().err() == &std::cerr);  // stderr 没被换走
    lubancode::cli::TermOut() << "只改道 stdout\n";
    CHECK(out_capture.str() == "只改道 stdout\n");
    lubancode::cli::TermPort().Reset();
}
