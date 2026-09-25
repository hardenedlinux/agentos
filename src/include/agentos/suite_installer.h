#pragma once

#include <expected>
#include <string>

#include "agentos/types.h"

namespace agentos
{
class Database;
class SuiteManager;

class SuiteInstaller
{
public:
    explicit SuiteInstaller (Database &db, SuiteManager &manager);

    std::expected<void, Error> install (const std::string &suite_id,
                                        const std::string &version = "");

    std::expected<void, Error> remove (const std::string &suite_id);

    std::expected<void, Error> update (const std::string &suite_id);

private:
    Database &db_;
    SuiteManager &suite_mgr_;
};

} // namespace agentos
