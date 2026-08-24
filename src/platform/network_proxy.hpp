#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace lubancode::platform {

// Windows 的 ProxyServer 既可写成一枚通用地址，也可写成
// "http=host:port;https=host:port"。拆成 libcurl 能直接吃的 URI。
std::optional<std::string> ParseWindowsProxyServer(std::string_view value,
                                                   std::string_view scheme);

// 环境变量中的代理交给 libcurl 自己处理；只有相关变量全没设置时，才取
// Windows 当前用户的手动系统代理。别的平台返回 nullopt。
std::optional<std::string> SystemProxyForScheme(std::string_view scheme);

}  // namespace lubancode::platform
