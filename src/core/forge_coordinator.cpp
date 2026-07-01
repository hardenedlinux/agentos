/**
 * agentos/forge_coordinator.cpp
 *
 * ADR-019: Forge pipeline state machine implementation.
 * ADR-022: ForgeCoordinator as internal sub-component of Orchestrator.
 * ADR-009: Enforce Layer capability policy applied at every decision point.
 */

#include "agentos/forge_coordinator.h"
#include "agentos/code_reviewer.h"
#include "agentos/code_writer.h"
#include "agentos/home_init.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace
{
  // Generate a UUID using the kernel's random UUID source.
  // Linux-only; suitable for single-machine deployments (ADR-001).
  std::string generate_uuid ()
  {
    std::ifstream f ("/proc/sys/kernel/random/uuid");
    std::string uuid;
    std::getline (f, uuid);
    return uuid;
  }
} // anonymous namespace

namespace agentos::forge
{

  // ---------------------------------------------------------------------------
  // Construction / destruction
  // ---------------------------------------------------------------------------

  ForgeCoordinator::ForgeCoordinator (Database &db, LlmProxy &llm,
                                      Registry &registry,
                                      CompletionCallback on_complete)
    : db_ (db), llm_ (llm), registry_ (registry),
      on_complete_ (std::move (on_complete))
  {
  }

  ForgeCoordinator::~ForgeCoordinator ()
  {
    stop ();
  }

  // ---------------------------------------------------------------------------
  // Lifecycle
  // ---------------------------------------------------------------------------

  void ForgeCoordinator::start ()
  {
    thread_ = std::thread ([this] { run (); });
  }

  void ForgeCoordinator::stop ()
  {
    {
      std::lock_guard<std::mutex> lk (queue_mutex_);
      stopping_ = true;
    }
    queue_cv_.notify_all ();
    if (thread_.joinable ())
      thread_.join ();
  }

  void ForgeCoordinator::post (ForgeRequest req)
  {
    {
      std::lock_guard<std::mutex> lk (queue_mutex_);
      queue_.push (std::move (req));
    }
    queue_cv_.notify_one ();
  }

  // ---------------------------------------------------------------------------
  // Background thread
  // ---------------------------------------------------------------------------

  void ForgeCoordinator::run ()
  {
    while (true)
    {
      ForgeRequest req;
      {
        std::unique_lock<std::mutex> lk (queue_mutex_);
        queue_cv_.wait (lk, [this] { return stopping_ || !queue_.empty (); });
        if (stopping_ && queue_.empty ())
          return;
        req = std::move (queue_.front ());
        queue_.pop ();
      }
      process (std::move (req));
    }
  }

  // ---------------------------------------------------------------------------
  // Core state machine driver
  // ---------------------------------------------------------------------------

