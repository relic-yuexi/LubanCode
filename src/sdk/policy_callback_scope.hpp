#pragma once

#include "sdk/callback_scope.hpp"

namespace lubancore::detail {

inline thread_local bool in_policy_callback = false;
inline bool InPolicyCallback() noexcept { return in_policy_callback; }

// Policy needs the existing SDK blocking barrier, plus a narrow event-wait flag.
// This is deliberately unrelated to authorization's notification_depth: ordinary
// callback retirement must not pretend to be a notification-side unsubscribe.
class PolicyCallbackScope final {
public:
    PolicyCallbackScope() noexcept : previous_(in_policy_callback) { in_policy_callback = true; }
    ~PolicyCallbackScope() { in_policy_callback = previous_; }
    PolicyCallbackScope(const PolicyCallbackScope&) = delete;
    PolicyCallbackScope& operator=(const PolicyCallbackScope&) = delete;
private:
    CallbackScope lifecycle_;
    bool previous_;
};

} // namespace lubancore::detail
