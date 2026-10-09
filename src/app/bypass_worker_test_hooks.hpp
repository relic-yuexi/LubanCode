#pragma once
#include <functional>
namespace lubancode::app::testing {
enum class BypassWorkerPurpose { Memory, Title };
enum class BypassWorkerPhase { BeforeBinding, BeforeSampling };
using BypassWorkerHook = std::function<void(BypassWorkerPurpose, BypassWorkerPhase)>;
BypassWorkerHook ExchangeBypassWorkerHook(BypassWorkerHook hook);
BypassWorkerHook SnapshotBypassWorkerHook();
}  // namespace lubancode::app::testing
