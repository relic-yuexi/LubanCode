#pragma once

#include <memory>
#include <nlohmann/json.hpp>

namespace lubancode::worker_host {
// Private supervisor IPC adapter. No runtime, tool or trajectory internals are
// part of this boundary: execution belongs to the installed public SDK.
class Host {
public:
    Host();
    ~Host();
    nlohmann::json Handle(const nlohmann::json& request);
    bool stopping() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace lubancode::worker_host
