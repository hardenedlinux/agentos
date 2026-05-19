#ifndef AGENTOS_FORGE_CLI_HPP
#define AGENTOS_FORGE_CLI_HPP

#include <string>
#include <vector>

namespace agentos {

class ForgeCli {
public:
    // Parse and execute CLI commands
    static int execute(int argc, char* argv[]);

private:
    static void print_help();
    static void list_jobs();
    static void show_job(const std::string& id);
    static void approve_job(const std::string& id);
    static void reject_job(const std::string& id, const std::string& reason);
};

} // namespace agentos

#endif // AGENTOS_FORGE_CLI_HPP
