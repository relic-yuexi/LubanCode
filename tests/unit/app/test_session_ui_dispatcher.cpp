// 会话级 UI 调度(按代理状态投影单 P2:收拢写者)的队列语义回归册:
//   1) 次序:FIFO——事件与动作按提交序执行,正文永远先于后提交的控制路;
//   2) 合并:同 item 相邻 ItemDelta 在队尾就地拼接;
//   3) 世代:渲染器按号登记/摘号——摘号后的迟到事件整枚丢弃,不留悬垂;
//   4) 同步口:RunSync 先排干余量再就地执行(调用线程即执行线程),
//      等待中的命令先于 body 落笔;
//   5) 统一提交锁:一切命令执行互斥(探针核并发上限);
//   6) 关账:Stop 排干余量不丢;停表后 PostAction 就地执行。
//
// 屏面本身(事件渲染成什么样)不归这册管——投影册
// (test_terminal_turn_sink_projection)钉。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/session_ui_dispatcher.hpp"
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

// 录音机:记标签与增量文本,数"同时在执行区里的线程数"。
struct Recorder {
    struct Entry {
        std::string tag;
        std::string text;
    };
    std::mutex mutex;
    std::vector<Entry> seen;
    std::atomic<int> in_exec{0};
    std::atomic<int> max_concurrent{0};

    void EnterExec() {
        const int now = ++in_exec;
        int expected = max_concurrent.load();
        while (now > expected && !max_concurrent.compare_exchange_weak(expected, now)) {
        }
    }

