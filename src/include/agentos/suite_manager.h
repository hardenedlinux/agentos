#pragma once

#include <expected>
#include <string>
#include <vector>

#include "agentos/types.h"

namespace agentos
{
class Database;

class SuiteManager
{
public:
    explicit SuiteManager (Database &db);

    std::expected<std::vector<std::string>, Error>
    get_pipeline_docs (const std::string &goal);

    std::expected<std::string, Error>
    resolve_ref (const std::string &ref, const std::string &version = "");

    void poll_availability ();

private:
    Database &db_;
};

} // namespace agentos
