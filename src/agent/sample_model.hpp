// SampleModel 原语(骨架拆解单批一·病四):一次"无工具、单发"的模型采样。
//
// 六处同一个需求——goal evaluator、workflow llm 节点、会话起名、compact 的
// map/reduce、microcompact、记忆抽取——原先各自攒流、各自算 usage、各自兜
// 错,六份 send_stream 手稿只在提示拼装上不同。这册把"路"收成一份:拼
// api::Request -> (可选)看门狗 -> send_stream -> MessageAssembler 攒正文 ->
// usage/错误统一折算。提示拼装、输出校验、错误文案仍是各调用方的活——
// 重构的是路,不是行为。
//
// 放 agent/ 的理由:原语只依赖 api/(Backend/Assembler/Usage)加标准库;
// 六个调用方分布在 agent/app/runtime/workflow 四层,依赖方向
// workflow->agent、runtime->agent、app->agent 全部合法,放引擎屋人人够得
// 着。与 model_router.hpp 同屋正好成一对:那边管"该用谁"(路由决策纯
// 函数),这边管"怎么采"。
//
// 行为对账(六处旧口径逐一保留,改一处须回这里对账):
//   - usage 半截也出账:发送失败/流内错时 usage/text 照带回(旧路都是先
//     记账再判错,不许这里改成失败就丢账)。
//   - 流内 StreamError 折成 ErrorKind::Api + 原消息(compact/起名/抽取/
//     microcompact/evaluator 的旧折法);发送失败原样透传(kind 保留,
//     llm 节点靠 Cancelled 区分 cancelled/api_error)。
//   - 正文取 assembler 的 TextBlock 串,半截流(无收尾事件)先
//     FinalizeOpenBlock 再取——llm 节点旧路按裸 TextDelta 累加,半截文本
//     不许丢。
//   - duration_ms 从起跑(看门狗线程起前)量到收工(join 完),与起名/
//     抽取/microcompact 旧钟同一跨度;compact 的 map/reduce 不吃这份时长,
//     调用方自己按旧口径落账。

#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "agent/model_router.hpp"  // BackgroundCallAccounting:usage 出账的统一口径
#include "api/assembler.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"

namespace lubancode::agent {

// 一次采样的入参。messages 一般就一条 user(compact 的 map/reduce 与全量
// 压缩要带消息列表,这是唯一的多人场);system 即各调用方拼好的指令。
struct SampleRequest {
    std::string model;
    std::string system;
    std::vector<api::Message> messages;
    std::optional<int> max_tokens;
    std::string reasoning_effort;  // 空 = 请求不带(cheap 路由的 effort 档)
    // 可选 output_schema:设了以后采样成功时本地复检正文——解析成 JSON 再
    // 过一遍 schema(与工具入参同一只校验器)。注意这不上 wire:api 层的
    // 中立 Request 没有 output_schema 字段(四家协议各异,批六 API 合流再
    // 议),六处旧口径都是"schema 拼进提示词 + 调用方自查",这里给的是
    // 调用方自查之后的第二道本地后手。复检结果见 SampleResult::schema_ok。
    nlohmann::json output_schema = nlohmann::json();
};

// 一次采样的执行选项。
struct SampleOptions {
    // 外部取消链(ESC 等)。非空时它是 send_stream 唯一吃的取消口——看门狗
    // 只改本地旗,不并进外部链(goal evaluator 旧口径:外部链在场时超时
    // 不抢断,如实保留)。
    const std::atomic<bool>* cancel = nullptr;
    // > 0 起看门狗:到点拉本地取消旗(与旧六处同一形状:steady clock 差 +
    // 100ms 轮询)。0 = 不起(compact 两处的旧路)。
    int timeout_secs = 0;
};

// 一次采样的产物。失败(!ok)时 text/usage 也照带回——半截流的账不许丢。
struct SampleResult {
    bool ok = false;
    api::Error error;  // !ok 时:发送失败原样(kind 保留),流内错折成 Api
    std::string text;  // assistant 正文(TextBlock 串,半截也保留)
    api::Usage usage;  // assembler 的账(MessageDone 为准)
    // 服务端是否真回报过 usage(五项全零 = 没给,不拿 0 冒充)。
    bool usage_reported = false;
    // 起跑到收工的墙钟(含看门狗全程);不计时口径的调用方(compact 的
    // map/reduce)不吃它。
    std::int64_t duration_ms = 0;
    // output_schema 复检账(请求没带 schema 时恒 true/空):采样成功但正文
    // 解析不动 JSON 或过不了 schema 时为 false,schema_error 给人话。这只
    // 是复检后手,不影响 ok——失败怎么收场由调用方定(旧六处各有兜底)。
    bool schema_ok = true;
    std::string schema_error;
};

// 跑一次采样。同步;永不抛(流内异常由各 backend 折成错误事件/返回值)。
SampleResult SampleModel(api::Backend& backend, const SampleRequest& request, const SampleOptions& options = {});

// BackgroundCallAccounting 出账的唯一写法(六处各自手抄的累加/首报收成
// 一份):usage 五项累加,usage_reported 只置不撤。duration_ms 不在此落——
// 各处口径不一(map/reduce 不计时,其余首包覆盖),调用方按旧口径自己写。
void AddSampleAccounting(BackgroundCallAccounting* accounting, const SampleResult& result);

}  // namespace lubancode::agent
