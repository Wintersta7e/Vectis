#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "cli/guide.h"

namespace {

// Strip carriage returns so the comparison is line-ending agnostic — the kit
// file may be checked out with CRLF on Windows while `print_guide` emits LF.
std::string normalize_newlines(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        if (c != '\r') {
            out.push_back(c);
        }
    }
    return out;
}

// Return `md` with a leading YAML frontmatter block ("---\n...\n---\n")
// removed. Assumes `md` is already newline-normalized.
std::string strip_frontmatter(const std::string& md)
{
    constexpr std::string_view k_open = "---\n";
    if (!md.starts_with(k_open)) {
        return md;
    }
    const std::string::size_type close = md.find("\n---\n", k_open.size());
    if (close == std::string::npos) {
        return md;
    }
    std::string::size_type body = close + 5; // past the closing "\n---\n"
    while (body < md.size() && md[body] == '\n') {
        ++body;
    }
    return md.substr(body);
}

std::string trim(std::string_view s)
{
    constexpr std::string_view k_ws = " \t\r\n";
    const std::string_view::size_type b = s.find_first_not_of(k_ws);
    if (b == std::string_view::npos) {
        return std::string{};
    }
    const std::string_view::size_type e = s.find_last_not_of(k_ws);
    return std::string{s.substr(b, e - b + 1)};
}

// The committed kit skill is generated from `vectis guide` output (see
// scripts/build-agent-kit.sh). This guards against it drifting from the
// binary's single source of truth between release runs.
TEST(AgentKitSkill, BodyMatchesGuideOutput)
{
    std::ifstream skill(VECTIS_AGENT_KIT_SKILL, std::ios::binary);
    ASSERT_TRUE(skill.is_open()) << "could not open " << VECTIS_AGENT_KIT_SKILL;
    std::ostringstream raw;
    raw << skill.rdbuf();
    const std::string committed = normalize_newlines(raw.str());

    // Must carry Claude-Code frontmatter; otherwise a bare guide body would
    // silently pass the body comparison below.
    ASSERT_TRUE(committed.starts_with("---\n")) << "SKILL.md is missing YAML frontmatter";

    std::ostringstream guide;
    vectis::cli::print_guide(guide);

    EXPECT_EQ(trim(strip_frontmatter(committed)), trim(normalize_newlines(guide.str())));
}

} // namespace
