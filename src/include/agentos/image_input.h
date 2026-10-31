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
/**
 * image_input.h -- one image attached to an Adviser-target step
 * (ADR-031 §10, ADR-017 "Image input").
 */

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "agentos/llm_client.h"
#include "agentos/types.h"

namespace agentos
{
  /// Largest image file the daemon will attach. The provider limit for an
  /// inline image is higher (DeepSeek: 32 MiB per image, 48 MiB per request
  /// body); base64 inflates by 4/3, so 20 MiB keeps the body well below it.
  inline constexpr std::size_t kMaxAdviserImageBytes = 20u * 1024 * 1024;

  /// Media type from the leading bytes (magic number), never from a file
  /// name or a declared type. Returns nullopt for anything that is not
  /// JPEG, PNG, GIF or WebP.
  std::optional<std::string> sniff_image_media_type (std::string_view bytes);

  /// Standard base64 (RFC 4648, with padding, no line breaks).
  std::string base64_encode (std::string_view bytes);

  /// Read, check and encode the image at `path`. Fails if the file cannot
  /// be read, is empty, exceeds `max_bytes`, or is not a supported format.
  /// Error messages name the reason and size only, never file content.
  Result<LlmImage> load_adviser_image (const std::filesystem::path &path,
                                       std::size_t max_bytes
                                       = kMaxAdviserImageBytes);

} // namespace agentos
