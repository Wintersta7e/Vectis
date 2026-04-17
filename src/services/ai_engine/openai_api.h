#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "core/result.h"
#include "services/ai_engine/ai_engine.h"

namespace vectis::services {

/// Default model used by the OpenAI backend. A reasonable dev-tool
/// default; overridable via config in Phase F.
constexpr std::string_view k_openai_default_model = "gpt-5-mini";

/// OpenAI chat completions endpoint.
constexpr std::string_view k_openai_endpoint =
    "https://api.openai.com/v1/chat/completions";

/// Build the JSON body. Emits a `system` message only when the system
/// prompt is non-empty, followed by a single `user` message containing
/// `[context + separator + prompt]` (or just `prompt` when no context).
[[nodiscard]] nlohmann::json build_openai_request(const AIRequest& req,
                                                  std::string_view model,
                                                  bool             stream);

/// Parse a complete non-streamed OpenAI response. Pulls text from
/// `choices[0].message.content`, usage counts from `usage.*_tokens`,
/// and sets `was_truncated` from `finish_reason == "length"`.
[[nodiscard]] vectis::core::Result<AIResponse>
parse_openai_response(const std::string& body);

/// Parse one OpenAI SSE event frame. Returns the `delta.content`
/// string when present and non-empty, empty optional for frames that
/// carry no content (role-only deltas, final finish_reason event,
/// `[DONE]` sentinel), ParseError for malformed JSON in a data line.
[[nodiscard]] vectis::core::Result<std::optional<std::string>>
parse_openai_sse_frame(std::string_view frame);

} // namespace vectis::services
