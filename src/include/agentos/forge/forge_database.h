#pragma once

#include <string>
#include <vector>
#include <optional>
#include "forge_job.h"

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

