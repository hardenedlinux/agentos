#pragma once

#include <rapidjson/document.h>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace agentos {

struct ScoreUpdateInput {
    std::optional<double> old_score;
    double                signal;
};

using ScoreUpdateFn = double (*)(const ScoreUpdateInput& input,
                                 const rapidjson::Value& params);

class MemoryCurveRegistry {
public:
    static MemoryCurveRegistry& instance();

    void register_algorithm(std::string_view name, ScoreUpdateFn fn);
    std::optional<ScoreUpdateFn> resolve(std::string_view name) const;

private:
    std::unordered_map<std::string, ScoreUpdateFn> algorithms_;
};

// Registers every built-in memory-curve algorithm compiled into this
// binary. Must be called exactly once, explicitly, before anything calls
// MemoryCurveRegistry::instance().resolve(...) — the daemon calls this
// during startup, before config is loaded/validated; any test that
// exercises the registry directly must call it too (safe to call more
// than once — see the .cpp for why).
//
// Deliberately NOT self-registering static-initializer objects. This
// codebase links algorithms into a STATIC library (agentos_core), and a
// translation unit whose only effect is a global constructor's side
// effect is not guaranteed to be pulled into the final binary by the
// linker if nothing else references a symbol from it — a linker may
// silently drop the whole .o, and the algorithm never gets registered.
// This actually happened once already (see memory-curve-algorithm.md).
// Explicit registration means every algorithm function is called by name
// from here, so the linker has no choice but to include it — there is no
// "plugin" behavior here, everything is compiled in; this function is
// just where "which ones exist" is spelled out once, by hand.
void register_builtin_memory_curve_algorithms();

// The algorithm functions themselves. Each one lives in its own .cpp
// file (see memory-curve-algorithm.md's "adding a new algorithm"
// checklist) and is declared here, by name, so
// register_builtin_memory_curve_algorithms() can reference it directly.
double ema_update(const ScoreUpdateInput& input, const rapidjson::Value& params);

}  // namespace agentos
