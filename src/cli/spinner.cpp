#include "cli/spinner.hpp"
#include "cli/terminal_port.hpp"  // TermOut/TermErr:散打 std::cout 清零,统一走输出端口

#include <algorithm>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include "cli/console_input.hpp"
#include "cli/i18n.hpp"

namespace lubancode::cli {

namespace {

constexpr auto kFrameInterval = std::chrono::milliseconds(140);

std::vector<std::string> Utf8Glyphs(const std::string& text) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        std::size_t bytes = 1;
        if ((lead & 0xE0U) == 0xC0U) {
            bytes = 2;
        } else if ((lead & 0xF0U) == 0xE0U) {
            bytes = 3;
        } else if ((lead & 0xF8U) == 0xF0U) {
            bytes = 4;
        }
        bytes = (std::min)(bytes, text.size() - i);
        out.push_back(text.substr(i, bytes));
        i += bytes;
    }
    return out;
}

}  // namespace

Spinner::Spinner(const Theme& theme, bool enabled) : enabled_(enabled), stopped_(!enabled) {
    if (!enabled_) {
        return;
    }
    const std::string label = tr("spinner.thinking");
    // RunTurn 已启用 footer 时，Working 是同一帧里的状态行；/compact 等
    // 独立场景没有 footer，才沿用原地单行 spinner。
    footer_mode_ = StartStreamFooterWorking(label);
    thread_ = std::thread([this, theme, label] {
        std::size_t frame = 0;
        const auto started = std::chrono::steady_clock::now();
        const std::vector<std::string> glyphs = Utf8Glyphs(label);
        while (!stop_flag_.load(std::memory_order_relaxed)) {
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count();
            if (footer_mode_) {
                UpdateStreamFooterWorking(label, frame, seconds);
            } else {
                // 跟 ESC 监听线程的"已打断/已排队"提示共用一个 stdout 锁,
                // 不持锁的话转轮帧会跟那些提示交错,花屏。
                std::lock_guard<std::mutex> lock(StdoutWriteMutex());
                TermOut() << "\r\x1b[2K" << theme.spinner << "• " << theme.reset;
                for (std::size_t i = 0; i < glyphs.size(); ++i) {
                    const bool lit = !theme.reset.empty() && i == frame % glyphs.size();
                    TermOut() << (lit ? theme.spinner : theme.stats) << glyphs[i];
                }
                TermOut() << theme.stats << " (" << seconds << "s)"
                          << theme.reset << std::flush;
            }
            ++frame;
            std::this_thread::sleep_for(kFrameInterval);
        }
    });
}

Spinner::~Spinner() {
    Stop();
}

void Spinner::Stop() {
    if (stopped_) {
        return;
    }
    stop_flag_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) {
        thread_.join();
    }
    if (footer_mode_) {
        StopStreamFooterWorking();
    } else {
        // 独立单行模式整行擦净，免得耗时数字变长后留下尾巴。
        std::lock_guard<std::mutex> lock(StdoutWriteMutex());
        TermOut() << "\r\x1b[2K\r" << std::flush;
    }
    stopped_ = true;
}

}  // namespace lubancode::cli
