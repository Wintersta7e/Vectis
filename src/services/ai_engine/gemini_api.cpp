#include "services/ai_engine/gemini_api.h"

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

/// Extract and concatenate text from `candidates[0].content.parts[*].text`.
std::string collect_text_from_candidate0(const nlohmann::json& root)
{
    std::string text;
    if (!root.contains("candidates") || !root["candidates"].is_array() ||
        root["candidates"].empty())
    {
        return text;
    }
    const auto& cand = root["candidates"][0];
    if (!cand.contains("content")) return text;
    const auto& content = cand["content"];
    if (!content.contains("parts") || !content["parts"].is_array()) return text;
    for (const auto& part : content["parts"]) {
        if (part.contains("text") && part["text"].is_string()) {
            text += part["text"].get<std::string>();
        }
    }
    return text;
}

bool candidate0_is_truncated(const nlohmann::json& root)
{
    if (!root.contains("candidates") || !root["candidates"].is_array() ||
        root["candidates"].empty())
    {
        return false;
    }
    return root["candidates"][0].value("finishReason", "") == "MAX_TOKENS";
}

} // namespace

nlohmann::json build_gemini_request(const AIRequest& req)
{
    nlohmann::json body;

    nlohmann::json user_part;
    user_part["text"] = build_user_content(req);

    nlohmann::json user_content;
    user_content["role"]  = "user";
    user_content["parts"] = nlohmann::json::array({std::move(user_part)});

    body["contents"] = nlohmann::json::array({std::move(user_content)});

    if (!req.system_prompt.empty()) {
        nlohmann::json sys_part;
        sys_part["text"] = req.system_prompt;
        body["systemInstruction"] = {
            {"parts", nlohmann::json::array({std::move(sys_part)})},
        };
    }

    body["generationConfig"] = {
        {"maxOutputTokens", req.max_tokens},
        {"temperature",     req.temperature},
    };

    return body;
}

Result<AIResponse> parse_gemini_response(const std::string& body)
{
    try {
        const auto root = nlohmann::json::parse(body);

        AIResponse resp;
        resp.text          = collect_text_from_candidate0(root);
        resp.was_truncated = candidate0_is_truncated(root);

        if (root.contains("usageMetadata")) {
            resp.prompt_tokens =
                root["usageMetadata"].value("promptTokenCount", 0);
            resp.completion_tokens =
                root["usageMetadata"].value("candidatesTokenCount", 0);
        }

        return resp;
    }
    catch (const nlohmann::json::exception& e) {
        return make_error(ErrorKind::ParseError,
                          std::string("Gemini response JSON parse failed: ") + e.what());
    }
}

Result<std::optional<std::string>>
parse_gemini_sse_frame(std::string_view frame)
{
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
            // Join multi-line data per SSE spec.
            if (!data_line.empty()) data_line.push_back('\n');
            data_line.append(rest);
        }
        if (eol == std::string_view::npos) break;
        pos = eol + 1;
    }

    if (data_line.empty()) {
        return std::optional<std::string>{};
    }

    try {
        const auto payload = nlohmann::json::parse(data_line);
        const auto text = collect_text_from_candidate0(payload);
        if (text.empty()) {
            return std::optional<std::string>{};
        }
        return std::optional<std::string>{text};
    }
    catch (const nlohmann::json::exception& e) {
        return make_error(ErrorKind::ParseError,
                          std::string("Gemini SSE frame JSON parse failed: ") + e.what());
    }
}

} // namespace vectis::services
