// Private native creation hook; never installed or exported by SDK.
#pragma once
#include <functional>
namespace lubancode::app::testing {
std::function<void()> ExchangeTitleWorkerStartHook(std::function<void()> hook);
enum class TitleWorkerPhase { BeforeWork, AfterRefinement, BeforePublish };
using TitleWorkerHook = std::function<void(TitleWorkerPhase)>;
TitleWorkerHook ExchangeTitleWorkerExecutionHook(TitleWorkerHook hook);
}
