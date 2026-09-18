// 只读工具并行与写入串行单 P2:批次调度核心的实现(合同见头文件)。

#include "agent/tool_batch_schedule.hpp"

#include <algorithm>
#include <atomic>
#include <thread>
#include <utility>

namespace lubancode::agent {

std::optional<ToolBatchStrategy> ParseToolBatchStrategy(const std::string& text) {
    if (text == "exclusive") {
        return ToolBatchStrategy::Exclusive;
    }
    if (text == "parallel_read") {
        return ToolBatchStrategy::ParallelRead;
    }
    return std::nullopt;
}

std::string_view ToolBatchStrategyName(ToolBatchStrategy strategy) {
    switch (strategy) {
        case ToolBatchStrategy::Exclusive:
            return "exclusive";
        case ToolBatchStrategy::ParallelRead:
            return "parallel_read";
    }
    return "exclusive";
}

int ClampParallelReadConcurrency(int configured) {
    if (configured < kMinParallelReadConcurrency) {
        return kMinParallelReadConcurrency;
    }
    if (configured > kMaxParallelReadConcurrency) {
        return kMaxParallelReadConcurrency;
    }
    return configured;
}

bool IsParallelReadAllowlistedBuiltin(const tools::ToolRegistry& registry, const std::string& tool_name) {
    if (tool_name != "read_file" && tool_name != "search") {
        return false;  // 不按名字含 read/get 放行(§三);web_fetch/web_search 首版保守串行
    }
    const tools::Tool* tool = registry.Find(tool_name);
    if (tool == nullptr) {
        return false;
    }
    // 注册来源必须是内置件:插件/Lua/MCP 不许借内置名影子放行;需要确认
    // 的调用也走独占——审批流(异步 future/前端确认)的既有语义不动。
    const tools::ToolRegistration* registration = registry.RegistrationOf(tool_name);
    if (registration == nullptr || registration->source_kind != tools::ToolSourceKind::Builtin) {
        return false;
    }
    return !tool->needs_confirm();
}

ParallelReadEligibility ProbeParallelReadEligibility(const tools::ToolRegistry& registry,
                                                     const tools::DeferredToolResolver* resolver,
                                                     const api::ToolUseBlock& wire_call) {
    ParallelReadEligibility probe;
    if (wire_call.name != "tool_invoke") {
        if (!IsParallelReadAllowlistedBuiltin(registry, wire_call.name)) {
            return probe;
        }
        probe.eligible = true;
        probe.resolved_call = wire_call;
        return probe;
    }
    // tool_invoke(§三):先解析真实目标再判策略;解析失败按独占收口,
    // 拒绝文案与稳定码由串行路的原错误路径出,这里不抢先。
    if (resolver == nullptr) {
        return probe;
    }
    const auto resolved = resolver->Resolve(registry, wire_call);
    if (!resolved.has_value()) {
        return probe;
    }
    if (!IsParallelReadAllowlistedBuiltin(registry, resolved->target_name)) {
        return probe;
    }
    probe.eligible = true;
    probe.via_proxy = true;
    probe.resolved_call = wire_call;  // id 沿用 wire 那枚 tool_invoke
    probe.resolved_call.name = resolved->target_name;
    probe.resolved_call.input = resolved->arguments;
    probe.proxy.transport_name = wire_call.name;
    probe.proxy.tool_ref = resolved->tool_ref;
    probe.proxy.schema_digest = resolved->schema_digest;
    return probe;
}

std::vector<ToolBatchSegment> PlanToolBatchSegments(const std::vector<char>& eligible) {
    std::vector<ToolBatchSegment> segments;
    std::size_t i = 0;
    while (i < eligible.size()) {
        if (eligible[i]) {
            const std::size_t begin = i;
            while (i < eligible.size() && eligible[i]) {
                ++i;
            }
            segments.push_back(ToolBatchSegment{begin, i, true});
            continue;
        }
        segments.push_back(ToolBatchSegment{i, i + 1, false});
        ++i;
    }
    return segments;
}

BoundedParallelExecutor::RunOutcome BoundedParallelExecutor::RunAll(
    int limit, std::vector<std::function<tools::Tool::Result()>> tasks) {
    RunOutcome outcome;
    const std::size_t count = tasks.size();
    outcome.results.resize(count);
    outcome.failures.resize(count);
    if (count == 0) {
        return outcome;
    }
    const std::size_t worker_count =
        std::max<std::size_t>(1, std::min<std::size_t>(static_cast<std::size_t>(limit < 1 ? 1 : limit), count));

    // 抢下标派活:worker 从原子计数领下一枚任务,先到先得;同段任务不
    // 保证启动序,结果一律按任务下标回槽(完成先后不改排列)。
    std::atomic<std::size_t> next_index{0};
    std::atomic<std::size_t> active{0};
    std::atomic<std::size_t> peak{0};

    const auto worker_main = [&] {
        while (true) {
            const std::size_t index = next_index.fetch_add(1);
            if (index >= count) {
                return;
            }
            std::size_t now_active = active.fetch_add(1) + 1;
            // 峰值只增不减:每枚"加完计数"的现值都有一枚 worker 亲眼见过,
            // 取它们的最广值恰是真实在跑峰值(CAS 抢位更新)。
            std::size_t observed = peak.load(std::memory_order_relaxed);
            while (now_active > observed &&
                   !peak.compare_exchange_weak(observed, now_active, std::memory_order_relaxed)) {
            }
            try {
                outcome.results[index] = tasks[index]();
            } catch (...) {
                // worker 上不许传播异常(那会 terminate):存指针交回主线程
                // 折算,该枚任务照常有终态,不吞、不冒充成功。
                outcome.failures[index] = std::current_exception();
            }
            active.fetch_sub(1);
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    // 起线程半路抛异常(bad_alloc 一类)也得先 join 已起的:worker 循环
    // 自己会耗尽下标退出,不会悬空;join 完再原样传播。
    try {
        for (std::size_t w = 0; w < worker_count; ++w) {
            workers.emplace_back(worker_main);
        }
    } catch (...) {
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }
    for (auto& worker : workers) {
        worker.join();
    }
    outcome.peak_concurrency = peak.load(std::memory_order_relaxed);
    return outcome;
}

}  // namespace lubancode::agent
