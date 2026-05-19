/**
 * @Copyright HardenedLinux community
 * @date : 2026
 * @License : GPL-3.0+
 * @author : NalaGinrut@hardenedlinux.org
 * @brief : This file implements JSON-RPC protocol.
 */

#include "agentos/rpc.h"
#include <random>
#include <sstream>
#include <iomanip>

namespace agentos
{
  std::string gen_new_uuid ()
  {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dist;

    uint64_t hi = dist(gen);
    uint64_t lo = dist(gen);

    // Set version 4 (random) and variant bits
    hi &= 0xFFFFFFFFFFFF0FFFULL; // clear version bits
    hi |= 0x0000000000004000ULL; // set version 4
    lo &= 0x3FFFFFFFFFFFFFFFULL; // clear variant bits
    lo |= 0x8000000000000000ULL; // set variant 1

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    oss << std::setw(8)  << (hi >> 32)
        << "-"
        << std::setw(4)  << ((hi >> 16) & 0xFFFF)
        << "-"
        << std::setw(4)  << (hi & 0xFFFF)
        << "-"
        << std::setw(4)  << ((lo >> 48) & 0xFFFF)
        << "-"
        << std::setw(12) << (lo & 0xFFFFFFFFFFFFULL);
    return oss.str();
  }
} // namespace agentos
