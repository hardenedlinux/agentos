#pragma once
/**
 * agentos/strong_id.hpp
 *
 * StrongId<Tag> – phantom type for type‑safe IDs (ADR‑010).
 */

#include <string>
#include <functional>

namespace agentos {

template<typename Tag>
class StrongId {
public:
    StrongId() = default;
    explicit StrongId(std::string value) : value_(std::move(value)) {}

    const std::string& value() const noexcept { return value_; }

    bool operator==(const StrongId& other) const noexcept { return value_ == other.value_; }
    bool operator!=(const StrongId& other) const noexcept { return value_ != other.value_; }
    bool operator<(const StrongId& other) const noexcept { return value_ < other.value_; }

    // Implicit conversion to std::string_view for formatting
    operator std::string_view() const noexcept { return value_; }

private:
    std::string value_;
};

} // namespace agentos

// Hash support for unordered containers
namespace std {
template<typename Tag>
struct hash<agentos::StrongId<Tag>> {
    size_t operator()(const agentos::StrongId<Tag>& id) const noexcept {
        return hash<std::string>{}(id.value());
    }
};
} // namespace std
