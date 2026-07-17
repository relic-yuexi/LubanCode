#include "cli/spinner.hpp"

#include <chrono>
#include <iostream>
#include <mutex>
#include <string>

#include "cli/console_input.hpp"

namespace lubancode::cli {

namespace {

// Windows 控制台字体保险起见用纯 ASCII 转轮,不用 ⠋⠙⠹ 这类 Unicode
// 盲文块字符(部分控制台字体/代码页下会显示成方块或问号)。
constexpr char kFrames[] = {'-', '\\', '|', '/'};
constexpr int kFrameCount = 4;
constexpr auto kFrameInterval = std::chrono::milliseconds(120);

}  // namespace

Spinner::Spinner(const Theme& theme, bool enabled) : enabled_(enabled), stopped_(!enabled) {
    if (!enabled_) {
        return;
    }
    thread_ = std::thread([this, theme] {
        int frame = 0;
        while (!stop_flag_.load(std::memory_order_relaxed)) {
            {
                // 跟 ESC 监听线程的"已打断/已排队"提示共用一个 stdout 锁,
                // 不持锁的话转轮帧会跟那些提示交错,花屏。
                std::lock_guard<std::mutex> lock(StdoutWriteMutex());
                std::cout << "\r" << theme.spinner << kFrames[frame % kFrameCount] << theme.reset << " 思考中"
                          << std::flush;
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
    // 擦掉这一行:回车 + 足够宽的空格盖掉 "- 思考中" 之类的内容 + 再回车。
    {
        std::lock_guard<std::mutex> lock(StdoutWriteMutex());
        std::cout << "\r" << std::string(16, ' ') << "\r" << std::flush;
    }
    stopped_ = true;
}

}  // namespace lubancode::cli
