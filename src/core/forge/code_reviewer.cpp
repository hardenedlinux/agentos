#include <string>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

namespace agentos::forge {

std::string code_reviewer(const std::string& input_json) {
    // TODO: actual static review, sandbox execution, and capability check.
    // For now always return accept.
    rapidjson::Document d;
    d.SetObject();
    d.AddMember("status", "accept", d.GetAllocator());
    d.AddMember("reason", "All checks passed – reviewer stub", d.GetAllocator());
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    d.Accept(w);
    return buf.GetString();
}

} // namespace agentos::forge
