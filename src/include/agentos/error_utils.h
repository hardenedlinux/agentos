#pragma once
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <string>

namespace agentos
{

  inline std::string make_error (const std::string &reason)
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
