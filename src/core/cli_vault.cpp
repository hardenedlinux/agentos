#include "agentos/cli_vault.h"
#include "agentos/home_init.h"

#include <iostream>
#include <filesystem>

namespace agentos::cli {

int vault_status(int /*argc*/, char ** /*argv*/)
{
    auto home = agentos_home();
    auto vault_dir = home / "vault";
    bool sealed = std::filesystem::exists(vault_dir / "vault.sealed");
    bool tpm_state = std::filesystem::exists(vault_dir / "tpm.state");

    std::cout << "Vault tier: community\n"
              << "Backend:    software TPM\n"
              << "Sealed:     " << (sealed ? "yes" : "no") << "\n";
    return 0;
}

int vault_rekey(int /*argc*/, char ** /*argv*/)
{
    std::cerr << "vault rekey: not yet implemented\n";
    return 1;
}

} // namespace agentos::cli
