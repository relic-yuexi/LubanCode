// Private native creation hook; never installed or exported by SDK.
#pragma once
#include <functional>
namespace lubancode::app::testing {
std::function<void()> ExchangeTitleWorkerStartHook(std::function<void()> hook);
}
