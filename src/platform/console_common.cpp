#include "platform/console.hpp"

namespace lubancode::platform {

std::recursive_timed_mutex& ConsoleInputMutex() {
    static std::recursive_timed_mutex mutex;
    return mutex;
}

}  // namespace lubancode::platform
