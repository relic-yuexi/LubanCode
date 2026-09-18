#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>

#include "agent/context.hpp"
#include "runtime/event.hpp"

namespace lubancode::runtime {

// 每轮一只，只读现有事件。显示失败不参与执行裁决，不改持久用量账。
class HeadlessProgressReporter {
public:
    using Emit = std::function<void(const std::string&)>;
    HeadlessProgressReporter(Emit emit, std::string label, std::string model);
    void Observe(const ServerEvent& event);
    void Context(const agent::ContextPressure& pressure);
    void Note(const std::string& text) const;
    static std::string Preview(const std::string& text, std::size_t cap = 240);

private:
    Emit emit_;
    std::mutex mutex_;
    std::string label_;
    std::string model_;
    std::map<std::string, std::string> tools_;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::int64_t input_ = 0, output_ = 0, cache_read_ = 0, cache_write_ = 0;
    int requests_ = 0, usage_reports_ = 0, reads_reported_ = 0, writes_reported_ = 0;
    std::size_t window_ = 0;
};

}  // namespace lubancode::runtime
