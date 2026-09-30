/**
 * Copyright (C) 2026  HardenedLinux community
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

// Access keys (ADR-020 as amended by ADR-041).
//
// A raw key is 32 random bytes as 64 lowercase hex characters, displayed
// with an "ak_" prefix; the prefix is optional on the wire. Every key is
// looked up by key_digest = SHA-256(raw key). User-bound keys are stored
// hash-only (empty plaintext column).

#include "agentos/database.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace agentos
{
  inline constexpr std::string_view kAccessKeyDisplayPrefix = "ak_";

  // SHA-256(raw) as 64 lowercase hex characters -- the lookup column.
  std::string access_key_digest (std::string_view raw);

  // SHA-256(raw || salt) as 64 lowercase hex characters (ADR-020 key_hash).
  std::string access_key_salted_hash (std::string_view raw,
                                      std::string_view salt);

  // Strip an optional "ak_" display prefix. Returns nullopt unless what is
  // left is exactly 64 lowercase hex characters.
  std::optional<std::string> normalize_access_key (std::string_view presented);

  // admin | operator | readonly | service
  bool access_key_role_valid (std::string_view role);

  struct IssuedAccessKey
  {
    Database::AccessKey record; // ready for Database::insert_access_key
    std::string raw;            // shown once to the holder, never stored
                                // for a user-bound key
  };

  // Generate a new key. A key bound to a user must have role "service"
  // (ADR-041 §2); it is stored hash-only. Returns nullopt on an invalid
  // role/binding combination or if the system RNG fails.
  std::optional<IssuedAccessKey>
  make_access_key (const std::string &role, const std::string &description,
                   const std::optional<std::string> &user_id,
                   std::optional<int64_t> expires_at = std::nullopt);

} // namespace agentos
