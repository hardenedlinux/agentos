#include "agentos/suite_installer.h"
#include "agentos/database.h"
#include "agentos/home_init.h"
#include "agentos/suite_manager.h"
#include "agentos/time_utils.h"
#include "agentos/types.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <spdlog/spdlog.h>
#include <system_error>

namespace agentos
{

  namespace
  {
    namespace fs = std::filesystem;

    // TODO: implement Marketplace download and signature verification
    static bool stub_download_and_verify (const std::string & /*suite_id*/,
                                          const std::string & /*version*/,
                                          const fs::path &staging)
    {
      std::error_code ec;
      fs::create_directories (staging, ec);
      if (ec)
        return false;
      std::ofstream marker (staging / ".staging_marker");
      marker << "stub\n";
      return true;
    }
  } // anonymous namespace

  SuiteInstaller::SuiteInstaller (Database &db, SuiteManager &manager)
    : db_ (db), suite_mgr_ (manager)
  {
  }

  std::expected<void, Error>
  SuiteInstaller::install (const std::string &suite_id,
                           const std::string &version)
  {
    if (version.empty ())
      return std::unexpected (Error ("version must be specified"));

    auto home = agentos_home ();
    auto staging = home / "suites" / ".staging"
                   / (suite_id + "-" + version);
    auto finaldir = home / "suites" / suite_id;

    if (!stub_download_and_verify (suite_id, version, staging))
      {
        spdlog::error ("[suite_installer] stub download/verify failed for {}",
                       suite_id);
        return std::unexpected (Error ("download/verify failed"));
      }

    // refuse to overwrite existing install
    if (fs::exists (finaldir))
      {
        std::error_code ec;
        fs::remove_all (staging, ec);
        return std::unexpected (Error ("suite already installed"));
      }

    std::error_code ec;
    fs::rename (staging, finaldir, ec);
    if (ec)
      {
        spdlog::error ("[suite_installer] rename staging->final failed: {}",
                       ec.message ());
        fs::remove_all (staging, ec);
        return std::unexpected (Error ("rename failed"));
      }

    const int64_t ts = now_unix ();
    SuitePurchase p;
    p.suite_id = suite_id;
    p.version = version;
    p.subscription_key = ""; // TODO: obtain via Marketplace
    p.purchased_at = ts;
    db_.insert_suite_purchase (p);

    SuiteStatus s;
    s.suite_id = suite_id;
    s.version = version;
    s.available = true;
    s.checked_at = ts;
    db_.upsert_suite_status (s);

    spdlog::info ("[suite_installer] installed suite {} version {}",
                  suite_id, version);
    return {};
  }

  std::expected<void, Error>
  SuiteInstaller::remove (const std::string &suite_id)
  {
    auto home = agentos_home ();
    auto dir = home / "suites" / suite_id;
    if (fs::exists (dir))
      {
        std::error_code ec;
        fs::remove_all (dir, ec);
        if (ec)
          {
            spdlog::error ("[suite_installer] remove directory failed: {}",
                           ec.message ());
            return std::unexpected (Error ("remove directory failed"));
          }
      }

    db_.remove_suite_purchase (suite_id);
    db_.remove_suite_status (suite_id);

    spdlog::info ("[suite_installer] removed suite {}", suite_id);
    return {};
  }

  std::expected<void, Error>
  SuiteInstaller::update (const std::string &suite_id)
  {
    // TODO: implement update flow per ADR-030:
    //   download new version to staging, verify, rename old → .old.<ts>,
    //   rename staging → current, drain in-flight jobs, GC old directory.
    spdlog::info ("[suite_installer] update not yet implemented for suite {}",
                 suite_id);
    return {};
  }

} // namespace agentos