  void ForgeCoordinator::process (ForgeRequest req)
  {
    // Load the persisted job record — it was created by Orchestrator before
    // post().
    auto opt = db_.load_forge_pipeline_job (req.forge_job_id);
    if (!opt)
    {
      spdlog::error ("[forge_coordinator] forge job {} not found in DB",
                     req.forge_job_id);
      on_complete_ (ForgeResult{req.forge_job_id,
                                req.task_id,
                                ForgeResult::Outcome::failed,
                                {},
                                {},
                                "forge job record missing from DB"});
      return;
    }

    ForgePipelineJob job = std::move (*opt);

    // step_id is the pipeline tasks row to update with token usage.
    // task_id on the job is job_id (used for resume); step_id is separate.
    const std::string step_id = req.step_id;

    // Accumulate LLM token usage across all attempts for this Forge job.
    // Written to the associated pipeline step on successful promotion.
    int forge_tokens_prompt = 0;
    int forge_tokens_completion = 0;

    // Outer retry loop — each iteration is one full attempt.
    while (true)
    {
      job.attempt++;
      job.status = ForgeStatus::drafting;
      persist (job);

      spdlog::info ("[forge_coordinator] job {} attempt {}/{}", job.id,
                    job.attempt, job.max_attempts);

      // --- Code Writer ---
      int w_pt = 0, w_ct = 0;
      if (!call_code_writer (job, w_pt, w_ct))
      {
        // Hard error (LLM failure, bad JSON) — count as a failed attempt.
        spdlog::warn (
          "[forge_coordinator] code_writer hard error on attempt {}",
          job.attempt);
        // fall through to retry / escalate logic below
      }
      else
      {
        // Enforce Layer check on Writer output (ADR-009).
        if (!enforce_capability_policy (job))
        {
          spdlog::info ("[forge_coordinator] capability policy rejected writer "
                        "output on attempt {}",
                        job.attempt);
          // job.feedback already set by enforce_capability_policy
        }
        else if (!syntax_precheck (job))
        {
          // Syntax pre-check failed — job.feedback already set.
          // Accumulate writer tokens before retry.
          forge_tokens_prompt += w_pt;
          forge_tokens_completion += w_ct;
          spdlog::info (
            "[forge_coordinator] syntax pre-check failed on attempt {}, "
            "skipping reviewer",
            job.attempt);
        }
        else
        {
          // --- Code Reviewer ---
          job.status = ForgeStatus::reviewing;
          persist (job);

          int r_pt = 0, r_ct = 0;
          bool accepted = call_code_reviewer (job, r_pt, r_ct);
          forge_tokens_prompt += w_pt + r_pt;
          forge_tokens_completion += w_ct + r_ct;

          // Enforce Layer check again on Reviewer-accepted output (ADR-009).
          // Even a Reviewer accept cannot bypass Enforce Layer.
          if (accepted && !enforce_capability_policy (job))
          {
            spdlog::warn ("[forge_coordinator] enforce overriding reviewer "
                          "accept on attempt {}",
                          job.attempt);
            accepted = false;
            // job.feedback set by enforce_capability_policy
          }

          if (accepted)
          {
            // Final Enforce Layer check before promotion (ADR-009).
            // Third gate: once promoted, the worker is trusted for reuse.
            if (!enforce_capability_policy (job))
            {
              spdlog::warn ("[forge_coordinator] enforce blocked promotion "
                            "on attempt {}",
                            job.attempt);
              accepted = false;
            }
          }

          if (accepted)
          {
            // ADR-031 Section 7: method name is controlled by promote_worker
            // which reads it directly from requirement_json.method — the LLM
            // has no input on the method name, so no consistency check against
            // writer output is needed here.
            auto worker_id = promote_worker (job);
            if (worker_id)
            {
              job.status = ForgeStatus::promoted;
              persist (job);
              spdlog::info ("[forge_coordinator] job {} promoted → worker {}",
                            job.id, *worker_id);
              // Record cumulative Forge token usage on the pipeline step.
              // Write Forge token usage to the pipeline step row.
              // Use step_id (not job.task_id which is the job_id).
              const std::string &token_target
                = step_id.empty () ? job.task_id : step_id;
              if (forge_tokens_prompt > 0 || forge_tokens_completion > 0)
                db_.update_step_tokens (token_target,
                                        forge_tokens_prompt,
                                        forge_tokens_completion);

              on_complete_ (ForgeResult{job.id,
                                        job.task_id,
                                        ForgeResult::Outcome::promoted,
                                        *worker_id,
                                        {},
                                        {}});
              return;
            }
            else
            {
              // promote_worker failure is a hard error — escalate.
              spdlog::error ("[forge_coordinator] promote_worker failed for "
                             "job {}",
                             job.id);
              std::string review_id = escalate_to_human (
                job, "worker promotion failed (filesystem or DB error)");
              job.status = ForgeStatus::human_review;
              persist (job);
              on_complete_ (ForgeResult{job.id,
                                        job.task_id,
                                        ForgeResult::Outcome::human_review,
                                        {},
                                        review_id,
                                        {}});
              return;
            }
          }
        }
      }

      // Attempt failed (policy rejection or reviewer rejection).
      if (job.attempt >= job.max_attempts)
      {
        spdlog::info ("[forge_coordinator] job {} exhausted {} attempts, "
                      "escalating",
                      job.id, job.max_attempts);
        std::string review_id
          = escalate_to_human (job, "max_attempts reached: " + job.feedback);
        job.status = ForgeStatus::human_review;
        persist (job);
        on_complete_ (ForgeResult{job.id,
                                  job.task_id,
                                  ForgeResult::Outcome::human_review,
                                  {},
                                  review_id,
                                  {}});
        return;
      }

      // Retry: feedback is already in job.feedback from the failed step.
      spdlog::info ("[forge_coordinator] job {} retrying with feedback: {}",
                    job.id, job.feedback);
    }
  }

