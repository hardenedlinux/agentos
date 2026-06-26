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

#include "agentos/periodic_executor.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <spdlog/spdlog.h>

#include <chrono>

namespace agentos
{

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PeriodicExecutor::PeriodicExecutor (Database           &db,
                                    Dispatcher         &dispatcher,
                                    const Config       &config,
                                    SendToOrchestrator  send_to_orchestrator,
                                    SendToMaster        send_to_master,
                                    GatewayPush         gateway_push)
    : db_ (db), dispatcher_ (dispatcher), config_ (config),
      send_to_orchestrator_ (std::move (send_to_orchestrator)),
      send_to_master_ (std::move (send_to_master)),
      gateway_push_ (std::move (gateway_push))
{
  daemon_start_s_ = now_s ();
}

// ---------------------------------------------------------------------------
// init — called by Central before start()
// ---------------------------------------------------------------------------

void PeriodicExecutor::init ()
{
  // timer_tasks DDL lives in Database::open() — no DDL here.
  load_enabled_tasks ();
  seed_heartbeat_if_absent ();
  seed_reaper_if_absent ();

  spdlog::info ("[periodic_executor] initialised: heartbeat={}s",
                (config_.gateway.heartbeat_interval_s > 0)
                  ? config_.gateway.heartbeat_interval_s
                  : 30);
}

// ---------------------------------------------------------------------------
// seed_reaper_if_absent — reap worker children every 5 seconds
// ---------------------------------------------------------------------------

void PeriodicExecutor::seed_reaper_if_absent ()
{
  if (db_.timer_task_exists ("reaper"))
    return;

  const int64_t now = now_s ();

  TimerTask tt;
  tt.id           = "reaper";
  tt.interval_s   = 5;
  tt.next_fire    = now + 5;
  tt.target       = TaskTarget::Orchestrator;
  tt.payload_json = "";
  tt.enabled      = true;
  tt.created_at   = now;
  db_.insert_timer_task (tt);

  Task t;
  t.id           = "reaper";
  t.next_fire    = tt.next_fire;
  t.interval_s   = 5;
  t.target       = TaskTarget::Orchestrator;
  t.payload_json = "";
  heap_.push (std::move (t));

  spdlog::info ("[periodic_executor] seeded reaper task (interval=5s)");
}

// ---------------------------------------------------------------------------
// load_enabled_tasks – restore from timer_tasks table via Database
// ---------------------------------------------------------------------------

void PeriodicExecutor::load_enabled_tasks ()
{
  const int64_t now = now_s ();

  for (auto &tt : db_.load_enabled_timer_tasks ())
    {
      // One-shot missed during downtime → mark disabled, skip.
      if (tt.interval_s == 0 && tt.next_fire < now)
        {
          spdlog::info ("[periodic_executor] expired one-shot task {}, "
                        "disabling", tt.id);
          db_.disable_timer_task (tt.id);
          continue;
        }

      int64_t next_fire = tt.next_fire;

      // Periodic missed → advance to now + interval, persist.
      if (tt.interval_s > 0 && next_fire < now)
        {
          next_fire = now + tt.interval_s;
          db_.upsert_timer_task_next_fire (tt.id, next_fire);
        }

      Task t;
      t.id           = tt.id;
      t.next_fire    = next_fire;
      t.interval_s   = tt.interval_s;
      t.target       = tt.target;
      t.payload_json = tt.payload_json;
      heap_.push (std::move (t));
    }
}

// ---------------------------------------------------------------------------
// seed_heartbeat_if_absent – ADR-023 seed_if_absent pattern
// ---------------------------------------------------------------------------

void PeriodicExecutor::seed_heartbeat_if_absent ()
{
  if (db_.timer_task_exists ("heartbeat"))
    {
      spdlog::info ("[periodic_executor] heartbeat task already seeded");
      return;
    }

  const int64_t now = now_s ();
  const int64_t hb_interval
    = (config_.gateway.heartbeat_interval_s > 0)
        ? config_.gateway.heartbeat_interval_s
        : 30;

  TimerTask hb;
  hb.id           = "heartbeat";
  hb.interval_s   = hb_interval;
  hb.next_fire    = now + hb_interval;
  hb.target       = TaskTarget::Gateway;
  hb.payload_json = "";
  hb.enabled      = true;
  hb.created_at   = now;
  db_.insert_timer_task (hb);

  // Push into heap — load_enabled_tasks ran before this and didn't see it.
  Task t;
  t.id           = "heartbeat";
  t.next_fire    = hb.next_fire;
  t.interval_s   = hb.interval_s;
  t.target       = TaskTarget::Gateway;
  t.payload_json = "";
  heap_.push (std::move (t));

  spdlog::info ("[periodic_executor] seeded heartbeat task with interval {}s",
                hb_interval);
}

// ---------------------------------------------------------------------------
// loop — overrides Actor default to use pop_for() with timeout
// ---------------------------------------------------------------------------

void PeriodicExecutor::loop ()
{
  while (running_)
    {
      // Compute sleep duration until next task fires (or 1 second max).
      const int64_t now    = now_s ();
      int64_t       sleep_ms = 1000;

      if (!heap_.empty ())
        {
          const int64_t until_next =
            (heap_.top ().next_fire - now) * 1000;
          if (until_next < sleep_ms)
            sleep_ms = std::max<int64_t> (0, until_next);
        }

      // Drain control messages with timeout.
      auto ctrl = pop_for (std::chrono::milliseconds (sleep_ms));
      if (ctrl)
        on_message (std::move (*ctrl));

      // Drain any remaining control messages without waiting.
      while (true)
        {
          auto more = pop_for (std::chrono::milliseconds (0));
          if (!more)
            break;
          on_message (std::move (*more));
        }

      // Fire all due tasks.
      const int64_t now2 = now_s ();
      while (!heap_.empty () && heap_.top ().next_fire <= now2)
        {
          Task t = heap_.top ();
          heap_.pop ();
          fire (t);

          if (t.interval_s > 0)
            {
              t.next_fire = now2 + t.interval_s;
              // Persist the new next_fire (insert or update in DB).
              persist_task (t);
              heap_.push (t);
            }
          else
            {
              mark_disabled (t.id);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// on_message — Register / Cancel
// ---------------------------------------------------------------------------

void PeriodicExecutor::on_message (PeriodicControl msg)
{
  if (msg.kind == PeriodicControl::Kind::Register)
    register_task (msg.task);
  else
    cancel_task (msg.cancel_id);
}

// ---------------------------------------------------------------------------
// register_task
// ---------------------------------------------------------------------------

void PeriodicExecutor::register_task (PeriodicControl::Task t)
{
  // Cancel existing task with same id if present (replace semantics).
  cancel_task (t.id);

  Task task;
  task.id          = t.id;
  task.next_fire   = t.next_fire > 0 ? t.next_fire : now_s () + t.interval_s;
  task.interval_s  = t.interval_s;
  task.target      = t.target;
  task.payload_json = t.payload_json;

  persist_task (task);
  heap_.push (task);

  spdlog::info ("[periodic_executor] registered task {} interval={}s target={}",
                task.id, task.interval_s, to_string (task.target));
}

// ---------------------------------------------------------------------------
// cancel_task — removes from heap by rebuilding (heap has no random remove)
// ---------------------------------------------------------------------------

void PeriodicExecutor::cancel_task (const std::string &id)
{
  if (id.empty ())
    return;

  // Rebuild heap without the cancelled task.
  std::vector<Task> remaining;
  while (!heap_.empty ())
    {
      Task t = heap_.top ();
      heap_.pop ();
      if (t.id != id)
        remaining.push_back (std::move (t));
    }
  for (auto &t : remaining)
    heap_.push (std::move (t));

  mark_disabled (id);
  spdlog::info ("[periodic_executor] cancelled task {}", id);
}

// ---------------------------------------------------------------------------
// fire — dispatch task to target (uses task.target enum)
// ---------------------------------------------------------------------------

void PeriodicExecutor::fire (const Task &t)
{
  spdlog::debug ("[periodic_executor] firing task {} target={}",
                 t.id, to_string (t.target));

  switch (t.target)
    {
    case TaskTarget::Gateway:
      {
        if (t.id == "heartbeat")
          {
            const std::string payload = build_heartbeat_payload ();
            gateway_push_ (payload);
          }
        else
          {
            gateway_push_ (t.payload_json);
          }
        break;
      }
    case TaskTarget::Orchestrator:
      {
        if (t.id == "reaper")
          {
            // Special case: call dispatcher_.reap() directly.
            dispatcher_.reap ();
          }
        else
          {
            OrchestratorEvent ev;
            ev.kind         = OrchestratorEvent::Kind::TimerFired;
            ev.payload_json = t.payload_json;
            send_to_orchestrator_ (std::move (ev));
          }
        break;
      }
    case TaskTarget::Master:
      {
        MasterEvent ev;
        ev.kind         = MasterEvent::Kind::ScheduledTask;
        ev.payload_json = t.payload_json;
        send_to_master_ (std::move (ev));
        break;
      }
    default:
      spdlog::warn ("[periodic_executor] unknown target '{}' for task {}",
                    static_cast<int>(t.target), t.id);
      break;
    }
}

// ---------------------------------------------------------------------------
// Heartbeat payload
// ---------------------------------------------------------------------------

std::string PeriodicExecutor::build_heartbeat_payload () const
{
  const int64_t now     = now_s ();
  const int64_t uptime  = now - daemon_start_s_;

  // running_jobs: count of active worker_runs with status=running (0).
  const auto active_runs = db_.get_active_worker_runs ();
  const int  running_jobs = static_cast<int> (active_runs.size ());

  rapidjson::StringBuffer buf;
  rapidjson::Writer<rapidjson::StringBuffer> w (buf);
  w.StartObject ();
  w.Key ("jsonrpc");       w.String ("2.0");
  w.Key ("method");        w.String ("system.heartbeat");
  w.Key ("params");
  w.StartObject ();
  w.Key ("timestamp");     w.Int64 (now);
  w.Key ("uptime_s");      w.Int64 (uptime);
  w.Key ("running_jobs");  w.Int (running_jobs);
  w.Key ("llm_queue_depth"); w.Int (0); // TODO: expose from LlmProxy
  w.EndObject ();
  w.EndObject ();
  return buf.GetString ();
}

// ---------------------------------------------------------------------------
// Persistence helpers — delegate to Database (ADR-021)
// ---------------------------------------------------------------------------

void PeriodicExecutor::persist_task (const Task &t)
{
  TimerTask tt;
  tt.id           = t.id;
  tt.interval_s   = t.interval_s;
  tt.next_fire    = t.next_fire;
  tt.target       = t.target;
  tt.payload_json = t.payload_json;
  tt.enabled      = true;
  tt.created_at   = now_s ();
  db_.persist_timer_task (tt);
}

void PeriodicExecutor::mark_disabled (const std::string &id)
{
  db_.disable_timer_task (id);
}

// ---------------------------------------------------------------------------
// now_s
// ---------------------------------------------------------------------------

int64_t PeriodicExecutor::now_s () const
{
  return static_cast<int64_t> (
    std::chrono::duration_cast<std::chrono::seconds> (
      std::chrono::system_clock::now ().time_since_epoch ())
      .count ());
}

} // namespace agentos
