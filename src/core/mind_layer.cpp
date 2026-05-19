#include "agentos/mind_layer.h"
#include "agentos/registry.h"
#include "database/database.hpp"
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

namespace agentos {

MindLayer::MindLayer(Registry& registry, Database& db)
    : registry_(registry), db_(db)
{}

std::string MindLayer::analyse_task(const Task& task) const {
    // For now, return a simple plan with a single step
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("steps");
    w.StartArray();
    w.StartObject();
    w.Key("id");      w.String("step_1");
    w.Key("command"); w.String("web.search");
    w.Key("args");
    w.StartObject();
    w.Key("query");   w.String(task.goal.c_str());
    w.EndObject();
    w.Key("depends_on");
    w.StartArray();
    w.EndArray();
    w.EndObject();
    w.EndArray();
    w.EndObject();
    return buf.GetString();
}

std::vector<CapabilityDeclaration> MindLayer::required_capabilities(
    const std::string& plan_json) const {
    std::vector<CapabilityDeclaration> caps;
    rapidjson::Document doc;
    if (doc.Parse(plan_json.c_str()).HasParseError()) {
        return caps;
    }
    if (!doc.HasMember("steps") || !doc["steps"].IsArray()) {
        return caps;
    }
    for (const auto& step : doc["steps"].GetArray()) {
        (void)step; // suppress unused variable warning
        CapabilityDeclaration decl;
        // For now, assume no network or exec needed
        decl.network = false;
        decl.exec = false;
        caps.push_back(decl);
    }
    return caps;
}

} // namespace agentos
// No changes needed
