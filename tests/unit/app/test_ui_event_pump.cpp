// UiEventPump 的队列语义回归(终端画面隔网先行批):
//  1) 合并:同 item 的相邻 delta 在队尾就地拼接——队列里只滚一枚,
//     text 逐字节齐(消费线程若中途收走一部分,收走的与剩下的各自成段,
//     全文拼接仍是原文);
//  2) 次序:控制路事件(DispatchInline)画前排干 pending 的流式事件,
//     正文永远先于 usage/工具卡;
//  3) 关账:StopAndDrain 之后一个 delta 不丢、恰好画一遍;停表后
//     PostDelta 退化成就地画(Stop 钩子续跑的迟到流式段);
//  4) 画笔锁:渲染闭包绝不并发(消费线程与就地路互斥)。
// 画面本身(直写闭包画成什么样)不归这册管——agent_stream_driver 与
// 各 cli 单测钉着。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/ui_event_pump.hpp"
#include "runtime/event.hpp"

using namespace lubancode;

namespace {

runtime::ServerEvent MakeDelta(const std::string& item_id, const std::string& text,
                               runtime::ItemKind kind = runtime::ItemKind::Text) {
    runtime::ServerEvent event;
    event.kind = runtime::ServerEventKind::ItemDelta;
    event.item_id = item_id;
    event.item_kind = kind;
    event.text = text;
    return event;
}

runtime::ServerEvent MakeUsage() {
    runtime::ServerEvent event;
    event.kind = runtime::ServerEventKind::UsageUpdated;
    return event;
}

// 收批侧的录音机:按到达次序记事件标签(delta 记 item+text,控制事件记
// kind),并数"同时在渲染区里的线程数"(画笔锁探测器)。
struct Recorder {
    struct Entry {
        std::string tag;   // "delta:<item_id>" / "usage"
        std::string text;  // delta 的增量
    };
    std::mutex mutex;
    std::vector<Entry> seen;
    std::atomic<int> in_render{0};
    std::atomic<int> max_concurrent{0};

    void EnterRender() {
        const int now = ++in_render;
        int expected = max_concurrent.load();
        while (now > expected && !max_concurrent.compare_exchange_weak(expected, now)) {
        }
    }

    app::UiEventPump::Renderer MakeRenderer(bool slow = false) {
        return [this, slow](const runtime::ServerEvent& event) {
            EnterRender();
            if (slow) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (event.kind == runtime::ServerEventKind::ItemDelta) {
                    seen.push_back(Entry{"delta:" + event.item_id, event.text});
                } else {
                    seen.push_back(Entry{"usage", std::string()});
                }
            }
            --in_render;
        };
    }

    std::string JoinedText(const std::string& tag_prefix) const {
        std::string joined;
        for (const Entry& entry : seen) {
            if (entry.tag.rfind(tag_prefix, 0) == 0) {
                joined += entry.text;
            }
        }
        return joined;
    }

    std::size_t CountTag(const std::string& tag) const {
        std::size_t count = 0;
        for (const Entry& entry : seen) {
            if (entry.tag == tag) {
                ++count;
            }
        }
        return count;
    }
};

}  // namespace

TEST_CASE("UiEventPump: 同 item 相邻 delta 投递侧合并,text 逐字节齐") {
    Recorder recorder;
    app::UiEventPump pump(recorder.MakeRenderer());
    pump.PostDelta(MakeDelta("t1", "鲁"));
    pump.PostDelta(MakeDelta("t1", "班"));
    pump.PostDelta(MakeDelta("t1", "code"));
    // 思考是另一枚 item,不并进正文的滚动 delta。
    pump.PostDelta(MakeDelta("k1", "想", runtime::ItemKind::Thinking));
    pump.StopAndDrain();

    CHECK(recorder.JoinedText("delta:t1") == "鲁班code");
    CHECK(recorder.JoinedText("delta:k1") == "想");
}

TEST_CASE("UiEventPump: 控制路画前排干,正文先于 usage") {
    Recorder recorder;
    app::UiEventPump pump(recorder.MakeRenderer());
    pump.PostDelta(MakeDelta("t1", "正文第一段"));
    pump.PostDelta(MakeDelta("t1", "还没画完的半句"));
    // 网络路投完,产生事件的线程立刻就地处理一枚控制事件:此刻队列里的
    // 正文必须先落,usage 垫后(次序与老路一致——正文先于工具卡/统计)。
    pump.DispatchInline(MakeUsage());
    pump.StopAndDrain();

    REQUIRE(recorder.CountTag("usage") == 1);
    CHECK(recorder.seen.back().tag == "usage");
    CHECK(recorder.JoinedText("delta:t1") == "正文第一段还没画完的半句");
}

TEST_CASE("UiEventPump: StopAndDrain 排干不丢、恰好一遍;停表后 PostDelta 就地画") {
    Recorder recorder;
    app::UiEventPump pump(recorder.MakeRenderer());
    std::string expected;
    for (int i = 0; i < 50; ++i) {
        const char c = static_cast<char>('a' + i % 26);
        expected += c;
        pump.PostDelta(MakeDelta("t1", std::string(1, c)));
    }
    pump.StopAndDrain();
    CHECK(recorder.JoinedText("delta:t1") == expected);

    // 停表后的迟到流式段(Stop 钩子续跑):就地画,不进队列、不丢。
    pump.PostDelta(MakeDelta("t1", "迟到段"));
    CHECK(recorder.JoinedText("delta:t1") == expected + "迟到段");
}

TEST_CASE("UiEventPump: 画笔锁串行化,消费线程与就地路绝不并发渲染") {
    Recorder recorder;
    app::UiEventPump pump(recorder.MakeRenderer(/*slow=*/true));
    std::thread inline_racer([&pump] {
        for (int i = 0; i < 20; ++i) {
            pump.DispatchInline(MakeUsage());
        }
    });
    for (int i = 0; i < 200; ++i) {
        pump.PostDelta(MakeDelta("t1", "x"));
    }
    inline_racer.join();
    pump.StopAndDrain();
    CHECK(recorder.max_concurrent.load() <= 1);
    CHECK(recorder.JoinedText("delta:t1") == std::string(200, 'x'));
}
