#pragma once
/**
 * agentos/strong_id.h
 *
 * StrongId<Tag> – phantom type for type‑safe IDs (ADR‑010).
 */

#include <string>
#include <functional>
#include <ostream>

namespace agentos {

template<typename Tag>
class StrongId {
public:
    StrongId() = default;
    explicit StrongId(std::string value) : value_(std::move(value)) {}
    // Allow implicit construction from const char* for convenience
    StrongId(const char* s) : value_(s) {}

    const std::string& value() const noexcept { return value_; }

    // Provide c_str() for C‑style string access
    const char* c_str() const noexcept { return value_.c_str(); }

    // Assignment operators
    StrongId& operator=(const std::string& s) {
        value_ = s;
        return *this;
    }
    StrongId& operator=(const char* s) {
        value_ = s;
        return *this;
    }

    bool operator==(const StrongId& other) const noexcept { return value_ == other.value_; }
    bool operator!=(const StrongId& other) const noexcept { return value_ != other.value_; }
    bool operator<(const StrongId& other) const noexcept { return value_ < other.value_; }

    // Comparison with std::string and const char*
    bool operator==(const std::string& s) const noexcept { return value_ == s; }
    bool operator!=(const std::string& s) const noexcept { return value_ != s; }
    bool operator==(const char* s) const noexcept { return value_ == s; }
    bool operator!=(const char* s) const noexcept { return value_ != s; }

    // Implicit conversion to std::string_view for formatting
    operator std::string_view() const noexcept { return value_; }

    // Stream output
    friend std::ostream& operator<<(std::ostream& os, const StrongId& id) {
        os << id.value_;
        return os;
    }

private:
    std::string value_;
};

// Concatenation helpers for building paths
template<typename Tag>
std::string operator+(const std::string& lhs, const StrongId<Tag>& rhs) {
    return lhs + rhs.value();
}
template<typename Tag>
std::string operator+(const StrongId<Tag>& lhs, const std::string& rhs) {
    return lhs.value() + rhs;
}
template<typename Tag>
std::string operator+(const StrongId<Tag>& lhs, const char* rhs) {
    return lhs.value() + rhs;
}
template<typename Tag>
std::string operator+(const char* lhs, const StrongId<Tag>& rhs) {
    return std::string(lhs) + rhs.value();
}

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
