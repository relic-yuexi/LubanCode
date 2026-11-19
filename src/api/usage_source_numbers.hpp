#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>

namespace lubancode::api::usage_wire {

// An accounting-only view of a borrowed frame. Paths are canonical literals,
// never body pointers or allocated strings. Invalid and missing stay distinct.
struct SourceScalar {
    std::string_view path;
    std::optional<std::int64_t> integer;
    bool object = false;
    bool is_object() const noexcept { return object; }
};

class SourceNumbers {
public:
    bool is_object() const noexcept { return object_; }
    void Scope(std::string_view path, bool object) noexcept {
        if (path.find('.') == std::string_view::npos) {
            count_ = 0; object_ = object;
            return;
        }
        for (std::size_t i = 0; i < count_;) {
            if (scalars_[i].path.starts_with(path) && scalars_[i].path.size() > path.size() &&
                scalars_[i].path[path.size()] == '.') scalars_[i] = scalars_[--count_];
            else ++i;
        }
        Set(path, {}, object);
    }
    void Set(std::string_view path, std::optional<std::int64_t> integer, bool object = false) noexcept {
        for (std::size_t i = 0; i < count_; ++i)
            if (scalars_[i].path == path) { scalars_[i] = {path, integer, object}; return; }
        if (count_ < scalars_.size()) scalars_[count_++] = {path, integer, object};
    }
    const SourceScalar* Find(std::initializer_list<const char*> keys) const noexcept {
        if (!object_) return nullptr;
        for (std::size_t i = 0; i < count_; ++i) {
            auto suffix = scalars_[i].path;
            suffix.remove_prefix(suffix.find('.') + 1);
            bool match = true;
            std::size_t n = 0;
            for (const char* key : keys) {
                const std::string_view part(key);
                if (!suffix.starts_with(part)) { match = false; break; }
                suffix.remove_prefix(part.size());
                if (++n < keys.size()) {
                    if (suffix.empty() || suffix.front() != '.') { match = false; break; }
                    suffix.remove_prefix(1);
                }
            }
            if (match && suffix.empty()) return &scalars_[i];
        }
        return nullptr;
    }
private:
    std::array<SourceScalar,64> scalars_{};
    std::size_t count_ = 0;
    bool object_ = false;
};

inline const SourceScalar* Find(const SourceNumbers& source,
    std::initializer_list<const char*> keys) noexcept { return source.Find(keys); }

} // namespace lubancode::api::usage_wire
