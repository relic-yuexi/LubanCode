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
// 默认静默。CLI 在入口显式安装 stderr writer;嵌入宿主不必先接管进程
// 标准流。SDK 创建/关闭不会替换这只出口。线程安全(自带锁)。
//
// 依赖:只认标准库,platform 层,谁都能引。

#pragma once

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

// 进程级兼容出口。宿主显式挂回调;未配置时不触碰标准流。
class LogSink {
public:
    using Writer = std::function<void(const LogRecord&)>;

    static LogSink& Instance() {
        static LogSink sink;
        return sink;
    }

    // 换落笔(宿主装配时一次;测试各自挂各自的)。传空恢复静默。
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
        }
    }

    std::mutex mutex_;
    Writer writer_;
};

}  // namespace lubancode::platform
