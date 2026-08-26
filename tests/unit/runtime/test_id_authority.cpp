// 骨架拆解批五先行半批:IdAuthority 的台账域号(前缀发号 + 抬底)。
//
// 固定档(thread/turn/item/req/seq)由显示系统剥离单的测试钉过,这里
// 钉批五新增的前缀档:按前缀独立单调、可抬底(存档回放续号不重号)、
// 与固定档互不串号、进程局唯一。

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "runtime/id_authority.hpp"

using lubancode::runtime::IdAuthority;
using lubancode::runtime::ProcessIdAuthority;

TEST_CASE("前缀发号:loop-N/goal-N 各自单调,互不串号") {
    IdAuthority ids;
    CHECK(ids.NextPrefixedId("loop") == "loop-1");
    CHECK(ids.NextPrefixedId("goal") == "goal-1");
    CHECK(ids.NextPrefixedId("loop") == "loop-2");
    CHECK(ids.NextPrefixedId("loop") == "loop-3");
    CHECK(ids.NextPrefixedId("goal") == "goal-2");
    // 裸数口:前缀各家拼。
    CHECK(ids.NextPrefixedCounter("run") == 1);
    CHECK(ids.NextPrefixedCounter("run") == 2);
}

TEST_CASE("抬底:只抬不降,回放续号不重号") {
    IdAuthority ids;
    CHECK(ids.NextPrefixedId("loop") == "loop-1");
    // 存档回放:档里 loop 任务的 creation_seq 是 2(loop-1 那笔的排序键)。
    // 抬到 2 之后,下一只默认号是 loop-3——与旧 next_seq 口径
    // (next_seq = creation_seq + 1,下一拍 id = loop-<next_seq>)一致。
    ids.SeedPrefixedId("loop", 2);
    CHECK(ids.NextPrefixedId("loop") == "loop-3");
    // 已发到 3,再抬 1(旧档更小):不动。
    ids.SeedPrefixedId("loop", 1);
    CHECK(ids.NextPrefixedId("loop") == "loop-4");
    // 别的前缀不受牵连。
    CHECK(ids.NextPrefixedId("goal") == "goal-1");
}

TEST_CASE("前缀档与固定档互不串号;NextSeq 照旧 1 起") {
    IdAuthority ids;
    CHECK(ids.NextSeq() == 1);
    CHECK(ids.NextSeq() == 2);
    CHECK(ids.NextPrefixedId("loop") == "loop-1");  // 不吃 seq 的号
    CHECK(ids.NextSeq() == 3);
    CHECK(ids.NextItemId() == "item-1");  // 固定档也各走各的
    CHECK(ids.NextPrefixedId("loop") == "loop-2");
}

TEST_CASE("进程局唯一:两次取同一引用") {
    CHECK(&ProcessIdAuthority() == &ProcessIdAuthority());
}

TEST_CASE("并发发号不重号") {
    IdAuthority ids;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 500;
    std::vector<std::thread> workers;
    std::vector<std::vector<std::string>> got(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&ids, &got, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                got[t].push_back(ids.NextPrefixedId("loop"));
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    // 号总数 = 线程数 × 每线程数,且互不重复(1..N 恰好一枚)。
    std::vector<bool> seen(kThreads * kPerThread + 1, false);
    int duplicates = 0;
    for (const auto& thread_got : got) {
        for (const auto& id : thread_got) {
            const int n = std::stoi(id.substr(5));
            if (seen[n]) ++duplicates;
            seen[n] = true;
        }
    }
    CHECK(duplicates == 0);
    for (int n = 1; n <= kThreads * kPerThread; ++n) {
        REQUIRE(seen[n]);
    }
}
