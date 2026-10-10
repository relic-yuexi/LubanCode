// 宿主兼容入口；落账实现归 runtime，CLI 与 SDK 共用。
#pragma once

#include "runtime/memory_ledger_bridge.hpp"

namespace lubancode::app {
using runtime::MemoryLedgerBridge;
}  // namespace lubancode::app
