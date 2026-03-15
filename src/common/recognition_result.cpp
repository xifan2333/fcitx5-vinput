#include "recognition_result.h"

#include <nlohmann/json.hpp>

#include <string>

namespace vinput::result {
namespace {

using json = nlohmann::json;

std::string JsonString(const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

}  // namespace

std::string Serialize(const Payload& payload) {
    json root = {
        {"commit_text", payload.commitText},
        {"candidates", json::array()},
    };

    for (const auto& candidate : payload.candidates) {
        root["candidates"].push_back({
            {"text", candidate.text},
            {"source", candidate.source},
        });
    }

    return root.dump();
}

Payload Parse(std::string_view payload_text) {
    if (payload_text.empty()) {
        return {};
    }

    json root;
    try {
        root = json::parse(payload_text);
    } catch (const std::exception&) {
        return {};
    }

    if (!root.is_object()) {
        return {};
    }

    Payload payload;
    payload.commitText = JsonString(root, "commit_text");

    const auto candidates_it = root.find("candidates");
    if (candidates_it != root.end() && candidates_it->is_array()) {
        for (const auto& candidate_value : *candidates_it) {
            if (!candidate_value.is_object()) {
                continue;
            }

            Candidate candidate{
                .text = JsonString(candidate_value, "text"),
                .source = JsonString(candidate_value, "source"),
            };
            if (!candidate.text.empty()) {
                payload.candidates.push_back(std::move(candidate));
            }
        }
    }

    if (payload.commitText.empty() && !payload.candidates.empty()) {
        payload.commitText = payload.candidates.front().text;
    }

    if (payload.candidates.empty() && !payload.commitText.empty()) {
        payload.candidates.push_back(
            Candidate{.text = payload.commitText, .source = kSourceRaw});
    }

    if (payload.commitText.empty() && payload.candidates.empty()) {
        return {};
    }

    return payload;
}

}  // namespace vinput::result
