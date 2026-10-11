#pragma once

#include "api/usage_wire_builder.hpp"
#include "api/usage_source_numbers.hpp"

namespace lubancode::api::usage_wire {

using NumericValues = std::array<std::int64_t, facts::kFieldCount>;
using NumericObserver = void (*)(void*, const NumericValues&) noexcept;

// Run the same provider normalization before any observation storage exists.
// Paths borrow provider literals; integer evidence borrows the parsed frame.
// Material admission cannot erase a scalar already owned by the request.
class NumericBuilder {
public:
    using Result = NumericValues;
    explicit NumericBuilder(std::string_view,
        const std::vector<facts::RawField>* lexical = nullptr) noexcept : lexical_(lexical) {}

    RawIndex Capture(std::string_view path, const nlohmann::json* value) noexcept {
        if (!value) return std::nullopt;
        for (std::uint16_t i = 0; i < count_; ++i)
            if (raws_[i].path == path) return i;
        if (count_ == raws_.size()) return std::nullopt;
        const auto index = count_++;
        raws_[index] = {path, IntegerScalar(*value, lexical_, path)};
        return index;
    }
    RawIndex Capture(std::string_view path, const SourceScalar* value) noexcept {
        if (!value) return std::nullopt;
        for (std::uint16_t i = 0; i < count_; ++i)
            if (raws_[i].path == path) return i;
        if (count_ == raws_.size()) return std::nullopt;
        const auto index = count_++;
        raws_[index] = {path, value->integer};
        return index;
    }
    std::optional<std::int64_t> Integer(RawIndex raw) const noexcept {
        return raw ? raws_[*raw].integer : std::nullopt;
    }
    void Report(facts::Field target, RawIndex raw) noexcept { Put(target, Integer(raw)); }
    RawIndex Select(facts::Field, RawIndex canonical, RawIndex alias) noexcept {
        return canonical ? canonical : alias;
    }
    void BlockedDetails(facts::Field target, RawIndex, RawIndex = {},
        facts::Presence = facts::Presence::Missing) noexcept { Put(target, std::nullopt); }
    void Subtract(facts::Field target, RawIndex first, RawIndex second,
        facts::Presence = facts::Presence::Present,
        facts::Origin = facts::Origin::Normalized) noexcept { Calculate(target, first, second, false); }
    void Add(facts::Field target, RawIndex first, RawIndex second,
        facts::Presence = facts::Presence::Present,
        facts::Origin = facts::Origin::Normalized) noexcept { Calculate(target, first, second, true); }
    void OrdinaryInput(RawIndex total, RawIndex read, RawIndex write,
        facts::Presence = facts::Presence::Present,
        facts::Origin = facts::Origin::Normalized) noexcept {
        auto result = Integer(total);
        if ((read && !Integer(read)) || (write && !Integer(write))) result.reset();
        if (result && read) result = usage_observation::CheckedSubtract(*result, *Integer(read));
        if (result && write) result = usage_observation::CheckedSubtract(*result, *Integer(write));
        Put(facts::Field::Input, result);
    }
    void CheckSum(facts::Field, RawIndex, RawIndex, RawIndex) noexcept {}
    void Note(facts::Field, facts::AnomalyCode, std::string_view,
        std::initializer_list<RawIndex>) noexcept {}
    Result Finish() && noexcept { return values_; }
private:
    struct Scalar { std::string_view path; std::optional<std::int64_t> integer; };
    void Put(facts::Field field, std::optional<std::int64_t> value) noexcept {
        values_[static_cast<std::size_t>(field)] = value.value_or(0);
    }
    void Calculate(facts::Field field, RawIndex first, RawIndex second, bool add) noexcept {
        const auto a = Integer(first), b = Integer(second);
        Put(field, a && b ? (add ? usage_observation::CheckedAdd(*a, *b)
            : usage_observation::CheckedSubtract(*a, *b)) : std::nullopt);
    }
    std::array<Scalar, facts::kMaxRawFields> raws_{};
    std::uint16_t count_ = 0;
    NumericValues values_{};
    const std::vector<facts::RawField>* lexical_;
};

} // namespace lubancode::api::usage_wire
