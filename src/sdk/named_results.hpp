#pragma once

#include <memory>
#include <optional>

#include <lubancore/named_results.hpp>
#include "trajectory/named_result_blobs.hpp"

namespace lubancore::detail {
Result<std::shared_ptr<lubancode::trajectory::NamedResultFactory>> MakeNamedResultFactory(
    std::optional<named_results::v1::Options> options);
} // namespace lubancore::detail
