#pragma once
#include "agentos/llm_proxy.h"
#include <string>

namespace agentos::forge
{
  std::string code_reviewer (const std::string &input_json, LlmProxy &proxy);
} // namespace agentos::forge