  // ---------------------------------------------------------------------------
  // Code Writer
  // ---------------------------------------------------------------------------

  bool ForgeCoordinator::call_code_writer (ForgePipelineJob &job,
                                             int &tokens_prompt,
                                             int &tokens_completion)
  {
    // Fail fast on missing API key — this is a configuration error,
    // not a transient failure. Retrying will not help (ADR-009 Enforce Layer).
    const char *api_key_env = std::getenv ("AGENTOS_ADVISER_API_KEY");
    if (!api_key_env || std::strlen (api_key_env) == 0)
    {
      spdlog::error ("[forge_coordinator] AGENTOS_ADVISER_API_KEY is not set "
                     "— Code Writer cannot call LLM. Set the environment "
                     "variable and restart the daemon.");
      job.feedback = "configuration error: AGENTOS_ADVISER_API_KEY not set";
      // Mark as terminal so process() escalates immediately without retrying.
      job.attempt = job.max_attempts;
      return false;
    }

    // Build input JSON per ADR-019 Code Writer contract.
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartObject ();
    w.Key ("task_id");
    w.String (job.task_id.c_str ());
    w.Key ("forge_job_id");
    w.String (job.id.c_str ());
    w.Key ("requirement");
    // requirement_json is a JSON object — embed as raw value.
    w.RawValue (job.requirement_json.c_str (), job.requirement_json.size (),
                rapidjson::kObjectType);
    w.Key ("feedback");
    w.String (job.feedback.c_str ());
    w.EndObject ();

    const std::string input = buf.GetString ();
    const std::string output = code_writer (input, llm_);

    // Validate response structure.
    rapidjson::Document doc;
    if (doc.Parse (output.c_str ()).HasParseError ())
    {
      spdlog::error ("[forge_coordinator] code_writer returned invalid JSON: {}",
                     output.substr (0, 200));
      job.feedback = "code_writer returned invalid JSON";
      return false;
    }

    // If code_writer itself returned an error envelope, surface the real reason.
    if (doc.HasMember ("error") && doc["error"].IsString ())
    {
      const std::string reason = doc["error"].GetString ();
      spdlog::error ("[forge_coordinator] code_writer reported error: {}",
                     reason);
      job.feedback = "code_writer error: " + reason;
      return false;
    }

    // Validate required business fields (ADR-031: impl_code replaces code).
    // task_id is NOT expected from LLM output.
    for (const char *field : {"language", "entry_point", "impl_code",
                               "signatures", "capability"})
    {
      if (!doc.HasMember (field))
      {
        spdlog::error ("[forge_coordinator] code_writer missing field '{}'",
                       field);
        job.feedback
          = std::string ("code_writer response missing field: ") + field;
        return false;
      }
    }

    // signatures must be an object
    if (!doc["signatures"].IsObject ())
    {
      spdlog::error ("[forge_coordinator] code_writer 'signatures' is not an object");
      job.feedback = "code_writer response: 'signatures' must be a JSON object";
      return false;
    }

    // Language must be python or guile (bash forbidden, ADR-006).
    std::string lang = doc["language"].GetString ();
    if (lang != "python" && lang != "guile")
    {
      spdlog::error (
        "[forge_coordinator] code_writer used forbidden language: {}", lang);
      job.feedback
        = "forbidden language: " + lang + " (only python/guile allowed)";
      return false;
    }

    // Inject task_id from job (not from LLM output -- LLM must not echo it).
    {
      auto &alloc = doc.GetAllocator ();
      if (doc.HasMember ("task_id"))
        doc.RemoveMember ("task_id");
      doc.AddMember (
        "task_id",
        rapidjson::Value (job.task_id.c_str (), alloc).Move (),
        alloc);
      rapidjson::StringBuffer sbuf;
      rapidjson::Writer<rapidjson::StringBuffer> sw (sbuf);
      doc.Accept (sw);
      job.writer_output_json = sbuf.GetString ();

    // Extract token usage from code_writer output if present.
    tokens_prompt = 0;
    tokens_completion = 0;
    if (doc.HasMember ("tokens_prompt") && doc["tokens_prompt"].IsInt ())
      tokens_prompt = doc["tokens_prompt"].GetInt ();
    if (doc.HasMember ("tokens_completion") && doc["tokens_completion"].IsInt ())
      tokens_completion = doc["tokens_completion"].GetInt ();
    }

    // Write worker_impl.py and copy worker.py template to disk.
    const std::string impl_code = doc["impl_code"].GetString ();
    write_code_file (job, impl_code, lang);

    return true;
  }

