#include "agentos/user_facts.h"

namespace agentos {

std::optional<FactTypeInfo> lookup_fact_type(std::string_view name) {
    for (const auto& ft : kFactTypes) {
        if (name == ft.name)
            return ft;
    }
    return std::nullopt;
}

}  // namespace agentos
