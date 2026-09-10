// 新会话 v3 开关实现。接线点清单见 session_switch.hpp 头注。
#include "trajectory/v3/session_switch.hpp"

#include "platform/paths.hpp"

namespace lubancode::trajectory::v3 {

bool NewSessionV3WriteEnabled() {
    auto value = platform::GetEnvVar("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS");
    return value.has_value() && *value == "1";
}

}  // namespace lubancode::trajectory::v3
