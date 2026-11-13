#pragma once

#include "lubancore/api.hpp"

namespace lubancore::detail {

// All SDK user-code entry points share this boundary, including observer threads
// and capture destruction. Blocking lifecycle reentry must fail before locking.
extern thread_local bool in_session_worker;

// Keep entry/exit in the SDK module. Private adapter copies in a host test
// executable must use the SDK's TLS, not bind an executable-side TLS wrapper.
class LUBANCORE_API CallbackScope final {
public:
    CallbackScope() noexcept;
    ~CallbackScope() noexcept;
    CallbackScope(const CallbackScope&) = delete;
    CallbackScope& operator=(const CallbackScope&) = delete;
private:
    bool previous_;
};

} // namespace lubancore::detail
