#include "agentos/memory_curve.h"

#include <cassert>
#include <spdlog/spdlog.h>
#include <string>

namespace agentos {

MemoryCurveRegistry& MemoryCurveRegistry::instance() {
    static MemoryCurveRegistry reg;
    return reg;
}

void MemoryCurveRegistry::register_algorithm(std::string_view name,
                                             ScoreUpdateFn fn) {
    // Two algorithms fighting over the same name is a real bug
    auto [it, inserted] = algorithms_.try_emplace(std::string{name}, fn);
    if (!inserted) {
        spdlog::error("[MemoryCurveRegistry] duplicate algorithm name \"{}\" "
                       "— refusing to overwrite",
                       name);
        std::abort();   // force developers to notice the clash at static-init time
    }
}

std::optional<ScoreUpdateFn>
MemoryCurveRegistry::resolve(std::string_view name) const {
    auto it = algorithms_.find(std::string{name});
    if (it == algorithms_.end())
        return std::nullopt;
    return it->second;
}

}  // namespace agentos
