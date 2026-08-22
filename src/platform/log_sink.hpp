// LogSink(显示系统剥离单第八步,单子"六、日志")。
//
// 引擎层的诊断出口:agent/tools/api 里原先的直接 std::cerr 全改投这里。
// 分 debug/info/warn/error,带 component 与结构字段;不认终端、不带
// ANSI、不按宽截断——落笔到哪(stderr、文件、app-server 的诊断通道)由
// 挂进来的回调定。
//
// 分账规矩(单子原文):用户可见 ErrorEvent 与诊断日志分账。一次错误不能
// 又发事件、又裸写 stderr、又塞工具结果,重复三遍——引擎只投 LogSink;
// "要不要给人看"由前端决定(app-server 保证 stdout 只有协议,警告走
// stderr 或诊断通道)。
//
// 默认行为:没挂回调时 warn/error 落 stderr(app-server 起服时换成自己的
// 通道,免得裸字节漏进协议管道),debug/info 丢弃(不打印)。线程安全
// (自带锁)。
//
// 依赖:只认标准库,platform 层,谁都能引。

#pragma once

#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <utility>

namespace lubancode::platform {

enum class LogLevel { Debug, Info, Warn, Error };

struct LogRecord {
    LogLevel level = LogLevel::Info;
    std::string component;  // 出身:loop / hooks / skills / anthropic …
    std::string message;    // 单行人话(诊断用,不是给最终用户的翻译文案)
};

// 进程级出口。挂回调替换落笔;不挂按默认(warn/error -> stderr)。
class LogSink {
public:
    using Writer = std::function<void(const LogRecord&)>;

    static LogSink& Instance() {
        static LogSink sink;
        return sink;
    }

    // 换落笔(前端装配时一次;测试各自挂各自的)。传空回到默认。
    void SetWriter(Writer writer) {
        std::lock_guard<std::mutex> lock(mutex_);
        writer_ = std::move(writer);
    }

    void Debug(const std::string& component, const std::string& message) {
        Emit(LogRecord{LogLevel::Debug, component, message});
    }
    void Info(const std::string& component, const std::string& message) {
        Emit(LogRecord{LogLevel::Info, component, message});
    }
    void Warn(const std::string& component, const std::string& message) {
        Emit(LogRecord{LogLevel::Warn, component, message});
    }
    void Error(const std::string& component, const std::string& message) {
        Emit(LogRecord{LogLevel::Error, component, message});
    }

private:
    void Emit(LogRecord record) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (writer_) {
            writer_(record);
            return;
        }
        // 默认:warn/error 落 stderr(诊断不是协议,不该进 stdout);debug/
        // info 丢弃——引擎的低频絮叨不默认刷屏。
        if (record.level == LogLevel::Warn || record.level == LogLevel::Error) {
            std::fprintf(stderr, "[%s] %s\n", record.component.c_str(), record.message.c_str());
        }
    }

    std::mutex mutex_;
    Writer writer_;
};

}  // namespace lubancode::platform