  // ---------------------------------------------------------------------------
  // Enforce Layer capability policy (ADR-009 Layer B)
  // Called after Writer output and after Reviewer accept — every decision
  // point.
  // ---------------------------------------------------------------------------

  bool ForgeCoordinator::enforce_capability_policy (ForgePipelineJob &job)
  {
    // Parse capability from writer_output_json.
    rapidjson::Document doc;
    if (doc.Parse (job.writer_output_json.c_str ()).HasParseError ())
    {
      job.feedback = "enforce: writer_output_json is not valid JSON";
      return false;
    }

    if (!doc.HasMember ("capability") || !doc["capability"].IsObject ())
    {
      job.feedback = "enforce: missing capability declaration";
      return false;
    }

    const auto &cap = doc["capability"];

    // network:true → always reject (ADR-006).
    if (cap.HasMember ("network") && cap["network"].IsBool ()
        && cap["network"].GetBool ())
    {
      job.feedback
        = "enforce: network:true is not permitted for generated workers";
      return false;
    }

    // exec:true → always reject (ADR-006).
    if (cap.HasMember ("exec") && cap["exec"].IsBool ()
        && cap["exec"].GetBool ())
    {
      job.feedback
        = "enforce: exec:true is not permitted for generated workers";
      return false;
    }

    // tcp_connect_ports non-empty + network:false → contradictory (ADR-015).
    if (cap.HasMember ("tcp_connect_ports")
        && cap["tcp_connect_ports"].IsArray ()
        && cap["tcp_connect_ports"].GetArray ().Size () > 0)
    {
      bool network_false = cap.HasMember ("network") && cap["network"].IsBool ()
                           && !cap["network"].GetBool ();
      if (network_false)
      {
        job.feedback = "enforce: tcp_connect_ports declared with network:false "
                       "(contradictory, ADR-015)";
        return false;
      }
    }

    return true;
  }

  // ---------------------------------------------------------------------------
  // Code Reviewer
  // ---------------------------------------------------------------------------

  bool ForgeCoordinator::call_code_reviewer (ForgePipelineJob &job,
                                               int &tokens_prompt,
                                               int &tokens_completion)
  {
    // Build input JSON per ADR-019 Code Reviewer contract.
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartObject ();
    w.Key ("task_id");
    w.String (job.task_id.c_str ());
    w.Key ("forge_job_id");
    w.String (job.id.c_str ());
    w.Key ("requirement");
    w.RawValue (job.requirement_json.c_str (), job.requirement_json.size (),
                rapidjson::kObjectType);
    w.Key ("writer_output");
    w.RawValue (job.writer_output_json.c_str (), job.writer_output_json.size (),
                rapidjson::kObjectType);
    w.EndObject ();

    const std::string input = buf.GetString ();
    const std::string output = code_reviewer (input, llm_);

    rapidjson::Document doc;
    if (doc.Parse (output.c_str ()).HasParseError ())
    {
      spdlog::error ("[forge_coordinator] code_reviewer returned invalid JSON");
      job.feedback = "code_reviewer returned invalid JSON";
      job.reviewer_verdict_json = output;
      return false;
    }

    if (!doc.HasMember ("status") || !doc["status"].IsString ())
    {
      spdlog::error (
        "[forge_coordinator] code_reviewer missing 'status' field");
      job.feedback = "code_reviewer response missing 'status' field";
      job.reviewer_verdict_json = output;
      return false;
    }

    job.reviewer_verdict_json = output;

    // Extract token usage from reviewer output if present.
    tokens_prompt = 0;
    tokens_completion = 0;
    if (doc.HasMember ("tokens_prompt") && doc["tokens_prompt"].IsInt ())
      tokens_prompt = doc["tokens_prompt"].GetInt ();
    if (doc.HasMember ("tokens_completion") && doc["tokens_completion"].IsInt ())
      tokens_completion = doc["tokens_completion"].GetInt ();

    const std::string status = doc["status"].GetString ();
    const std::string reason
      = (doc.HasMember ("reason") && doc["reason"].IsString ())
          ? doc["reason"].GetString ()
          : "";

    if (status == "accept")
    {
      spdlog::info ("[forge_coordinator] reviewer accepted job {}: {}", job.id,
                    reason);
      job.feedback = ""; // clear on accept
      return true;
    }
    else
    {
      spdlog::info ("[forge_coordinator] reviewer rejected job {}: {}", job.id,
                    reason);
      job.feedback = reason;
      return false;
    }
  }

