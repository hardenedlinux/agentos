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
    uuids::uuid const id = uuids::uuid_system_generator{}();
    return uuids::to_string (id);
  }
} // namespace agentos
