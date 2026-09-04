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

// ---------------------------------------------------------------------------
// 按帧落屏(终端画面隔网单·条 2/5):帧节拍收批。一帧间隔内的增量并在队尾,
// 到点一口气收——滴流(delta 每 10ms 一枚)下渲染批次被压到"每帧一批",
// 远少于逐枚直写;帧间隔 0 是直写老轨,枚枚即画。两条轨的全文都一字不丢。
// ---------------------------------------------------------------------------

TEST_CASE("UiEventPump: 按帧节拍收批——滴流下渲染批次远少于 delta 枚数") {
    Recorder recorder;
    constexpr int kFrameMs = 50;
    app::UiEventPump pump(recorder.MakeRenderer(), std::chrono::milliseconds(kFrameMs));
    // 期望全文:30 枚"字"(UTF-8 三字节,不能用 std::string(n,'字')——
    // 多字节字符字面量截成 char 会变垃圾)。
    std::string expected;
    const auto drip_start = std::chrono::steady_clock::now();
    for (int i = 0; i < 30; ++i) {
        pump.PostDelta(MakeDelta("t1", "字"));
        expected += "字";
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto drip_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - drip_start)
                             .count();
    pump.StopAndDrain();
    const std::size_t batches = recorder.CountTag("delta:t1");
    CHECK(batches >= 2);
    // 上界随实测滴流时长推导,不拍常数:批数被帧节拍钉住,约等于滴流时长
    // /帧间隔——首枚即画 +1、停表尾批 +1、调度抖动 +2,故 +4。名义滴流
    // 300ms ≈ 5~8 批;慢 runner(CI macOS 实翻 23 批,run 33873402016)上
    // sleep_for 超睡、滴流拉到 ~1.1s,帧节拍照常工作,批数随帧数正比涨
    // ——恒定上界 20 冤枉的是正常节拍。若帧节流真失效(短时长高批数),
    // 此界照样抓红。下界"不止一批"才是本断言的钉;全文一字不丢另钉。
    const long long frame_budget = drip_ms / kFrameMs + 4;
    CHECK(static_cast<long long>(batches) <= frame_budget);
    CHECK(recorder.JoinedText("delta:t1") == expected);
}

TEST_CASE("UiEventPump: 帧间隔 0 是直写老轨——投递即醒,枚枚即画") {
    Recorder recorder;
    app::UiEventPump pump(recorder.MakeRenderer(), std::chrono::milliseconds(0));
    std::string expected;
    for (int i = 0; i < 30; ++i) {
        pump.PostDelta(MakeDelta("t1", "字"));
        expected += "字";
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    pump.StopAndDrain();
    // 滴流下每枚 delta 都该有自己的批次(合并在队尾也只并掉同拍到达的,
    // 10ms 间隔远大于线程切换,30 枚基本是 30 批)。下界放掉几拍调度抖动。
    const std::size_t batches = recorder.CountTag("delta:t1");
    CHECK(batches >= 20);
    CHECK(recorder.JoinedText("delta:t1") == expected);
}

TEST_CASE("UiEventPump: 按帧等待中控制路不等帧——DispatchInline 就地排干") {
    Recorder recorder;
    constexpr int kFrameMs = 500;  // 长帧:消费线程多半正睡在帧边界上
    app::UiEventPump pump(recorder.MakeRenderer(), std::chrono::milliseconds(kFrameMs));
    pump.PostDelta(MakeDelta("t1", "正文一枚"));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    // 消费线程首枚即画(首帧不等待),随后睡在长帧里;此刻控制事件必须
    // 就地画掉、把 pending 排干,不许等那 500ms 的帧。
    const auto t0 = std::chrono::steady_clock::now();
    pump.DispatchInline(MakeUsage());
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    CHECK(elapsed < 400);
    REQUIRE(recorder.CountTag("usage") == 1);
    CHECK(recorder.seen.back().tag == "usage");
    pump.StopAndDrain();
    CHECK(recorder.JoinedText("delta:t1") == "正文一枚");
}
