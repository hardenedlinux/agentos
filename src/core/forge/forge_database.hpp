#ifndef AGENTOS_FORGE_DATABASE_HPP
#define AGENTOS_FORGE_DATABASE_HPP

#include <string>
#include <vector>
#include <optional>
#include "forge_job.hpp"

namespace agentos {

class Database; // forward declaration

class ForgeDatabase {
public:
    explicit ForgeDatabase(Database& db);

    void create_tables();

    void insert_job(const ForgeJob& job);
    void update_job(const ForgeJob& job);
    std::optional<ForgeJob> get_job(const std::string& id);
    std::vector<ForgeJob> get_jobs_by_phase(const std::string& phase);
    std::vector<ForgeJob> get_all_jobs();

private:
    Database& db_;
};

} // namespace agentos

#endif // AGENTOS_FORGE_DATABASE_HPP
