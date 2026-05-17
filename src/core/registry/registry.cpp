#include "agentos/registry.h"
#include <spdlog/spdlog.h>

namespace agentos
{

  Registry::Registry (Registry &&other) noexcept
  {
    std::unique_lock lock (other.mutex_);

    advisers_ = std::move (other.advisers_);
    executors_ = std::move (other.executors_);
    command_index_ = std::move (other.command_index_);
  }

  Registry &Registry::operator= (Registry &&other) noexcept
  {
    if (this == &other)
      return *this;

    std::unique_lock lock1 (mutex_, std::defer_lock);
    std::unique_lock lock2 (other.mutex_, std::defer_lock);

    std::lock (lock1, lock2);

    advisers_ = std::move (other.advisers_);
    executors_ = std::move (other.executors_);
    command_index_ = std::move (other.command_index_);

    return *this;
  }

  void Registry::register_adviser (const RegisteredAdviser &adviser)
  {
    std::unique_lock lock (mutex_);
    advisers_[adviser.id] = adviser;
    spdlog::info ("[registry] adviser registered: {} ({}) domains=[{}]",
                  adviser.name, adviser.id,
                  [&]
                  {
                    std::string s;
                    for (auto &d : adviser.domains)
                      s += d + ",";
                    return s;
                  }());
  }

  void Registry::register_executor (const RegisteredExecutor &executor)
  {
    std::unique_lock lock (mutex_);
    executors_[executor.id] = executor;
    for (const auto &cmd : executor.commands)
    {
      command_index_[cmd.name] = executor.id;
      spdlog::info ("[registry] command registered: {} → executor:{}", cmd.name,
                    executor.id);
    }
    spdlog::info ("[registry] executor registered: {} ({}) commands={}",
                  executor.name, executor.id, executor.commands.size ());
  }

  void Registry::remove (const ClientId &id)
  {
    std::unique_lock lock (mutex_);
    if (advisers_.erase (id))
    {
      spdlog::info ("[registry] adviser disconnected: {}", id);
      return;
    }
    auto it = executors_.find (id);
    if (it != executors_.end ())
    {
      for (const auto &cmd : it->second.commands)
        command_index_.erase (cmd.name);
      executors_.erase (it);
      spdlog::info ("[registry] executor disconnected: {}", id);
    }
  }

  std::optional<RegisteredAdviser>
  Registry::find_adviser (const std::string &domain) const
  {
    std::shared_lock lock (mutex_);
    for (const auto &[id, adviser] : advisers_)
    {
      for (const auto &d : adviser.domains)
      {
        if (d == domain)
          return adviser;
      }
    }
    return std::nullopt;
  }

  std::optional<RegisteredExecutor>
  Registry::find_executor_for_command (const std::string &command) const
  {
    std::shared_lock lock (mutex_);
    auto it = command_index_.find (command);
    if (it == command_index_.end ())
      return std::nullopt;
    auto ex = executors_.find (it->second);
    if (ex == executors_.end ())
      return std::nullopt;
    return ex->second;
  }

  std::optional<CommandSchema>
  Registry::get_command_schema (const std::string &command) const
  {
    std::shared_lock lock (mutex_);
    auto it = command_index_.find (command);
    if (it == command_index_.end ())
      return std::nullopt;
    auto ex = executors_.find (it->second);
    if (ex == executors_.end ())
      return std::nullopt;
    for (const auto &cmd : ex->second.commands)
      if (cmd.name == command)
        return cmd;
    return std::nullopt;
  }

  std::vector<CommandSchema> Registry::all_command_schemas () const
  {
    std::shared_lock lock (mutex_);
    std::vector<CommandSchema> result;
    for (const auto &[id, ex] : executors_)
      for (const auto &cmd : ex.commands)
        result.push_back (cmd);
    return result;
  }

  size_t Registry::adviser_count () const
  {
    std::shared_lock l (mutex_);
    return advisers_.size ();
  }
  size_t Registry::executor_count () const
  {
    std::shared_lock l (mutex_);
    return executors_.size ();
  }

} // namespace agentos
