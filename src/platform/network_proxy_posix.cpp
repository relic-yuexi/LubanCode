#include "platform/network_proxy.hpp"

namespace lubancode::platform {

std::optional<std::string> SystemProxyForScheme(std::string_view) {
    return std::nullopt;
}

}  // namespace lubancode::platform