  // ---------------------------------------------------------------------------
  // Write code file (ADR-031: two-file structure)
  // Writes worker_impl.py to the forge attempt directory.
  // Also copies worker_template.py from ~/.agentos/skills/ as worker.py.
  // ---------------------------------------------------------------------------

  bool ForgeCoordinator::write_code_file (ForgePipelineJob &job,
                                          const std::string &impl_code,
                                          const std::string &language)
  {
    const std::string ext = (language == "guile") ? "scm" : "py";
    fs::path forge_dir = agentos_home () / "forge" / job.id;
    std::error_code ec;
    fs::create_directories (forge_dir, ec);
    if (ec)
    {
      spdlog::error ("[forge_coordinator] cannot create forge dir {}: {}",
                     forge_dir.string (), ec.message ());
      return false;
    }

    // Write worker_impl.<ext> (the generated business logic).
    fs::path impl_path
      = forge_dir / ("attempt_" + std::to_string (job.attempt) + "_impl." + ext);
    {
      std::ofstream out (impl_path);
      if (!out)
      {
        spdlog::error ("[forge_coordinator] cannot write impl file {}",
                       impl_path.string ());
        return false;
      }
      out << impl_code;
    }

    // Copy worker_template.py from ~/.agentos/skills/ as worker.py alongside.
    // The template is the fixed runtime entry point (ADR-031).
    fs::path template_src = agentos_home () / "skills" / "worker_template.py";
    fs::path worker_path
      = forge_dir / ("attempt_" + std::to_string (job.attempt) + "_worker.py");
    if (fs::exists (template_src))
    {
      fs::copy_file (template_src, worker_path,
                     fs::copy_options::overwrite_existing, ec);
      if (ec)
        spdlog::warn ("[forge_coordinator] cannot copy worker template: {}",
                      ec.message ());
    }
    else
    {
      spdlog::warn ("[forge_coordinator] worker_template.py not found at {}; "
                    "sandbox will use impl directly",
                    template_src.string ());
    }

    job.last_code_path = impl_path.string ();
    spdlog::info ("[forge_coordinator] wrote impl to {}", impl_path.string ());
    return true;
  }

  // ---------------------------------------------------------------------------
  // Python syntax pre-check
  // Runs ast.parse on the impl file to catch SyntaxError before invoking
  // the Reviewer LLM.  Returns true if syntax is valid (or file is not .py).
  // On failure, sets job.feedback with the error message.
  // ---------------------------------------------------------------------------

  bool ForgeCoordinator::syntax_precheck (ForgePipelineJob &job)
  {
    // Only check Python files.
    if (job.last_code_path.empty ()
        || !job.last_code_path.ends_with (".py"))
      return true;

    // Use ast.parse — it checks syntax without executing code.
    const std::string cmd
      = "python3 -c \"import ast; ast.parse(open('"
        + job.last_code_path + "').read())\" 2>&1";

    FILE *pipe = popen (cmd.c_str (), "r");
    if (!pipe)
    {
      spdlog::warn ("[forge_coordinator] popen failed for syntax pre-check, "
                    "proceeding to reviewer");
      return true; // fail-open: let reviewer handle it
    }

    std::string output;
    char buf[512];
    while (fgets (buf, sizeof (buf), pipe))
      output += buf;
    int rc = pclose (pipe);

    if (rc != 0)
    {
      // Trim trailing newline for cleaner feedback.
      while (!output.empty () && output.back () == '\n')
        output.pop_back ();
      spdlog::warn ("[forge_coordinator] syntax pre-check failed: {}", output);
      job.feedback = "SyntaxError (pre-check): " + output;
      return false;
    }

    spdlog::info ("[forge_coordinator] syntax pre-check passed for {}",
                  job.last_code_path);
    return true;
  }

