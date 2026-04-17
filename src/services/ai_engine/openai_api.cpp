#include "services/ai_engine/openai_api.h"

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
constexpr std::string_view k_done_sentinel     = "[DONE]";

std::string build_user_content(const AIRequest& req)
{
    if (req.context.empty()) return req.user_prompt;
    std::string out;
    out.reserve(req.context.size() + k_context_separator.size() +
                req.user_prompt.size());
    out.append(req.context);
    out.append(k_context_separator);
    out.append(req.user_prompt);
    return out;
}

} // namespace

nlohmann::json build_openai_request(const AIRequest& req,
                                    std::string_view model,
                                    bool             stream)
{
    nlohmann::json messages = nlohmann::json::array();
    if (!req.system_prompt.empty()) {
        messages.push_back({
            {"role",    "system"},
            {"content", req.system_prompt},
        });
    }
    messages.push_back({
        {"role",    "user"},
        {"content", build_user_content(req)},
    });

    nlohmann::json body;
    body["model"]       = std::string(model);
    body["messages"]    = std::move(messages);
    body["max_tokens"]  = req.max_tokens;
    body["temperature"] = req.temperature;
    if (stream) {
        body["stream"] = true;
    }
    return body;
}

Result<AIResponse> parse_openai_response(const std::string& body)
{
    try {
        const auto root = nlohmann::json::parse(body);

        AIResponse resp;

        if (root.contains("choices") && root["choices"].is_array() &&
            !root["choices"].empty())
        {
            const auto& first = root["choices"][0];
            if (first.contains("message")) {
                resp.text = first["message"].value("content", "");
            }
            resp.was_truncated =
                (first.value("finish_reason", "") == "length");
        }

        if (root.contains("usage")) {
            resp.prompt_tokens     = root["usage"].value("prompt_tokens", 0);
            resp.completion_tokens = root["usage"].value("completion_tokens", 0);
        }

        return resp;
    }
    catch (const nlohmann::json::exception& e) {
        return make_error(ErrorKind::ParseError,
                          std::string("OpenAI response JSON parse failed: ") + e.what());
    }
}

Result<std::optional<std::string>>
parse_openai_sse_frame(std::string_view frame)
{
    // Extract the data line from the frame (OpenAI only sends data:
    // lines; no event: header).
    std::string data_line;

    std::size_t pos = 0;
    while (pos < frame.size()) {
        const std::size_t eol = frame.find('\n', pos);
        std::string_view line =
            frame.substr(pos, eol == std::string_view::npos
                                  ? std::string_view::npos
                                  : eol - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.starts_with("data:")) {
            auto rest = line.substr(5);
            while (!rest.empty() && rest.front() == ' ') {
                rest.remove_prefix(1);
            }
            // Join multi-line data per SSE spec (OpenAI currently uses
            // single-line JSON, but be spec-compliant defensively).
            if (!data_line.empty()) data_line.push_back('\n');
            data_line.append(rest);
            // Don't break — keep scanning in case the event spans
            // multiple data: lines.
        }
        if (eol == std::string_view::npos) break;
        pos = eol + 1;
    }

    if (data_line.empty()) {
        return std::optional<std::string>{};
    }
    // `data: [DONE]` is the end-of-stream sentinel — no delta, no error.
    if (data_line == k_done_sentinel) {
        return std::optional<std::string>{};
    }

    try {
        const auto payload = nlohmann::json::parse(data_line);
        if (payload.contains("choices") && payload["choices"].is_array() &&
            !payload["choices"].empty())
        {
            const auto& first = payload["choices"][0];
            if (first.contains("delta")) {
                const auto& delta = first["delta"];
                if (delta.contains("content") && delta["content"].is_string()) {
                    auto text = delta["content"].get<std::string>();
                    if (!text.empty()) {
                        return std::optional<std::string>{std::move(text)};
                    }
                }
            }
        }
        return std::optional<std::string>{};
    }
    catch (const nlohmann::json::exception& e) {
        return make_error(ErrorKind::ParseError,
                          std::string("OpenAI SSE frame JSON parse failed: ") + e.what());
    }
}

} // namespace vectis::services
