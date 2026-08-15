// 请求前缀记账(前缀缓存守恒单第三期):把"后一份请求是不是前一份请求
// 的原样追加版"写成纯函数,供回归测试钉死与 AgentLoop 逐请求记账共用。
//
// 稳定前缀是三家 wire 共用的请求纪律,不只 DeepSeek:已经发给模型的
// system、tools 与旧消息,不得追改;新材料只往尾部添。断了不可耻,无名
// 无姓、每轮偷偷断一次才可耻——DiffRequests 把断因点名(model/system/
// tools/旧消息追改),cache epoch 的账跟着走。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "api/types.hpp"

namespace lubancode::agent {

// 两份请求的前缀差异,结构化结果。
struct PrefixDiff {
    bool model_changed = false;
    bool system_changed = false;
    bool tools_changed = false;
    // 旧消息(两份请求共有的那部分)是否逐条深等;next 比.prev 长出来的
    // 尾部消息不算"改"。
    bool messages_append_only = true;
    bool old_message_changed = false;
    std::size_t old_message_changed_at = 0;  // 违反追加律的第一条旧消息下标
    std::size_t appended_messages = 0;       // 尾部新添的消息条数

    // 整体判定:追加律成立 = 模型/system/tools 没动,旧消息没被追改。
    bool append_only() const {
        return !model_changed && !system_changed && !tools_changed && messages_append_only;
    }

    // 断因点名(第一次命中的那根梁;没断是空串)。多根梁同时断时按
    // model > system > tools > 旧消息 的顺序报第一根,排序只为稳定输出,
    // 不是重要度排序。
    std::string break_reason() const {
        if (model_changed) return "model_changed";
        if (system_changed) return "system_changed";
        if (tools_changed) return "tools_changed";
        if (old_message_changed) return "old_message_changed";
        return std::string();
    }
};

// 逐项对比:model、system、tools(名字+描述+schema,含次序)与旧消息
// (逐块深等)。prev.messages 必须是 next.messages 的前缀(逐条相等),
// 尾部多出来的消息才算"追加"。
PrefixDiff DiffRequests(const api::Request& prev, const api::Request& next);

// 纯函数判定:next 是 prev 的原样追加版。规格"请求前缀测试"钉的就是它。
inline bool IsAppendOnlySuccessor(const api::Request& prev, const api::Request& next) {
    return DiffRequests(prev, next).append_only();
}

// 请求指纹:把 model/system/每条消息/tools 各自折成一段稳定字符串
// (FNV-1a,agent/context_events.cpp 同款),不落正文——诊断与 epoch 记账
// 只留 hash 与长度,不把 prompt 全文写进日志。
struct PrefixFingerprint {
    std::string model;
    std::string system_hash;
    std::string tools_hash;
    std::vector<std::string> message_hashes;
};

PrefixFingerprint FingerprintRequest(const api::Request& request);

// 指纹对比:与 DiffRequests 同一套判定,只是拿 hash 比(逐字节比不动的
// 大历史用这份;测试与小历史直接用 DiffRequests)。
PrefixDiff DiffFingerprints(const PrefixFingerprint& prev, const PrefixFingerprint& next);

}  // namespace lubancode::agent
