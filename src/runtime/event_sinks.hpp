// TerminalEventSink / JsonEventSink(显示系统剥离单第五步后半)。
//
// 单子验收原文:"同一份假 backend 脚本分别喂 TerminalEventSink 与
// JsonEventSink:事件 id、次序、终态和领域数据完全一致,只有渲染不同"。
// 这两只 sink 就是那两位收银员:
//   - TerminalEventSink:ServerEvent 流投影成终端画面(现住 app 侧接线层,
//     第五步先立"回执账本"——事件照单全收、逐条记账,终端渲染接线在
//     SessionRuntime 接线时挂上);
//   - JsonEventSink:ServerEvent 流原样落 JSON 行(ndjson,一行一事件),
//     app-server 的 stdout 协议与脚本桥吃这份数据。
//
// 两家吃同一 ServerEvent 流,谁也不许改事件本体(seq/id/终态/领域数据),
// 这是单子硬边界;测试(test_event_sinks.cpp)钉同一事件流两家收出的
// 账完全一致。
//
// 依赖:JsonEventSink 只认合同头(零实现依赖,放 runtime/);
// TerminalEventSink 同样零依赖——它只做"事件 -> 账本/回调"的翻译,不碰
// std::cout(真终端的那只住终端装配层,turn_runner 在第六步接)。

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/event.hpp"
#include "runtime/event_sink.hpp"

namespace lubancode::runtime {

// ---------------------------------------------------------------------------
// JsonEventSink:事件流 -> ndjson 行
// ---------------------------------------------------------------------------

// 每条事件原样 to_json,一行一事件(\n 收尾)。线程安全(自带锁);收到
// 什么写什么,不重排、不删减、不翻文案——"协议只是适配器"的适配端。
class JsonEventSink final : public EventSink {
public:
    // 落笔回调(持有方管 IO:文件、stdout、字符串缓冲)。在 Emit 的调用
    // 线程上被调;锁只护"一事件一行"的原子性,IO 的快慢由持有方自管。
    using Writer = std::function<void(const std::string& line)>;

    explicit JsonEventSink(Writer writer) : writer_(std::move(writer)) {}

    void Emit(const ServerEvent& event) override {
        if (!writer_) {
            return;
        }
        // 一事件一行,锁内落笔:多线程投递时行与行不劈。
        std::lock_guard<std::mutex> lock(mutex_);
        writer_(event.to_json().dump() + "\n");
        ++emitted_;
    }

    std::uint64_t emitted() const { return emitted_; }

private:
    Writer writer_;
    std::mutex mutex_;
    std::uint64_t emitted_ = 0;
};

// ---------------------------------------------------------------------------
// TerminalEventSink:事件流 -> 终端投影的原材料账本
// ---------------------------------------------------------------------------

// 终端条目的"原材料":从 ServerEvent 流归并出的条目账(item_id/工具名/
// 状态/正文累计/终态),终端渲染层(TranscriptPainter 那一路)拿它翻
// cli::TranscriptItem 画面。先立账本与归并逻辑,渲染接线随 SessionRuntime
// (第六步)挂上;这保证账本形状从第一天就是"从事件归并",不是从
// AgentLoop 回调直抄——前端可替换性的根就在这。
struct TerminalItemRecord {
    std::string item_id;
    ItemKind kind = ItemKind::Tool;
    std::string tool_name;         // ItemStarted 的 payload.tool_name(没有给空)
    std::string text;              // ItemDelta 累计(正文/思考)
    bool has_outcome = false;
    Outcome outcome = Outcome::Succeeded;
    bool completed = false;        // ItemCompleted 到过
    std::uint64_t last_seq = 0;    // 本条目见过的最大 seq(乱序诊断用)
};

class TerminalEventSink final : public EventSink {
public:
    void Emit(const ServerEvent& event) override {
        std::lock_guard<std::mutex> lock(mutex_);
        switch (event.kind) {
            case ServerEventKind::ItemStarted: {
                TerminalItemRecord record;
                record.item_id = event.item_id;
                record.kind = event.item_kind;
                record.tool_name = event.payload.value("tool_name", std::string());
                record.last_seq = event.envelope.seq;
                records_.push_back(std::move(record));
                break;
            }
            case ServerEventKind::ItemDelta: {
                TerminalItemRecord* record = Find(event.item_id);
                if (record != nullptr) {
                    record->text += event.text;
                    record->last_seq = event.envelope.seq;
                }
                break;
            }
            case ServerEventKind::ItemCompleted: {
                TerminalItemRecord* record = Find(event.item_id);
                if (record != nullptr) {
                    if (event.outcome.has_value()) {
                        record->has_outcome = true;
                        record->outcome = *event.outcome;
                    }
                    record->completed = true;
                    record->last_seq = event.envelope.seq;
                }
                break;
            }
            default:
                break;  // thread/turn 层事件不进条目账;渲染层另有挂点
        }
        ++emitted_;
    }

    // 账本快照(渲染层与单测吃;锁内拷贝)。
    std::vector<TerminalItemRecord> Snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_;
    }

    std::uint64_t emitted() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return emitted_;
    }

private:
    TerminalItemRecord* Find(const std::string& item_id) {
        for (auto& record : records_) {
            if (record.item_id == item_id) {
                return &record;
            }
        }
        return nullptr;  // 迟到/陌生条目:丢弃不误伤(与 ToolDisplay 同规矩)
    }

    mutable std::mutex mutex_;
    std::vector<TerminalItemRecord> records_;
    std::uint64_t emitted_ = 0;
};

}  // namespace lubancode::runtime