    app::SessionUiDispatcher::EventRenderer MakeRenderer() {
        return [this](const runtime::ServerEvent& event) {
            EnterExec();
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (event.kind == runtime::ServerEventKind::ItemDelta) {
                    seen.push_back(Entry{"delta:" + event.item_id, event.text});
                } else {
                    seen.push_back(Entry{"usage", std::string()});
                }
            }
            --in_exec;
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

TEST_CASE("SessionUiDispatcher: FIFO——事件与动作按提交次序执行") {
    Recorder recorder;
    app::SessionUiDispatcher dispatcher;
    const std::uint64_t renderer = dispatcher.AttachRenderer(recorder.MakeRenderer());

    std::vector<std::string> order;
    std::mutex order_mutex;
    (void)dispatcher.PostAction([&] {
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back("a1");
    });
    dispatcher.PostEvent(renderer, MakeDelta("t1", "正文"));
    (void)dispatcher.PostAction([&] {
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back("a2");
    });
    dispatcher.PostEvent(renderer, MakeUsage());
    dispatcher.Quiesce();  // 全部落定再断言

    CHECK(recorder.JoinedText("delta:t1") == "正文");
    CHECK(recorder.CountTag("usage") == 1);
    // usage 是最后提交的事件,动作 a2 在它之前——提交序即执行序。
    REQUIRE(order.size() == 2);
    CHECK(order[0] == "a1");
    CHECK(order[1] == "a2");
    {
        std::lock_guard<std::mutex> lock(recorder.mutex);
        REQUIRE(recorder.seen.size() == 2);
        CHECK(recorder.seen.back().tag == "usage");
    }
    dispatcher.DetachRenderer(renderer);
}

TEST_CASE("SessionUiDispatcher: 同 item 相邻 delta 投递侧合并") {
    Recorder recorder;
    app::SessionUiDispatcher dispatcher;
    const std::uint64_t renderer = dispatcher.AttachRenderer(recorder.MakeRenderer());
    dispatcher.PostEvent(renderer, MakeDelta("t1", "鲁"));
    dispatcher.PostEvent(renderer, MakeDelta("t1", "班"));
    dispatcher.PostEvent(renderer, MakeDelta("t1", "code"));
    dispatcher.PostEvent(renderer, MakeDelta("k1", "想", runtime::ItemKind::Thinking));
    dispatcher.Quiesce();
    CHECK(recorder.JoinedText("delta:t1") == "鲁班code");
    CHECK(recorder.JoinedText("delta:k1") == "想");
    dispatcher.DetachRenderer(renderer);
}

TEST_CASE("SessionUiDispatcher: 渲染世代——摘号后迟到事件整枚丢弃") {
    Recorder recorder;
    app::SessionUiDispatcher dispatcher;
    const std::uint64_t renderer = dispatcher.AttachRenderer(recorder.MakeRenderer());
    dispatcher.PostEvent(renderer, MakeDelta("t1", "活着的回合"));
    dispatcher.Quiesce();
    dispatcher.DetachRenderer(renderer);
    dispatcher.PostEvent(renderer, MakeDelta("t1", "回合已收口的迟到事件"));
    dispatcher.Quiesce();
    CHECK(recorder.JoinedText("delta:t1") == "活着的回合");  // 迟到的丢了,不悬垂
}

TEST_CASE("SessionUiDispatcher: RunSync 排干在前、body 在后,调用线程执行") {
    Recorder recorder;
    app::SessionUiDispatcher dispatcher;
    const std::uint64_t renderer = dispatcher.AttachRenderer(recorder.MakeRenderer());
    dispatcher.PostEvent(renderer, MakeDelta("t1", "先提交的正文"));
    bool body_ran_after = false;
    dispatcher.RunSync([&] {
        // body 跑的时候,先前提交的事件必须已经落笔(RunSync 先排干)。
        std::lock_guard<std::mutex> lock(recorder.mutex);
        for (const Recorder::Entry& entry : recorder.seen) {
            if (entry.tag == "delta:t1") {
                body_ran_after = true;
            }
        }
    });
    CHECK(body_ran_after);
    CHECK(recorder.JoinedText("delta:t1") == "先提交的正文");
    // RunSync 的 body 在调用线程上执行(UI 线程不参与——IsUiThread 为假)。
    CHECK_FALSE(dispatcher.IsUiThread());
    dispatcher.DetachRenderer(renderer);
}

TEST_CASE("SessionUiDispatcher: 统一提交锁——命令执行绝不并发") {
    Recorder recorder;
    app::SessionUiDispatcher dispatcher;
    const std::uint64_t renderer = dispatcher.AttachRenderer(recorder.MakeRenderer());
    // 竞赛双方都过提交锁:UI 线程的事件渲染 vs 调用线程的 RunSync body。
    // 探针(EnterExec)两头都插,谁没被锁住谁就现形。
    std::thread racer([&] {
        for (int i = 0; i < 20; ++i) {
            dispatcher.RunSync([&recorder] {
                recorder.EnterExec();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                --recorder.in_exec;
            });
        }
    });
    for (int i = 0; i < 200; ++i) {
        dispatcher.PostEvent(renderer, MakeDelta("t1", "x"));
    }
    racer.join();
    dispatcher.Quiesce();
    CHECK(recorder.max_concurrent.load() <= 1);
    CHECK(recorder.JoinedText("delta:t1") == std::string(200, 'x'));
    dispatcher.DetachRenderer(renderer);
}

TEST_CASE("SessionUiDispatcher: Stop 排干不丢;停表后 PostAction 就地执行") {
    Recorder recorder;
    app::SessionUiDispatcher dispatcher;
    const std::uint64_t renderer = dispatcher.AttachRenderer(recorder.MakeRenderer());
    std::string expected;
    for (int i = 0; i < 50; ++i) {
        const char c = static_cast<char>('a' + i % 26);
        expected += c;
        dispatcher.PostEvent(renderer, MakeDelta("t1", std::string(1, c)));
    }
    dispatcher.Stop();
    CHECK(recorder.JoinedText("delta:t1") == expected);

    // 停表后的动作:就地执行,future 即成。
    bool ran = false;
    const auto future = dispatcher.PostAction([&] { ran = true; });
    REQUIRE(future.valid());
    future.wait();
    CHECK(ran);
    CHECK(dispatcher.PendingApprox() == 0);
}
