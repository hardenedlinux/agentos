#pragma once

#include <rapidjson/document.h>
#include <functional>
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

}  // namespace agentos
