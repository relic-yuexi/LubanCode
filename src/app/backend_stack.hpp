// 旧宿主 include 的兼容入口，后端工厂与稳定引用壳只在中立层实现。
#pragma once

#include "runtime/assembly/backend.hpp"

namespace lubancode::app {

using runtime::assembly::BuildBackend;
using runtime::assembly::RebuildableBackend;

}  // namespace lubancode::app
