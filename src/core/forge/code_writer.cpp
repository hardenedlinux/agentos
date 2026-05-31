#include <string>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

namespace agentos::forge {

std::string code_writer(const std::string& input_json) {
    // TODO: integrate LLM to produce real code.
    // For now return a hard‑coded success that fulfills any requirement.
    rapidjson::Document d;
    d.SetObject();
    d.AddMember("task_id", "dummy", d.GetAllocator());
    d.AddMember("understanding", "Understood.", d.GetAllocator());
    d.AddMember("language", "python", d.GetAllocator());
    d.AddMember("entry_point", "main", d.GetAllocator());
    d.AddMember("code", "def main(): pass", d.GetAllocator());
    rapidjson::Value cap(rapidjson::kObjectType);
    cap.AddMember("network", false, d.GetAllocator());
    cap.AddMember("fs_read", rapidjson::kArrayType, d.GetAllocator());
    cap.AddMember("fs_write", rapidjson::kArrayType, d.GetAllocator());
    cap.AddMember("exec", false, d.GetAllocator());
    d.AddMember("capability", cap, d.GetAllocator());
    d.AddMember("notes", "", d.GetAllocator());
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    d.Accept(w);
    return buf.GetString();
}

} // namespace agentos::forge
