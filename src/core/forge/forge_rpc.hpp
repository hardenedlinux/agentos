#ifndef AGENTOS_FORGE_RPC_HPP
#define AGENTOS_FORGE_RPC_HPP

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

#endif // AGENTOS_FORGE_RPC_HPP
