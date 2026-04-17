#include "services/ai_engine/claude_api.h"

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "core/result.h"
#include "services/ai_engine/ai_engine.h"

namespace vectis::services {

using vectis::core::ErrorKind;
using vectis::core::make_error;
using vectis::core::Result;

namespace {

constexpr std::string_view k_context_separator = "\n\n---\n\n";

/// Combine optional context + user prompt into a single user turn.
std::string build_user_content(const AIRequest& req)
{
    if (req.context.empty()) {
        return req.user_prompt;
    }
    std::string out;
    out.reserve(req.context.size() + k_context_separator.size() +
                req.user_prompt.size());
    out.append(req.context);
    out.append(k_context_separator);
    out.append(req.user_prompt);
    return out;
}

} // namespace

nlohmann::json build_claude_request(const AIRequest& req,
                                    std::string_view model,
                                    bool             stream)
{
    nlohmann::json body;
    body["model"]      = std::string(model);
    body["max_tokens"] = req.max_tokens;
    body["temperature"] = req.temperature;

    if (!req.system_prompt.empty()) {
        body["system"] = req.system_prompt;
    }

    body["messages"] = nlohmann::json::array({
        nlohmann::json{
            {"role",    "user"},
            {"content", build_user_content(req)},
        },
    });

    if (stream) {
        body["stream"] = true;
    }

    return body;
}

Result<AIResponse> parse_claude_response(const std::string& body)
{
    try {
        const auto root = nlohmann::json::parse(body);

        AIResponse resp;

        // Text from content[0].text — Claude can emit multiple content
        // blocks; concatenate any with type == "text".
        if (root.contains("content") && root["content"].is_array()) {
            for (const auto& block : root["content"]) {
                if (block.value("type", "") == "text") {
                    resp.text += block.value("text", "");
                }
            }
        }

        // Usage.
        if (root.contains("usage")) {
            resp.prompt_tokens     = root["usage"].value("input_tokens", 0);
            resp.completion_tokens = root["usage"].value("output_tokens", 0);
        }

        // Truncation is reported as stop_reason == "max_tokens".
        resp.was_truncated = (root.value("stop_reason", "") == "max_tokens");

        return resp;
    }
    catch (const nlohmann::json::exception& e) {
        return make_error(ErrorKind::ParseError,
                          std::string("Claude response JSON parse failed: ") + e.what());
    }
}

Result<std::optional<std::string>>
parse_claude_sse_frame(std::string_view frame)
{
    // A frame is one or more CRLF/LF-terminated lines that together
    // form one event. We only care about the `data:` payload and
    // the `event:` name (for discrimination); every other line is
    // ignored.
    std::string data_line;
    std::string event_name;

    std::size_t pos = 0;
    while (pos < frame.size()) {
        const std::size_t eol = frame.find('\n', pos);
        std::string_view line =
            frame.substr(pos, eol == std::string_view::npos
                                  ? std::string_view::npos
                                  : eol - pos);
        // Trim optional trailing \r.
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        if (line.starts_with("event:")) {
            auto rest = line.substr(6);
            while (!rest.empty() && rest.front() == ' ') {
                rest.remove_prefix(1);
            }
            event_name.assign(rest);
        } else if (line.starts_with("data:")) {
            auto rest = line.substr(5);
            while (!rest.empty() && rest.front() == ' ') {
                rest.remove_prefix(1);
            }
            // Multi-line data fields get concatenated with newlines per
            // the SSE spec; Claude uses single-line JSON so we keep it
            // simple and overwrite rather than append.
            data_line.assign(rest);
        }

        if (eol == std::string_view::npos) break;
        pos = eol + 1;
    }

    // Frames without a data payload (keep-alive comments, empty frames)
    // are not an error — just report "no delta".
    if (data_line.empty()) {
        return std::optional<std::string>{};
    }

    try {
        const auto payload = nlohmann::json::parse(data_line);
        // The payload's "type" field is authoritative; the event: line
        // may not appear in every frame (Anthropic sends it, but SSE
        // parsers must not require it).
        const std::string type =
            payload.value("type", event_name);

        if (type == "content_block_delta") {
            if (payload.contains("delta")) {
                const auto& d = payload["delta"];
                if (d.value("type", "") == "text_delta") {
                    return std::optional<std::string>{d.value("text", "")};
                }
            }
        }
        // All other event types (message_start, message_delta,
        // content_block_start/stop, message_stop, ping) carry no
        // renderable text — return empty optional.
        return std::optional<std::string>{};
    }
    catch (const nlohmann::json::exception& e) {
        return make_error(ErrorKind::ParseError,
                          std::string("Claude SSE frame JSON parse failed: ") + e.what());
    }
}

} // namespace vectis::services
