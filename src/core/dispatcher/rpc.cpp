/**
 * @Copyright HardenedLinux community
 * @date : 2026
 * @License : GPL-3.0+
 * @author : NalaGinrut@hardenedlinux.org
 * @brief : This file implements JSON-RPC protocol.
 */

#include "agentos/rpc.h"
#include <array>
#include <iomanip>
#include <random>
#include <sstream>

namespace agentos
{
  std::string gen_new_uuid ()
  {
    static std::random_device rd;
    static std::mt19937 gen (rd ());
    static std::uniform_int_distribution<uint8_t> dist (0, 255);

    std::array<uint8_t, 16> bytes;
    for (auto &b : bytes)
      b = dist (gen);

    // Set version 4 (bits 12-15 of byte 6)
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    // Set variant (bits 6-7 of byte 8)
    bytes[8] = (bytes[8] & 0x3f) | 0x80;

    std::ostringstream oss;
    oss << std::hex << std::setfill ('0');
    for (int i = 0; i < 16; ++i)
      {
        if (i == 4 || i == 6 || i == 8 || i == 10)
          oss << '-';
        oss << std::setw (2) << static_cast<int> (bytes[i]);
      }
    return oss.str ();
  }
} // namespace agentos
