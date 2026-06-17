#include "agentos/suite_manager.h"
#include "agentos/database.h"
#include "agentos/home_init.h"
#include "agentos/time_utils.h"
#include "agentos/types.h"
#include <fstream>
#include <spdlog/spdlog.h>

namespace agentos
{

  SuiteManager::SuiteManager (Database &db) : db_ (db) {}

  std::expected<std::vector<std::string>, Error>
  SuiteManager::get_pipeline_docs (const std::string &goal)
  {
    // TODO: filter installed suites by capability matching against `goal`.
    // Phase 1: return pipeline.md from all available suites unconditionally.
    std::vector<std::string> docs;
    auto statuses = db_.load_all_suite_status ();
    for (const auto &s : statuses)
    {
      auto suite_dir
        = agentos_home () / "suites" / s.suite_id / "pipeline.md";
      std::ifstream in (suite_dir);
      if (!in)
        continue;
      std::string content ((std::istreambuf_iterator<char> (in)),
                           std::istreambuf_iterator<char> ());
      if (!content.empty ())
        docs.push_back (std::move (content));
    }
    return docs;
  }

  std::expected<std::string, Error>
  SuiteManager::resolve_ref (const std::string &ref,
                             const std::string &version)
  {
    auto maybe = db_.resolve_agent_binary (ref, version);
    if (maybe)
      return *maybe;

    spdlog::warn ("[suite_manager] resolve_ref: ref '{}' not found", ref);
    return std::unexpected (
      Error ("Capability unavailable: ref not found (code "
             + std::to_string (suite_error::capability_unavailable) + ")"));
  }

  void SuiteManager::poll_availability ()
  {
    // TODO: implement Marketplace availability query (ADR-030)
    // For now, mark all suites temporarily unavailable
    auto statuses = db_.load_all_suite_status ();
    const auto now = now_unix ();
    for (const auto &s : statuses)
    {
      db_.update_suite_availability (s.suite_id, false, now);
    }
    spdlog::info ("[suite_manager] poll_availability: {} suites marked "
                  "unavailable (stub)",
                  statuses.size ());
  }

} // namespace agentos
