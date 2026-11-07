#pragma once

#include <string>
#include <string_view>

#include "common/asr/recognition_result.h"

namespace vinput::client {

// Serializes a daemon status change into a single-line JSON object for stdin broadcast.
// Example: {"type":"status","status":"recording","ime":true}
std::string SerializeStatusEvent(std::string_view status, bool ime);

// Serializes an interim partial transcription into a single-line JSON object for stdin broadcast.
// Example: {"type":"partial","text":"recognizing audio","ime":true}
std::string SerializePartialEvent(std::string_view text, bool ime);

// Serializes a committed transcription and candidates into a single-line JSON object for stdin
// broadcast. Example: {"type":"committed","text":"Hello world.","candidates":[{"text":"Hello
// world.","source":"llm"}],"ime":true}
std::string SerializeCommittedEvent(const vinput::result::Payload& payload, bool ime);

} // namespace vinput::client
