#pragma once
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <string>

namespace agentos
{
  // ---------------------------------------------------------------
  // Used by Enforce Layer and sandbox checks
  // ---------------------------------------------------------------
  static inline std::string make_reject_verdict (const std::string &task_id,
                                                 const std::string &reason)
  {
    rapidjson::Document doc;
    doc.SetObject ();
    auto &alloc = doc.GetAllocator ();
    doc.AddMember ("task_id",
                   rapidjson::Value (task_id.c_str (), alloc).Move (), alloc);
    doc.AddMember ("status", rapidjson::Value ("reject", alloc).Move (), alloc);
    doc.AddMember ("reason", rapidjson::Value (reason.c_str (), alloc).Move (),
                   alloc);
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    doc.Accept (w);
    return buf.GetString ();
  }

  static inline std::string make_error (const std::string &reason)
  {
    rapidjson::Document doc;
    doc.SetObject ();
    doc.AddMember ("status", "error", doc.GetAllocator ());
    doc.AddMember (
      "reason", rapidjson::Value (reason.c_str (), doc.GetAllocator ()).Move (),
      doc.GetAllocator ());
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w (buf);
    doc.Accept (w);
    return buf.GetString ();
  }

} // namespace agentos
