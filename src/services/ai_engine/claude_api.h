#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "core/result.h"
#include "services/ai_engine/ai_engine.h"

namespace vectis::services {

/// Default model used by the Claude backend when the caller does not
/// override. Chosen as a sensible dev-tool default (recent Sonnet);
/// callers can set anything via `APIBackend` config in Step 7's
/// Phase F wiring.
constexpr std::string_view k_claude_default_model = "claude-sonnet-4-6";

/// The Anthropic Messages API endpoint.
constexpr std::string_view k_claude_endpoint = "https://api.anthropic.com/v1/messages";

/// Build the JSON body for the Messages API. The `system` field is
/// included only when the request has a non-empty system prompt. The
/// `context` field of `AIRequest` is prepended to the user prompt
/// with a separator so it is delivered as a single user turn.
[[nodiscard]] nlohmann::json build_claude_request(const AIRequest& req,
                                                  std::string_view model,
                                                  bool             stream);

/// Parse a complete Claude JSON response (the non-streamed path).
/// Populates `text`, `prompt_tokens`, `completion_tokens`, and
/// `was_truncated` from `stop_reason == "max_tokens"`. `backend_used`
/// is NOT set here — the caller sets it after dispatch.
[[nodiscard]] vectis::core::Result<AIResponse>
parse_claude_response(const std::string& body);

/// Parse one Claude SSE event frame. A frame is one
/// `event: <name>\ndata: <json>` block. Returns the text delta string
/// when the event is `content_block_delta` with a `text_delta`, an
/// empty optional for every other event type (message_start,
/// message_stop, etc.), and a ParseError for malformed input.
[[nodiscard]] vectis::core::Result<std::optional<std::string>>
parse_claude_sse_frame(std::string_view frame);

} // namespace vectis::services
