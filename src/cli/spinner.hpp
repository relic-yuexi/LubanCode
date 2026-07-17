// "思考中" 转轮:发出请求到第一个流事件到达之间,原地转一个字符,让人
// 知道程序没卡死。enabled=false(非真控制台,比如管道/重定向)时整个类
// 什么都不做——不起线程、不写一个字符,绝不污染管道输出。

#pragma once

#include <atomic>
#include <thread>

#include "cli/theme.hpp"

namespace lubancode::cli {

class Spinner {
public:
    // theme 只取 spinner/reset 两个字段。enabled 为真时,构造函数立即起一个
    // 小线程,按固定间隔把 `\r` + 转轮字符 + " 思考中" 打到 stdout 上,原地转。
    Spinner(const Theme& theme, bool enabled);

    // 没显式 Stop() 过的话,析构时自动停(RAII 兜底,不会泄漏线程)。
    ~Spinner();

    Spinner(const Spinner&) = delete;
    Spinner& operator=(const Spinner&) = delete;
    Spinner(Spinner&&) = delete;
    Spinner& operator=(Spinner&&) = delete;

    // 停止转轮:置停止标志、join 线程、把这一行擦干净(`\r` + 空格 + `\r`)。
    // 重复调用是安全的,第二次起是空操作。enabled=false 时天然是空操作。
    void Stop();

private:
    std::thread thread_;
    std::atomic<bool> stop_flag_{false};
    bool enabled_;
    bool stopped_;
};

}  // namespace lubancode::cli
