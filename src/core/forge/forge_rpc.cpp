#include "agentos/forge/forge_rpc.h"
#include "agentos/forge/forge_manager.h"
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <spdlog/spdlog.h>

namespace agentos {

ForgeRpc::ForgeRpc(ForgeManager& forgeManager) : forgeManager_(forgeManager) {}

std::string ForgeRpc::handle_request(const std::string& method,
                                     const std::string& params_json) {
    rapidjson::Document params;
    params.Parse(params_json.c_str());

    if (method == "forge.list") {
        auto jobs = forgeManager_.list_jobs();
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        w.StartArray();
        for (const auto& job : jobs) {
            w.StartObject();
            w.Key("id"); w.String(job.id.value().c_str());
            w.Key("method"); w.String(job.method.c_str());
            w.Key("phase"); w.String(job.phase.c_str());
            w.Key("attempt"); w.Int(job.attempt);
            w.Key("max_attempts"); w.Int(job.max_attempts);
            w.EndObject();
        }
        w.EndArray();
        return buf.GetString();
    } else if (method == "forge.list_human_review") {
        auto jobs = forgeManager_.list_human_review_jobs();
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        w.StartArray();
        for (const auto& job : jobs) {
            w.StartObject();
            w.Key("id"); w.String(job.id.value().c_str());
            w.Key("method"); w.String(job.method.c_str());
            w.Key("phase"); w.String(job.phase.c_str());
            w.Key("attempt"); w.Int(job.attempt);
            w.Key("max_attempts"); w.Int(job.max_attempts);
            w.Key("last_code"); w.String(job.last_code.c_str());
            w.Key("last_feedback"); w.String(job.last_feedback.c_str());
            w.EndObject();
        }
        w.EndArray();
        return buf.GetString();
    } else if (method == "forge.show") {
        if (!params.HasMember("id") || !params["id"].IsString()) {
            return R"({"error":"missing id"})";
        }
        std::string id = params["id"].GetString();
        auto opt = forgeManager_.get_job(id);
        if (!opt) {
            return R"({"error":"job not found"})";
        }
        const auto& job = *opt;
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        w.StartObject();
        w.Key("id"); w.String(job.id.value().c_str());
        w.Key("method"); w.String(job.method.c_str());
        w.Key("requirement"); w.String(job.requirement.c_str());
        w.Key("phase"); w.String(job.phase.c_str());
        w.Key("attempt"); w.Int(job.attempt);
        w.Key("max_attempts"); w.Int(job.max_attempts);
        w.Key("last_code"); w.String(job.last_code.c_str());
        w.Key("last_feedback"); w.String(job.last_feedback.c_str());
        w.Key("created_at"); w.Int64(job.created_at);
        w.Key("updated_at"); w.Int64(job.updated_at);
        w.EndObject();
        return buf.GetString();
    } else if (method == "forge.approve") {
        if (!params.HasMember("id") || !params["id"].IsString()) {
            return R"({"error":"missing id"})";
        }
        std::string id = params["id"].GetString();
        forgeManager_.approve_human_review(id);
        return R"({"status":"approved"})";
    } else if (method == "forge.reject") {
        if (!params.HasMember("id") || !params["id"].IsString()) {
            return R"({"error":"missing id"})";
        }
        std::string id = params["id"].GetString();
        std::string reason;
        if (params.HasMember("reason") && params["reason"].IsString()) {
            reason = params["reason"].GetString();
        }
        forgeManager_.reject_human_review(id, reason);
        return R"({"status":"rejected"})";
    } else {
        return R"({"error":"unknown method"})";
    }
}

} // namespace agentos
