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

#include <cerrno>
#include <cstddef>
#include <unistd.h>

namespace agentos
{

  // Write exactly len bytes to fd, retrying on EINTR and partial writes.
  // On unrecoverable error (n < 0 && errno != EINTR), returns false and
  // stops — caller decides how to report. Async-signal-safe.
  inline bool write_full (int fd, const char *buf, size_t len)
  {
    while (len > 0)
    {
      ssize_t n = write (fd, buf, len);
      if (n < 0)
      {
        if (errno == EINTR)
          continue;
        return false;
      }
      buf += static_cast<size_t> (n);
      len -= static_cast<size_t> (n);
    }
    return true;
  }

} // namespace agentos
