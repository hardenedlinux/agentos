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
#include "agentos/image_input.h"

#include <fstream>
#include <system_error>

#include <openssl/evp.h>

namespace agentos
{
  std::optional<std::string> sniff_image_media_type (std::string_view b)
  {
    auto starts = [&] (std::string_view sig)
    { return b.size () >= sig.size () && b.substr (0, sig.size ()) == sig; };

    if (starts (std::string_view ("\xFF\xD8\xFF", 3)))
      return "image/jpeg";
    if (starts (std::string_view ("\x89PNG\r\n\x1A\n", 8)))
      return "image/png";
    if (starts ("GIF87a") || starts ("GIF89a"))
      return "image/gif";
    if (b.size () >= 12 && b.substr (0, 4) == "RIFF" && b.substr (8, 4) == "WEBP")
      return "image/webp";
    return std::nullopt;
  }

  std::string base64_encode (std::string_view bytes)
  {
    if (bytes.empty ())
      return {};
    // EVP_EncodeBlock writes 4 bytes per 3 input bytes (rounded up) plus a
    // NUL, and takes an int length; inputs are capped far below INT_MAX by
    // kMaxAdviserImageBytes.
    std::string out (4 * ((bytes.size () + 2) / 3) + 1, '\0');
    const int n = EVP_EncodeBlock (
      reinterpret_cast<unsigned char *> (out.data ()),
      reinterpret_cast<const unsigned char *> (bytes.data ()),
      static_cast<int> (bytes.size ()));
    out.resize (n < 0 ? 0 : static_cast<std::size_t> (n));
    return out;
  }

  Result<LlmImage> load_adviser_image (const std::filesystem::path &path,
                                       std::size_t max_bytes)
  {
    std::error_code ec;
    const auto size = std::filesystem::file_size (path, ec);
    if (ec)
      return {"image not readable", ErrorTag{}};
    if (size == 0)
      return {"image is empty", ErrorTag{}};
    if (size > max_bytes)
      return {"image is " + std::to_string (size) + " bytes, limit is "
                + std::to_string (max_bytes),
              ErrorTag{}};

    std::ifstream f (path, std::ios::binary);
    if (!f)
      return {"image not readable", ErrorTag{}};
    std::string bytes (static_cast<std::size_t> (size), '\0');
    if (!f.read (bytes.data (), static_cast<std::streamsize> (bytes.size ())))
      return {"image not readable", ErrorTag{}};

    auto media_type = sniff_image_media_type (bytes);
    if (!media_type)
      return {"unsupported image format (JPEG, PNG, GIF or WebP required)",
              ErrorTag{}};

    LlmImage img;
    img.media_type = std::move (*media_type);
    img.base64 = base64_encode (bytes);
    return img;
  }

} // namespace agentos
