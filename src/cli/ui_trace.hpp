// 按代理状态投影单 P0(证据基建):UI 事件链路的诊断台账。
//
// 抓混屏要的不是更多日志,是带身份的账:每笔事件在"接收、状态应用、
// 帧提交"三个关卡各记一行——谁是 owner(哪页会话)、哪一代 session、
// 修订号到了多少、这笔写屏落在哪。三列一对账,"main 的字写进 sub 页"
// 这类事实就从"感觉串了"变成可指认的记录。
//
// 规矩(单子 §七 P0:诊断写文件,不刷屏):
//   - 默认关。设 LUBANCODE_UI_TRACE=<路径> 开文件台账(追加写,每进程
//     一行头部说明);测试用 UiTraceInstallSink 装内存录音机。两者都
//     不碰 TermOut/TermErr——诊断自己不许成为新的写屏者。
//   - 记录极小:定长小结构 + 一行文本,mutex 串行。开着台账的代价是
//     每关卡一次锁一行字,不进任何热路径的判断分支(先查开关,atomic)。
//   - owner 缺席的写屏(例如收口 chrome)也记:owner 填当前查看页,
//     writer 字段写明来路,事后照样能对账。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "cli/agent_view_state.hpp"  // AgentViewKey

namespace lubancode::cli::ui_trace {

// 三个关卡(单子 §七 P0:事件接收、状态应用、帧提交)。
enum class Stage {
    Received,   // 事件进了 UI 边界(Emit/分发入口)
    Applied,    // 状态应用完(视图账/工具配对/统计落账,含修订号)
    Committed,  // 帧提交(这笔内容真的写了屏;writer 标来路)
};

// 一行账。全部值类型,拷贝即走。
struct Record {
    Stage stage = Stage::Received;
    AgentViewKey owner;            // 这笔属于哪页(含 session_generation)
    std::uint64_t view_epoch = 0;  // 换页纪元(登记簿发,0 = 未接登记簿)
    std::uint64_t revision = 0;    // Applied/Committed 带修订号;Received 带事件序
    std::string kind;              // 事件种类/写屏来路的短标签
    std::string writer;            // Committed:写屏者(body/tool/footer/view_frame…)
    std::int64_t timestamp_ms = 0; // Unix epoch 毫秒
};

// 内存录音机(测试与诊断共用)。安装即生效,拆下即停。
using Sink = std::function<void(const Record&)>;

// 开关与安装。InstallSink(nullptr) = 拆录音机。文件台账由 EnableFromEnv()
// 一次性装好(进程启动时调;没设环境变量是空操作)。
void InstallSink(Sink sink);
void EnableFromEnv();
bool Enabled();

// 三个关卡的记法。Enabled() 为假时全部即时返回,一行不写。
void NoteReceived(const AgentViewKey& owner, std::uint64_t view_epoch, const std::string& kind,
                  std::uint64_t event_seq);
void NoteApplied(const AgentViewKey& owner, std::uint64_t view_epoch, const std::string& kind,
                 std::uint64_t revision);
void NoteCommitted(const AgentViewKey& owner, std::uint64_t view_epoch, const std::string& kind,
                   std::uint64_t revision, const std::string& writer);

// 事件种类的短标签(runtime::ServerEventKind 等枚举的稳定字符串由调用方
// 递;这里只提供一个通用格式化,避免各处手拼)。
std::string Describe(const Record& record);

}  // namespace lubancode::cli::ui_trace
