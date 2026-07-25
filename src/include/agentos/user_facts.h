#pragma once

#include <optional>
#include <string_view>

namespace agentos {

enum class FactType {
    CategoryInterest,
    CardReaction,
    RiskPreference,
    MarketRegion,
};

struct FactTypeInfo {
    const char* name;
    bool        decayed;
};

inline constexpr FactTypeInfo kFactTypes[] = {
    { "category_interest", true  },
    { "card_reaction",     false },
    { "risk_preference",   true  },
    { "market_region",     true  },
};

std::optional<FactTypeInfo> lookup_fact_type(std::string_view name);

}  // namespace agentos
