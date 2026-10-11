#pragma once

#include <cstddef>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::api {

// nlohmann's nonempty DOM destructor allocates a traversal stack. Keep all
// containers registered while SAX builds the document, and empty children
// before parents, including during exception unwinding. Borrow the root only;
// copying or mutating it would escape this cleanup ownership.
class ParsedJson final {
    using Json = nlohmann::json;
    static constexpr std::size_t none = (std::numeric_limits<std::size_t>::max)();
    struct Entry {
        Json::object_t* object = nullptr;
        Json::array_t* array = nullptr;
        std::size_t parent = none;
        std::size_t first_child = none;
        std::size_t next_sibling = none;
        bool alive = false;
    };
    struct Frame {
        std::size_t entry;
        Json::string_t key;
    };

public:
    ParsedJson() = default;
    ParsedJson(const ParsedJson&) = delete;
    ParsedJson& operator=(const ParsedJson&) = delete;
    ParsedJson(ParsedJson&&) = delete;
    ParsedJson& operator=(ParsedJson&&) = delete;
    ~ParsedJson() noexcept { ClearAll(); }

    const Json& value() const noexcept { return root_; }

    void Parse(std::string_view input) {
        ClearAll();
        root_ = nullptr;
        entries_.clear();
        frames_.clear();
        try {
            // Only text JSON is accepted. Match json::parse's strict trailing
            // input and comment handling; parser errors retain concrete types.
            Json::sax_parse(input.begin(), input.end(), this,
                Json::input_format_t::json, true, false);
        } catch (...) {
            ClearAll();
            throw;
        }
    }

    // Public SAX callbacks are an internal parser protocol, not an SDK API.
    bool null() { Attach(nullptr); return true; }
    bool boolean(bool value) { Attach(value); return true; }
    bool number_integer(Json::number_integer_t value) { Attach(value); return true; }
    bool number_unsigned(Json::number_unsigned_t value) { Attach(value); return true; }
    bool number_float(Json::number_float_t value, const Json::string_t&) {
        Attach(value); return true;
    }
    bool string(Json::string_t& value) { Attach(std::move(value)); return true; }
    bool binary(Json::binary_t& value) {
        Attach(Json::binary(std::move(value))); return true;
    }
    bool start_object(std::size_t) { Start(true); return true; }
    bool start_array(std::size_t) { Start(false); return true; }
    bool key(Json::string_t& value) {
        frames_.back().key = std::move(value);
        return true;
    }
    bool end_object() { frames_.pop_back(); return true; }
    bool end_array() { frames_.pop_back(); return true; }
    template<class Exception>
    bool parse_error(std::size_t, const std::string&, const Exception& error) {
        throw error;
    }

private:
    static const void* Storage(const Json& value) noexcept {
        if (value.is_object()) return value.get_ptr<const Json::object_t*>();
        if (value.is_array()) return value.get_ptr<const Json::array_t*>();
        return nullptr;
    }

    Json& Attach(Json value) {
        if (frames_.empty()) {
            root_ = std::move(value);
            return root_;
        }
        const auto& frame = frames_.back();
        auto& parent = entries_[frame.entry];
        if (parent.array) {
            parent.array->push_back(std::move(value));
            return parent.array->back();
        }
        auto existing = parent.object->find(frame.key);
        if (existing != parent.object->end()) {
            if (const auto* storage = Storage(existing->second)) {
                // A duplicate key replaces a complete old subtree. Empty it
                // before assignment and invalidate its ledger entries first.
                ClearSubtree(locations_.find(storage)->second);
            }
            existing->second = std::move(value);
            return existing->second;
        }
        return parent.object->emplace(frame.key, std::move(value)).first->second;
    }

    void Start(bool object) {
        // Reserve the cleanup record before constructing/attaching even an
        // empty container. Subsequent allocations always see a registered DOM.
        const auto index = entries_.size();
        entries_.push_back(Entry{});
        auto& value = Attach(object ? Json::object() : Json::array());
        auto& entry = entries_[index];
        entry.object = value.get_ptr<Json::object_t*>();
        entry.array = value.get_ptr<Json::array_t*>();
        entry.parent = frames_.empty() ? none : frames_.back().entry;
        if (entry.parent != none) {
            auto& parent = entries_[entry.parent];
            entry.next_sibling = parent.first_child;
            parent.first_child = index;
        }
        entry.alive = true;
        locations_.emplace(Storage(value), index);
        frames_.push_back(Frame{index, {}});
    }

    void ClearEntry(std::size_t index) noexcept {
        auto& entry = entries_[index];
        if (!entry.alive) return;
        // All structured children have already been emptied. Their normal
        // destructors now reserve zero slots; strings/scalars only free memory.
        const void* storage = entry.object ? static_cast<const void*>(entry.object)
                                           : static_cast<const void*>(entry.array);
        if (entry.object) entry.object->clear();
        else entry.array->clear();
        locations_.erase(storage);
        entry.alive = false;
    }

    void ClearSubtree(std::size_t root) noexcept {
        auto index = root;
        for (;;) {
            auto& entry = entries_[index];
            while (entry.first_child != none && !entries_[entry.first_child].alive)
                entry.first_child = entries_[entry.first_child].next_sibling;
            if (entry.first_child != none) {
                index = entry.first_child;
                continue;
            }
            const auto parent = entry.parent;
            ClearEntry(index);
            if (index == root) return;
            index = parent;
        }
    }

    void ClearAll() noexcept {
        for (auto index = entries_.size(); index != 0; --index) ClearEntry(index - 1);
    }

    Json root_;
    std::vector<Entry> entries_;
    std::map<const void*, std::size_t, std::less<const void*>> locations_;
    std::vector<Frame> frames_;
};

}  // namespace lubancode::api
