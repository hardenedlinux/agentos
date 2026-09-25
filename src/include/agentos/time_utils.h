#pragma once
#include <chrono>
#include <cstdint>

namespace agentos {
inline int64_t now_unix() {
    using namespace std::chrono;
    return static_cast<int64_t>(
        duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}
} // namespace agentos
