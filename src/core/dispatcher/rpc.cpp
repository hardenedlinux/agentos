/**
 * @Copyright HardenedLinux community
 * @date : 2026
 * @License : GPL-3.0+
 * @author : NalaGinrut@hardenedlinux.org
 * @brief : This file implements JSON-RPC protocol.
 */

#include "agentos/rpc.h"
#include <uuid.h>

namespace agentos
{
  std::string gen_new_uuid ()
  {
    static uuids::uuid_system_generator gen;
    uuids::uuid const id = gen();
    return uuids::to_string(id);
  }
} // namespace agentos
