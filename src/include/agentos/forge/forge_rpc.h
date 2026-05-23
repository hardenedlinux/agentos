#pragma once

#include <string>
#include <functional>

namespace agentos {

class ForgeManager;

class ForgeRpc {
public:
    explicit ForgeRpc(ForgeManager& forgeManager);

    // Handle RPC requests
    std::string handle_request(const std::string& method,
                               const std::string& params_json);

private:
    ForgeManager& forgeManager_;
};

} // namespace agentos


