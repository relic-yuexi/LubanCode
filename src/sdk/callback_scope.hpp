#pragma once

namespace lubancore::detail {

// All SDK user-code entry points share this boundary, including observer threads
// and capture destruction. Blocking lifecycle reentry must fail before locking.
extern thread_local bool in_session_worker;

class CallbackScope final {
public:
    CallbackScope() noexcept : previous_(in_session_worker) { in_session_worker = true; }
    ~CallbackScope() { in_session_worker = previous_; }
    CallbackScope(const CallbackScope&) = delete;
    CallbackScope& operator=(const CallbackScope&) = delete;
private:
    bool previous_;
};

} // namespace lubancore::detail
