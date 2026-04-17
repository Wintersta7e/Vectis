#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "core/result.h"
#include "services/ai_engine/ai_engine.h"

namespace vectis::services {

constexpr std::string_view k_gemini_default_model = "gemini-2.5-flash";

/// Non-streaming endpoint base. Caller appends `<model>:generateContent`.
constexpr std::string_view k_gemini_endpoint_base =
    "https://generativelanguage.googleapis.com/v1beta/models/";

/// Streaming path segment used with the `alt=sse` query parameter so the
/// stream format matches Claude / OpenAI (SSE frames delimited by \n\n).
constexpr std::string_view k_gemini_stream_suffix = ":streamGenerateContent?alt=sse";
constexpr std::string_view k_gemini_gen_suffix    = ":generateContent";

/// Build the JSON body. Gemini nests text under `contents[*].parts[*].text`
/// and puts the system prompt in `systemInstruction` rather than inline.
/// Decoding parameters live under `generationConfig`. The `stream` flag
/// does NOT appear in the body — streaming is selected via URL suffix.
[[nodiscard]] nlohmann::json build_gemini_request(const AIRequest& req);

/// Parse a non-streamed Gemini response. Collects text from every
/// `candidates[0].content.parts[*].text`, sums usage from
/// `usageMetadata`, and sets `was_truncated` from `finishReason ==
/// "MAX_TOKENS"`.
[[nodiscard]] vectis::core::Result<AIResponse>
parse_gemini_response(const std::string& body);

/// Parse one Gemini SSE event frame (when requested via alt=sse). Each
/// data payload is a full `GenerateContentResponse` chunk; extract the
/// concatenated text from `candidates[0].content.parts[*].text`.
[[nodiscard]] vectis::core::Result<std::optional<std::string>>
parse_gemini_sse_frame(std::string_view frame);

} // namespace vectis::services
