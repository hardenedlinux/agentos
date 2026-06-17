#pragma once

#include "agentos/types.h"
#include "agentos/database.h"
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agentos {

struct UserRecord {
    std::string  id;
    bool         enabled = true;
    int64_t      created_at = 0;
};

struct UserProfile {
    std::string              user_id;
    int64_t                  first_seen = 0;
    std::optional<int64_t>   last_seen;
    int                      total_jobs = 0;
    int                      successful_jobs = 0;
    int                      failed_jobs = 0;
    std::vector<std::string> connected_providers;
};

class UserManager {
public:
    explicit UserManager(Database& db);

    std::expected<UserRecord, Error> register_user(std::string_view user_id);

    std::expected<std::vector<UserRecord>, Error>
    list_users(std::optional<bool> enabled_filter, int limit, int offset);

    std::expected<int, Error> count_users(std::optional<bool> enabled_filter);

    std::expected<void, Error> enable_user(std::string_view user_id);
    std::expected<void, Error> disable_user(std::string_view user_id);

    std::expected<UserProfile, Error> get_profile(std::string_view user_id);

    /// Returns success if the user is known and enabled, otherwise an Error.
    /// The ADR requires that the same error code be returned for both
    /// unknown and disabled users.
    std::expected<void, Error> validate_user(std::string_view user_id);

private:
    Database& db_;
};

} // namespace agentos
