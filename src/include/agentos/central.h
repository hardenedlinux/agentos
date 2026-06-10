// central.h — STUB, pending ADR-024 implementation
#pragma once
#include "agentos/orchestrator.h"

namespace agentos
{

  class Central
  {
  public:
    template <typename ActorType, typename Msg> void send (Msg &&msg);
  };

  // Explicit instantiation declaration — defined in central_stub.cpp until
  // ADR-024
  template <> void Central::send<Orchestrator> (GatewayInbound &&msg);

} // namespace agentos
