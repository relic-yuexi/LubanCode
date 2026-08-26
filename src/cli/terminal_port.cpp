// 终端输出端口实现(合同见 terminal_port.hpp)。

#include "cli/terminal_port.hpp"

#include <iostream>

namespace lubancode::cli {

std::ostream& TerminalPort::StdOut() { return std::cout; }
std::ostream& TerminalPort::StdErr() { return std::cerr; }

void TerminalPort::Redirect(std::ostream* out, std::ostream* err) {
    if (out != nullptr) {
        out_ = out;
    }
    if (err != nullptr) {
        err_ = err;
    }
}

void TerminalPort::Reset() {
    out_ = &StdOut();
    err_ = &StdErr();
}

TerminalPort& TermPort() {
    static TerminalPort port;
    return port;
}

std::ostream& TermOut() { return TermPort().out(); }

std::ostream& TermErr() { return TermPort().err(); }

}  // namespace lubancode::cli
