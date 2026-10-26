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
#include "agentos/step_repair.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>

namespace agentos
{
  namespace
  {
    // The Worker's error is free text of unknown size; keep the prompt
    // bounded.
    constexpr std::size_t kMaxErrorBytes = 4096;

    bool is_space (char c)
    {
      return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    }

    std::string trim (std::string s)
    {
      while (!s.empty () && is_space (s.back ()))
        s.pop_back ();
      std::size_t i = 0;
      while (i < s.size () && is_space (s[i]))
        ++i;
      return s.substr (i);
    }

    // "```json\n{...}\n```" -> "{...}"; anything else unchanged.
    std::string strip_code_fence (std::string s)
    {
      s = trim (std::move (s));
      if (s.rfind ("```", 0) != 0)
        return s;
      const auto nl = s.find ('\n');
      if (nl == std::string::npos)
        return s;
      s = s.substr (nl + 1);
      s = trim (std::move (s));
      if (s.size () >= 3 && s.compare (s.size () - 3, 3, "```") == 0)
        s.erase (s.size () - 3);
      return trim (std::move (s));
    }

    std::string serialise (const rapidjson::Value &v)
    {
      rapidjson::StringBuffer buf;
      rapidjson::Writer<rapidjson::StringBuffer> w (buf);
      v.Accept (w);
      return buf.GetString ();
    }

    // Canonical (re-serialised) form of a JSON text, or empty if invalid.
    std::string canonical (const std::string &json)
    {
      rapidjson::Document d;
      if (d.Parse (json.c_str ()).HasParseError ())
        return {};
      return serialise (d);
    }

    // "$step:<id>" or "$step:<id>.<field>" -> "<id>"; otherwise empty.
    std::string referenced_step (const std::string &value)
    {
      static const std::string kStep = "$step:";
      if (value.rfind (kStep, 0) != 0)
        return {};
      const std::string rest = value.substr (kStep.size ());
      return rest.substr (0, rest.find ('.'));
    }
  } // namespace

  std::string previous_step_id (const std::vector<Step> &steps,
                                const std::string &step_id)
  {
    for (std::size_t i = 1; i < steps.size (); ++i)
      if (steps[i].id == step_id)
        return steps[i - 1].id;
    return {};
  }

  std::vector<std::string>
  repair_candidates (const std::vector<Step> &steps,
                     const std::string &failing_step_id,
                     const std::unordered_map<std::string, std::string> &params)
  {
    // Only steps strictly before the failing one; steps are in plan order.
    std::vector<const Step *> earlier;
    for (const auto &s : steps)
    {
      if (s.id == failing_step_id)
        break;
      earlier.push_back (&s);
    }
    if (earlier.size () == steps.size ()) // failing step not in the plan
      return {};

    const std::string prev = previous_step_id (steps, failing_step_id);

    std::vector<std::string> wanted;
    for (const auto &[key, value] : params)
    {
      (void)key;
      if (value == "$prev_result")
      {
        if (!prev.empty ())
          wanted.push_back (prev);
        continue;
      }
      const std::string ref = referenced_step (value);
      if (!ref.empty ())
        wanted.push_back (ref);
    }
    if (wanted.empty () && !prev.empty ())
      wanted.push_back (prev);

    // Plan order, deduplicated, filtered to Adviser outputs that exist.
    std::vector<std::string> out;
    for (const Step *s : earlier)
    {
      if (std::find (wanted.begin (), wanted.end (), s->id) == wanted.end ())
        continue;
      if (s->target_type != "adviser" || s->result_json.empty ())
        continue;
      out.push_back (s->id);
    }
    return out;
  }

  std::string repair_system_prompt ()
  {
    return R"(You repair the output of an AI Adviser step in an automated pipeline. A downstream program consumed that output and rejected it with the error shown. Return the corrected output.

Rules:
- Fix the defect that causes the reported error, and any other defect of the same kind. Change nothing else: keep every key, value, wording and structure that is not part of the defect.
- Each corrected result must be the complete JSON object, with exactly the same schema as the original. Never return a diff, a fragment, or an explanation.
- Reply with one JSON object and nothing else:
  {"repairs": {"<step id>": <corrected result object>}}
  Include only the steps whose output you changed, and at least one.)";
  }

  std::string repair_user_prompt (const std::string &step_id,
                                  const std::string &command,
                                  const std::string &description,
                                  const std::string &error,
                                  const std::vector<RepairSource> &sources)
  {
    std::string err = error;
    if (err.size () > kMaxErrorBytes)
      err = err.substr (0, kMaxErrorBytes) + " [truncated]";

    std::string p;
    p += "Failed step: " + step_id + "\n";
    p += "Program: " + command + "\n";
    if (!description.empty ())
      p += "Purpose: " + description + "\n";
    p += "\nError reported by the program:\n" + err + "\n";
    p += "\nOutput consumed by the program:\n";
    for (const auto &s : sources)
    {
      p += "\nStep " + s.step_id + " (Adviser " + s.adviser_id + "):\n";
      p += s.result_json + "\n";
    }
    return p;
  }

  RepairParse parse_repair_reply (const std::string &content,
                                  const std::vector<RepairSource> &sources)
  {
    RepairParse out;
    const std::string body = strip_code_fence (content);

    rapidjson::Document doc;
    if (doc.Parse (body.c_str ()).HasParseError () || !doc.IsObject ())
    {
      out.error = "repair reply is not a JSON object";
      return out;
    }
    if (!doc.HasMember ("repairs") || !doc["repairs"].IsObject ())
    {
      out.error = "repair reply has no repairs object";
      return out;
    }
    const auto &repairs = doc["repairs"];
    if (repairs.MemberCount () == 0)
    {
      out.error = "repair reply repairs nothing";
      return out;
    }

    for (auto m = repairs.MemberBegin (); m != repairs.MemberEnd (); ++m)
    {
      const std::string id = m->name.GetString ();
      auto src = std::find_if (sources.begin (), sources.end (),
                               [&] (const RepairSource &s)
                               { return s.step_id == id; });
      if (src == sources.end ())
      {
        out.repairs.clear ();
        out.error = "repair reply names step '" + id
                    + "', which is not a repairable output";
        return out;
      }
      if (!m->value.IsObject ())
      {
        out.repairs.clear ();
        out.error = "repaired result for step '" + id + "' is not an object";
        return out;
      }
      const std::string fixed = serialise (m->value);
      if (fixed == canonical (src->result_json))
        continue; // unchanged
      out.repairs.emplace_back (id, fixed);
    }

    if (out.repairs.empty ())
    {
      out.error = "repair reply returned the output unchanged";
      return out;
    }
    out.ok = true;
    return out;
  }
} // namespace agentos
