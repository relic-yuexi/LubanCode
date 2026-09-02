// 版本号单一出处:PrintVersion/--help/横幅、WebFetch 的 User-Agent、
// 更新检查都用这一份,别处不许再抄写一遍字符串字面量。

#pragma once

#include <string_view>

namespace lubancode::app {

inline constexpr std::string_view kVersion = "0.26.165";

}  // namespace lubancode::app
