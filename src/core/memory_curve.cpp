#include "agentos/memory_curve.h"

#include <cstdlib>
#include <spdlog/spdlog.h>
#include <string>

namespace agentos {

MemoryCurveRegistry& MemoryCurveRegistry::instance() {
    static MemoryCurveRegistry reg;
    return reg;
}

void MemoryCurveRegistry::register_algorithm(std::string_view name,
                                             ScoreUpdateFn fn) {
    // Two algorithms fighting over the same name is a real bug.
    auto [it, inserted] = algorithms_.try_emplace(std::string{name}, fn);
    if (!inserted) {
        spdlog::error("[MemoryCurveRegistry] duplicate algorithm name \"{}\" "
                       "— refusing to overwrite",
                       name);
        std::abort();   // force developers to notice the clash immediately
    }
}

std::optional<ScoreUpdateFn>
MemoryCurveRegistry::resolve(std::string_view name) const {
    auto it = algorithms_.find(std::string{name});
    if (it == algorithms_.end())
        return std::nullopt;
    return it->second;
}

void register_builtin_memory_curve_algorithms() {
    // Guarded so this is safe to call more than once in the same process
    // (e.g. from more than one test fixture, or if a future call site
    // ends up calling it twice by accident) — without this guard, a
    // second call would hit register_algorithm's duplicate-name abort()
    // even though it's the exact same (name, fn) pair, which would be a
    // confusing way to fail for something harmless.
    static bool done = false;
    if (done) return;
    done = true;

    MemoryCurveRegistry::instance().register_algorithm("ema", &ema_update);
    // Add one line per new algorithm here. See memory-curve-algorithm.md
    // for the full "adding a new algorithm" checklist — this is the only
    // step that touches this file.
}

}  // namespace agentos
