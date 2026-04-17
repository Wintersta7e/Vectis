#include "modes/ask/question_classifier.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace vectis::modes::ask {

namespace {

/// Lowercase copy for case-insensitive matching. ASCII only — fine for
/// the English-language keywords we're comparing against.
std::string to_lower(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (const char ch : s) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

bool contains(std::string_view haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string_view::npos;
}

/// True when `needle` appears in `haystack` with a non-alphanumeric
/// character before it (or at the start of the string). Prevents
/// keyword prefixes from matching inside longer words — e.g. stops
/// "function" from matching "dysfunction" or "interfaces".
bool contains_word_start(std::string_view haystack, std::string_view needle)
{
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
        if (pos == 0 ||
            !std::isalnum(static_cast<unsigned char>(haystack[pos - 1])))
        {
            return true;
        }
        ++pos;
    }
    return false;
}

/// Does the text contain a directory prefix like "src/" or "tests/"
/// or any common source-file extension?
bool has_file_path_pattern(std::string_view lower_q)
{
    static constexpr std::array<std::string_view, 4> k_dir_prefixes = {
        "src/", "tests/", "lib/", "include/",
    };
    for (const auto p : k_dir_prefixes) {
        if (contains(lower_q, p)) return true;
    }

    static constexpr std::array<std::string_view, 16> k_extensions = {
        ".py", ".ts", ".tsx", ".js", ".jsx", ".cpp", ".cc", ".c",
        ".h", ".hpp", ".rs", ".go", ".rb", ".php", ".sql", ".java",
    };
    for (const auto e : k_extensions) {
        if (contains(lower_q, e)) return true;
    }

    // ".cs" is ambiguous ("CS" as a two-letter word). Require it only
    // when followed by a word boundary (space, end, punctuation).
    const auto pos = lower_q.find(".cs");
    if (pos != std::string_view::npos) {
        const auto after = pos + 3;
        if (after == lower_q.size() ||
            lower_q[after] == ' ' || lower_q[after] == '?' ||
            lower_q[after] == '.' || lower_q[after] == ',')
        {
            return true;
        }
    }
    return false;
}

bool has_code_keyword(std::string_view lower_q)
{
    // Word-start matching so "function" doesn't match "dysfunction",
    // "interface" doesn't match "interfaces are" (it will via the
    // plural check below), etc.
    static constexpr std::array<std::string_view, 14> k_keywords = {
        "function", "class ", "method", "module", "import ",
        "struct",   "interface", "constructor", "namespace",
        "endpoint", "this codebase", "this project", "my code",
        "the code",
    };
    for (const auto k : k_keywords) {
        if (contains_word_start(lower_q, k)) return true;
    }
    return false;
}

bool has_web_keyword(std::string_view lower_q)
{
    static constexpr std::array<std::string_view, 8> k_keywords = {
        "how do i", "how to ", "what is ", "what are ",
        "difference between", "best practice", "documentation for",
        " vs ",
    };
    for (const auto k : k_keywords) {
        if (contains(lower_q, k)) return true;
    }
    return false;
}

} // namespace

QuestionSource classify_question(std::string_view question,
                                 bool             codebase_loaded)
{
    const auto lower = to_lower(question);

    // Rule 1: explicit file path → Codebase, but ONLY if a project is
    // loaded. Without a project, routing to Codebase leaves the
    // pipeline with zero context (both FTS and web searches get
    // skipped), and the AI answers from priors — which for a question
    // like "what's in src/main.cpp?" means hallucinating file content.
    // Prefer Web in that case so the AI at least has search snippets.
    if (has_file_path_pattern(lower) && codebase_loaded) {
        return QuestionSource::Codebase;
    }

    const bool code_kw = has_code_keyword(lower);
    const bool web_kw  = has_web_keyword(lower);

    // Rule 4 (checked before 2/3): both families of keywords present
    // AND a codebase is loaded → Mixed. Without a codebase, fall
    // through to a pure Web answer.
    if (code_kw && web_kw) {
        return codebase_loaded ? QuestionSource::Mixed : QuestionSource::Web;
    }

    // Rule 2: code keyword + codebase → Codebase.
    if (code_kw && codebase_loaded) {
        return QuestionSource::Codebase;
    }

    // Rule 3: explicit web-phrasing keyword → Web.
    if (web_kw) {
        return QuestionSource::Web;
    }

    // Rule 5: default fallback based on whether a project is loaded.
    return codebase_loaded ? QuestionSource::Codebase : QuestionSource::Web;
}

} // namespace vectis::modes::ask
