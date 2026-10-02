#pragma once

#include <atomic>
#include <expected>
#include <string>

#if defined(_WIN32)
#  if defined(LUBANCORE_BUILDING)
#    define LUBANCORE_API __declspec(dllexport)
#  else
#    define LUBANCORE_API __declspec(dllimport)
#  endif
#else
#  define LUBANCORE_API __attribute__((visibility("default")))
#endif

// Experimental C++23 API. Consumer and library must use compatible compilers,
// standard libraries and (on Windows) CRTs. No stable cross-toolchain ABI.
namespace lubancore {

struct Error { std::string code; std::string message; };
template<class T> using Result = std::expected<T, Error>;

struct Cancellation {
    // Borrowed only until the current backend, tool or extension invocation ends.
    const std::atomic<bool>* flag = nullptr;
    bool requested() const noexcept { return flag && flag->load(); }
};

} // namespace lubancore
