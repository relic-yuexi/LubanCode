// ui_trace 的实现(按代理状态投影单 P0:证据基建)。诊断写文件不刷屏——
// 文件台账经 EnableFromEnv 装,内存录音机经 InstallSink 装;两者都没装时
// 三个关卡全是一枚 atomic 读,零开销。

#include "cli/ui_trace.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <utility>

#include "platform/process.hpp"  // CurrentProcessId(文件台账头行的 pid)

namespace lubancode::cli::ui_trace {

namespace {

// 进程内唯一状态:开关 + 已格式化行的出口。文件台账与内存录音机共用
// 同一出口(都格式化成一行文本),Record 原件只给内存录音机——文件台账
// 不需要结构,少一次拷贝。
struct TraceState {
    std::atomic<bool> enabled{false};
    bool env_installed = false;           // EnableFromEnv 只装一次
    std::mutex mutex;
    Sink sink;                        // 内存录音机(可空)
    std::unique_ptr<std::ofstream> file;  // 文件台账(可空)
};

TraceState& State() {
    static TraceState state;
    return state;
}

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

const char* StageTag(Stage stage) {
    switch (stage) {
        case Stage::Received:
            return "recv";
        case Stage::Applied:
            return "apply";
        case Stage::Committed:
            return "commit";
    }
    return "?";
}

// 统一的落账半边:开着才进来;文件/录音机各走各的出口。
void Emit(Record record) {
    TraceState& state = State();
    if (!state.enabled.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.sink != nullptr) {
        state.sink(record);
    }
    if (state.file != nullptr && state.file->good()) {
        (*state.file) << Describe(record) << "\n";
        state.file->flush();  // 崩溃/挂死现场也要能对账,宁可慢
    }
}

}  // namespace

void InstallSink(Sink sink) {
    TraceState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.sink = std::move(sink);
    state.enabled.store(state.sink != nullptr || state.file != nullptr, std::memory_order_release);
}

bool Enabled() { return State().enabled.load(std::memory_order_acquire); }

void NoteReceived(const AgentViewKey& owner, std::uint64_t view_epoch, const std::string& kind,
                  std::uint64_t event_seq) {
    Record record;
    record.stage = Stage::Received;
    record.owner = owner;
    record.view_epoch = view_epoch;
    record.kind = kind;
    record.revision = event_seq;
    record.timestamp_ms = NowMs();
    Emit(std::move(record));
}

void NoteApplied(const AgentViewKey& owner, std::uint64_t view_epoch, const std::string& kind,
                 std::uint64_t revision) {
    Record record;
    record.stage = Stage::Applied;
    record.owner = owner;
    record.view_epoch = view_epoch;
    record.kind = kind;
    record.revision = revision;
    record.timestamp_ms = NowMs();
    Emit(std::move(record));
}

void NoteCommitted(const AgentViewKey& owner, std::uint64_t view_epoch, const std::string& kind,
                   std::uint64_t revision, const std::string& writer) {
    Record record;
    record.stage = Stage::Committed;
    record.owner = owner;
    record.view_epoch = view_epoch;
    record.kind = kind;
    record.revision = revision;
    record.writer = writer;
    record.timestamp_ms = NowMs();
    Emit(std::move(record));
}

std::string Describe(const Record& record) {
    // 一行一账:时间 | 关卡 | gen | task | epoch | rev | kind | writer。
    char line[512];
    std::snprintf(line, sizeof(line), "%lld | %s | gen=%llu task=%d | epoch=%llu | rev=%llu | %s%s%s",
                  static_cast<long long>(record.timestamp_ms), StageTag(record.stage),
                  static_cast<unsigned long long>(record.owner.session_generation), record.owner.task_id,
                  static_cast<unsigned long long>(record.view_epoch),
                  static_cast<unsigned long long>(record.revision), record.kind.c_str(),
                  record.writer.empty() ? "" : " | ", record.writer.c_str());
    return line;
}

// 文件台账:EnableFromEnv 在 main 装配早期调一次。LUBANCODE_UI_TRACE 给
// 路径就开(相对路径按进程 cwd 解,不给默认落点猜用户目录——诊断文件
// 落哪必须明说)。没设/空串 = 不开。
void EnableFromEnv() {
    TraceState& state = State();
    if (state.env_installed) {
        return;
    }
    state.env_installed = true;
    const char* raw = std::getenv("LUBANCODE_UI_TRACE");
    if (raw == nullptr || *raw == '\0') {
        return;
    }
    auto file = std::make_unique<std::ofstream>(raw, std::ios::app);
    if (!file->good()) {
        return;
    }
    (*file) << "# lubancode ui trace start pid=" << lubancode::platform::CurrentProcessId() << "\n";
    std::lock_guard<std::mutex> lock(state.mutex);
    state.file = std::move(file);
    state.enabled.store(true, std::memory_order_release);
}

}  // namespace lubancode::cli::ui_trace
