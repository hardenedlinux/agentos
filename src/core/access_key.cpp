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

#include "agentos/access_key.h"

#include <chrono>
#include <openssl/evp.h>
#include <sys/random.h>

namespace agentos
{
  namespace
  {
    std::string to_hex (const unsigned char *p, size_t n)
    {
      static constexpr char kHex[] = "0123456789abcdef";
      std::string out;
      out.reserve (n * 2);
      for (size_t i = 0; i < n; ++i)
      {
        out.push_back (kHex[p[i] >> 4]);
        out.push_back (kHex[p[i] & 0x0f]);
      }
      return out;
    }

    std::string sha256_hex (std::string_view a, std::string_view b)
    {
      unsigned char md[EVP_MAX_MD_SIZE];
      unsigned int len = 0;
      EVP_MD_CTX *ctx = EVP_MD_CTX_new ();
      EVP_DigestInit_ex (ctx, EVP_sha256 (), nullptr);
      EVP_DigestUpdate (ctx, a.data (), a.size ());
      EVP_DigestUpdate (ctx, b.data (), b.size ());
      EVP_DigestFinal_ex (ctx, md, &len);
      EVP_MD_CTX_free (ctx);
      return to_hex (md, len);
    }

    std::optional<std::string> random_hex (size_t bytes)
    {
      std::string buf (bytes, '\0');
      size_t got = 0;
      while (got < bytes)
      {
        const ssize_t r = getrandom (buf.data () + got, bytes - got, 0);
        if (r <= 0)
          return std::nullopt;
        got += static_cast<size_t> (r);
      }
      return to_hex (reinterpret_cast<const unsigned char *> (buf.data ()),
                     bytes);
    }
  } // namespace

  std::string access_key_digest (std::string_view raw)
  {
    return sha256_hex (raw, {});
  }

  std::string access_key_salted_hash (std::string_view raw,
                                      std::string_view salt)
  {
    return sha256_hex (raw, salt);
  }

  std::optional<std::string> normalize_access_key (std::string_view presented)
  {
    if (presented.starts_with (kAccessKeyDisplayPrefix))
      presented.remove_prefix (kAccessKeyDisplayPrefix.size ());
    if (presented.size () != 64)
      return std::nullopt;
    for (char c : presented)
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        return std::nullopt;
    return std::string (presented);
  }

  bool access_key_role_valid (std::string_view role)
  {
    return role == "admin" || role == "operator" || role == "readonly"
           || role == "service";
  }

  std::optional<IssuedAccessKey>
  make_access_key (const std::string &role, const std::string &description,
                   const std::optional<std::string> &user_id,
                   std::optional<int64_t> expires_at)
  {
    if (!access_key_role_valid (role))
      return std::nullopt;
    if (user_id && (role != "service" || user_id->empty ()))
      return std::nullopt;

    auto raw = random_hex (32);
    auto salt = random_hex (16);
    if (!raw || !salt)
      return std::nullopt;

    IssuedAccessKey out;
    out.raw = *raw;
    auto &k = out.record;
    k.key_hash = access_key_salted_hash (*raw, *salt);
    k.key_salt = *salt;
    k.key_digest = access_key_digest (*raw);
    // User-bound keys can number one per end user: 64 bits of id keep
    // collisions out of reach. Unbound keys keep the short ADR-020 id.
    k.id = k.key_hash.substr (0, user_id ? 16 : 8);
    k.key = user_id ? std::string{} : *raw; // ADR-041 §3: hash-only if bound
    k.description = description;
    k.role = role;
    k.user_id = user_id;
    k.created_at = std::chrono::duration_cast<std::chrono::seconds> (
                     std::chrono::system_clock::now ().time_since_epoch ())
                     .count ();
    k.expires_at = expires_at;
    return out;
  }

} // namespace agentos
