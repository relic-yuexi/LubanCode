// 统一墙钟(骨架拆解批五先行半批:五套台账时间戳同源)。
//
// session JSONL(session_store)、tool_trace(_hub)、loop(LoopClock)、
// goal(装配层喂 now)、workflow(JournalClock/DefaultRunId)原先各读各的
// system_clock——读法一样,口径各立。批五把"现在几点"收成这一枚:
//   - WallClockNowMs():Unix epoch 毫秒(system_clock,与收编前各家的
//     读法逐字节同口径);
//   - WallClockToTimeT(ms):毫秒 -> 历元秒(格式化本地时间用,丢弃的
//     毫秒与 to_time_t(now) 一致)。
//
// 落在 platform/:五家台账分住 engine(agent/)与 runtime 两层,钟必须
// 放两层层层都够得着的地板上;engine 内已有 platform 先例(paths、
// json_safe)。各台账的时间戳表示不变(JSONL 的本地串、journal 的 epoch
// 毫秒),只换读钟的手。
//
// 单测喂 fake clock 的注入口各域自带(LoopClock/JournalClock 可注入),
// 本件只是真钟的唯一落点,不设注入。

#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>

namespace lubancode::platform {

// 现在几点:Unix epoch 毫秒。
inline std::int64_t WallClockNowMs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

// epoch 毫秒 -> 历元秒(本地格式化用;毫秒位丢弃与 to_time_t 一致)。
inline std::time_t WallClockToTimeT(std::int64_t epoch_ms) {
    return static_cast<std::time_t>(epoch_ms / 1000);
}

}  // namespace lubancode::platform