  // ---------------------------------------------------------------------------
  // Promote worker (ADR-019 worker registration steps)
  // ---------------------------------------------------------------------------

  std::optional<std::string>
  ForgeCoordinator::promote_worker (const ForgePipelineJob &job)
  {
    // Parse writer output to extract code and capability.
    rapidjson::Document writer_doc;
    if (writer_doc.Parse (job.writer_output_json.c_str ()).HasParseError ())
    {
      spdlog::error (
        "[forge_coordinator] promote_worker: invalid writer_output_json");
      return std::nullopt;
    }

    const std::string language = writer_doc.HasMember ("language")
                                   ? writer_doc["language"].GetString ()
                                   : "python";
    // ADR-031: impl_code replaces code — two-file worker structure.
    const std::string impl_code
      = writer_doc.HasMember ("impl_code")
          ? writer_doc["impl_code"].GetString ()
          : (writer_doc.HasMember ("code")
               ? writer_doc["code"].GetString ()  // backward compat
               : "");

    // Generate worker_id — use forge_job_id as the stable identifier (ADR-019).
    const std::string worker_id = job.id;

    // 1. Write worker_impl.py and worker.py (template) to worker directory.
    fs::path worker_dir = agentos_home () / "workers" / worker_id;
    std::error_code ec;
    fs::create_directories (worker_dir, ec);
    if (ec)
    {
      spdlog::error ("[forge_coordinator] cannot create worker dir {}: {}",
                     worker_dir.string (), ec.message ());
      return std::nullopt;
    }

    // Write worker_impl.py — generated business logic.
    fs::path impl_path = worker_dir / "worker_impl.py";
    {
      std::ofstream out (impl_path);
      if (!out)
      {
        spdlog::error ("[forge_coordinator] cannot write worker_impl.py {}",
                       impl_path.string ());
        return std::nullopt;
      }
      out << impl_code;
    }

    // Copy worker_template.py → worker.py (fixed runtime entry point).
    fs::path binary_path = worker_dir / "worker.py";
    {
      fs::path template_src
        = agentos_home () / "skills" / "worker_template.py";
      if (fs::exists (template_src))
      {
        fs::copy_file (template_src, binary_path,
                       fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
          spdlog::error (
            "[forge_coordinator] cannot copy worker template to {}: {}",
            binary_path.string (), ec.message ());
          return std::nullopt;
        }
      }
      else
      {
        // Fallback: write impl_code directly as worker.py so the worker
        // can still run (degraded — no template separation).
        spdlog::warn ("[forge_coordinator] worker_template.py missing, "
                      "writing impl directly to worker.py (degraded)");
        std::ofstream out (binary_path);
        if (!out)
        {
          spdlog::error ("[forge_coordinator] cannot write worker.py {}",
                         binary_path.string ());
          return std::nullopt;
        }
        out << impl_code;
      }
    }

    // 2. Build capability_json per finalize_worker_promotion contract
    // (ADR-019). Format matches make_cap_json in test_forge_pipeline.cpp: {
    // agent_id, capabilities:[{method,description,input_schema,output_schema}],
    //   requires:{...}, provenance:{...} }
    rapidjson::Document req_doc;
    req_doc.Parse (job.requirement_json.c_str ());

    rapidjson::StringBuffer manifest_buf;
    rapidjson::Writer<rapidjson::StringBuffer> mw (manifest_buf);
    mw.StartObject ();

    mw.Key ("agent_id");
    mw.String (worker_id.c_str ());

    mw.Key ("role");
    mw.String ("worker");

    mw.Key ("forge_job_id");
    mw.String (job.id.c_str ());

    // capabilities array — one entry built from requirement_json fields.
    // The method name must match the command name used in the plan step
    // so that dispatch_next_step can find this worker by capability.
    const std::string capability_method
      = (req_doc.IsObject () && req_doc.HasMember ("method")
         && req_doc["method"].IsString ())
          ? req_doc["method"].GetString ()
          : worker_id; // fallback to worker_id if method absent

    mw.Key ("capabilities");
    mw.StartArray ();
    mw.StartObject ();
    mw.Key ("method");
    mw.String (capability_method.c_str ());
    mw.Key ("description");
    if (req_doc.IsObject () && req_doc.HasMember ("description")
        && req_doc["description"].IsString ())
      mw.String (req_doc["description"].GetString ());
    else
      mw.String ("");
    mw.Key ("input_schema");
    if (req_doc.IsObject () && req_doc.HasMember ("input_schema"))
    {
      rapidjson::StringBuffer sb;
      rapidjson::Writer<rapidjson::StringBuffer> sw (sb);
      req_doc["input_schema"].Accept (sw);
      mw.RawValue (sb.GetString (), sb.GetSize (), rapidjson::kObjectType);
    }
    else
    {
      mw.StartObject ();
      mw.EndObject ();
    }
    mw.Key ("output_schema");
    if (req_doc.IsObject () && req_doc.HasMember ("output_schema"))
    {
      rapidjson::StringBuffer sb;
      rapidjson::Writer<rapidjson::StringBuffer> sw (sb);
      req_doc["output_schema"].Accept (sw);
      mw.RawValue (sb.GetString (), sb.GetSize (), rapidjson::kObjectType);
    }
    else
    {
      mw.StartObject ();
      mw.EndObject ();
    }
    mw.EndObject ();
    mw.EndArray ();
    mw.Key ("requires");
    // Copy capability block from writer output.
    if (writer_doc.HasMember ("capability"))
    {
      rapidjson::StringBuffer rb;
      rapidjson::Writer<rapidjson::StringBuffer> rw (rb);
      writer_doc["capability"].Accept (rw);
      mw.RawValue (rb.GetString (), rb.GetSize (), rapidjson::kObjectType);
    }
    mw.Key ("provenance");
    mw.StartObject ();
    mw.Key ("forge_job_id");
    mw.String (job.id.c_str ());
    mw.Key ("attempt");
    mw.Int (job.attempt);
    mw.Key ("promoted_at");
    mw.Int64 (static_cast<int64_t> (
      std::chrono::duration_cast<std::chrono::seconds> (
        std::chrono::system_clock::now ().time_since_epoch ())
        .count ()));
    mw.EndObject ();
    mw.EndObject ();
    const std::string manifest_json = manifest_buf.GetString ();

    // 3. Write manifest.json to worker directory.
    {
      std::ofstream mf (worker_dir / "manifest.json");
      if (!mf)
      {
        spdlog::error ("[forge_coordinator] cannot write manifest.json for {}",
                       worker_id);
        return std::nullopt;
      }
      mf << manifest_json;
    }

    // 4. Insert into agents + capabilities tables via Registry.
    registry_.finalize_worker_promotion (job, impl_code, manifest_json, db_);

    spdlog::info ("[forge_coordinator] worker {} promoted and registered",
                  worker_id);
    return worker_id;
  }

