#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace vinput::result {

inline constexpr const char* kSourceRaw = "raw";
inline constexpr const char* kSourceLlm = "llm";
inline constexpr const char* kSourceAsr = "asr";
inline constexpr const char* kSourceCancel = "cancel";

struct Candidate {
    std::string text;
    std::string source;
};

struct Payload {
    std::string commitText;
    std::vector<Candidate> candidates;
};

std::string Serialize(const Payload& payload);
Payload Parse(std::string_view payload);
Payload PlainTextPayload(const std::string& text);

}  // namespace vinput::result
