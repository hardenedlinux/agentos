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
#include "agentos/review.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace agentos
{
  ReviewCheck check_review (const std::string &result_json)
  {
    ReviewCheck out;
    rapidjson::Document doc;
    if (doc.Parse (result_json.c_str ()).HasParseError () || !doc.IsObject ())
    {
      out.error = "reviewer result is not a JSON object";
      return out;
    }
    if (!doc.HasMember ("review") || !doc["review"].IsObject ())
    {
      out.error = "reviewer result has no review object";
      return out;
    }
    const auto &review = doc["review"];
    if (!review.HasMember ("verdict") || !review["verdict"].IsString ())
    {
      out.error = "review.verdict missing";
      return out;
    }
    const std::string verdict = review["verdict"].GetString ();
    if (verdict != "pass" && verdict != "reject")
    {
      out.error = "review.verdict must be \"pass\" or \"reject\", got \""
                  + verdict + "\"";
      return out;
    }

    if (review.HasMember ("findings"))
    {
      const auto &findings = review["findings"];
      if (!findings.IsArray ())
      {
        out.error = "review.findings must be an array";
        return out;
      }
      for (const auto &f : findings.GetArray ())
      {
        if (!f.IsObject () || !f.HasMember ("code") || !f["code"].IsString ()
            || !f.HasMember ("message") || !f["message"].IsString ()
            || (f.HasMember ("detail") && !f["detail"].IsObject ()))
        {
          out.error = "review.findings entries must be objects with string "
                      "code and message and an optional object detail";
          return out;
        }
      }
      rapidjson::StringBuffer buf;
      rapidjson::Writer<rapidjson::StringBuffer> w (buf);
      findings.Accept (w);
      out.findings_json = buf.GetString ();
    }

    out.valid = true;
    out.rejected = (verdict == "reject");
    return out;
  }

  std::string make_job_review_json (const std::string &step_id,
                                    const std::string &reviewer_id,
                                    const std::string &findings_json)
  {
    rapidjson::Document findings;
    if (findings.Parse (findings_json.c_str ()).HasParseError ()
        || !findings.IsArray ())
      findings.SetArray ();

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    w.StartObject ();
    w.Key ("step_id");
    w.String (step_id.c_str ());
    w.Key ("reviewer_id");
    w.String (reviewer_id.c_str ());
    w.Key ("verdict");
    w.String ("reject");
    w.Key ("findings");
    findings.Accept (w);
    w.EndObject ();
    return buf.GetString ();
  }
} // namespace agentos