  // ---------------------------------------------------------------------------
  // Human review escalation
  // ---------------------------------------------------------------------------

  std::string ForgeCoordinator::escalate_to_human (const ForgePipelineJob &job,
                                                   const std::string &reason)
  {
    // Generate a review UUID.
    const std::string review_id = generate_uuid ();

    // Build artifacts JSON: last writer output + reviewer verdict.
    rapidjson::StringBuffer art_buf;
    rapidjson::Writer<rapidjson::StringBuffer> aw (art_buf);
    aw.StartObject ();
    aw.Key ("forge_job_id");
    aw.String (job.id.c_str ());
    aw.Key ("attempt");
    aw.Int (job.attempt);
    aw.Key ("writer_output");
    if (!job.writer_output_json.empty ())
      aw.RawValue (job.writer_output_json.c_str (),
                   job.writer_output_json.size (), rapidjson::kObjectType);
    else
      aw.Null ();
    aw.Key ("reviewer_verdict");
    if (!job.reviewer_verdict_json.empty ())
      aw.RawValue (job.reviewer_verdict_json.c_str (),
                   job.reviewer_verdict_json.size (), rapidjson::kObjectType);
    else
      aw.Null ();
    aw.EndObject ();

    db_.insert_human_review (review_id, reason, art_buf.GetString (), job.id);

    spdlog::info ("[forge_coordinator] job {} escalated to human review {}",
                  job.id, review_id);
    return review_id;
  }

  // ---------------------------------------------------------------------------
  // DB persistence — persist-before-act (ADR-022)
  // ---------------------------------------------------------------------------

  void ForgeCoordinator::persist (const ForgePipelineJob &job)
  {
    db_.update_forge_pipeline_job (job);
  }

} // namespace agentos::forge
