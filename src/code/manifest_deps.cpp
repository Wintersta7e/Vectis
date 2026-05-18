#include "code/manifest_deps.h"

#include <array>
#include <cctype>
#include <set>
#include <string_view>

#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

#include "core/string_util.h"
#include "platform/file_io.h"

namespace vectis::code::deps {

namespace {

/// Collect the keys of the `child_name` object on `parent` into `sink`.
/// No-op if the child is absent or not an object — matches the
/// "skip-and-keep-going" stance of the extractors as a whole.
void collect_object_keys(const nlohmann::json& parent, std::string_view child_name,
                         std::set<std::string>& sink)
{
    auto it = parent.find(child_name);
    if (it == parent.end() || !it->is_object()) {
        return;
    }
    for (const auto& [key, value] : it->items()) {
        sink.insert(key);
    }
}

/// Extract the leading package-name token from a PEP 508 requirement
/// string. PEP 508 starts with an identifier ([A-Za-z0-9._-]+) and
/// then optional extras / version / marker. Returning the identifier
/// alone gives the agent-facing dep name without forcing the matcher
/// to also strip version operators.
std::string extract_pep508_name(std::string_view spec)
{
    std::size_t start = spec.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) {
        return {};
    }
    std::size_t end = start;
    while (end < spec.size()) {
        const auto c = static_cast<unsigned char>(spec[end]);
        if (std::isalnum(c) == 0 && c != '.' && c != '_' && c != '-') {
            break;
        }
        ++end;
    }
    return std::string{spec.substr(start, end - start)};
}

/// Iterate a `toml::array` of strings and stuff each PEP 508 name
/// extracted from each entry into `sink`. Non-string entries are
/// skipped silently — a typoed dep list shouldn't fail a hint pass.
void collect_pep508_array(const toml::array& arr, std::set<std::string>& sink)
{
    for (const auto& node : arr) {
        if (const auto* str = node.as_string()) {
            auto name = extract_pep508_name(str->get());
            if (!name.empty()) {
                sink.insert(std::move(name));
            }
        }
    }
}

/// Iterate a `toml::table` and insert each key into `sink`. Used for
/// Poetry-style dep tables where keys ARE the package names. Skips
/// the Poetry-specific `python` entry — that's an interpreter
/// constraint, not a dependency.
void collect_table_keys(const toml::table& tbl, std::set<std::string>& sink,
                        bool exclude_poetry_python = false)
{
    for (const auto& [key, value] : tbl) {
        std::string k{key.str()};
        if (exclude_poetry_python && k == "python") {
            continue;
        }
        sink.insert(std::move(k));
    }
}

} // namespace

std::vector<std::string> extract_npm(const std::filesystem::path& package_json)
{
    auto contents = vectis::platform::read_file(package_json);
    if (!contents) {
        return {};
    }
    // `ignore_comments=true` — npm/pnpm/yarn forbid comments but some
    // toolchains (notably tsconfig-style configs and copies-of-templates)
    // ship `package.json` with `//` lines. We're hint-extracting, not
    // validating; ignore them.
    auto doc = nlohmann::json::parse(*contents, /*cb=*/nullptr,
                                     /*allow_exceptions=*/false,
                                     /*ignore_comments=*/true);
    if (doc.is_discarded() || !doc.is_object()) {
        return {};
    }

    std::set<std::string> deps;
    collect_object_keys(doc, "dependencies", deps);
    collect_object_keys(doc, "devDependencies", deps);
    collect_object_keys(doc, "peerDependencies", deps);
    collect_object_keys(doc, "optionalDependencies", deps);
    return {deps.begin(), deps.end()};
}

std::vector<std::string> extract_pyproject(const std::filesystem::path& pyproject_toml)
{
    toml::table table;
    try {
        table = toml::parse_file(pyproject_toml.string());
    }
    catch (const toml::parse_error&) {
        return {};
    }

    std::set<std::string> deps;

    // PEP 621: [project] dependencies (array of PEP 508 strings) +
    //          [project] optional-dependencies (table of arrays).
    if (auto* project = table["project"].as_table()) {
        if (auto* dep_arr = (*project)["dependencies"].as_array()) {
            collect_pep508_array(*dep_arr, deps);
        }
        if (auto* opt_tbl = (*project)["optional-dependencies"].as_table()) {
            for (const auto& [key, value] : *opt_tbl) {
                if (auto* arr = value.as_array()) {
                    collect_pep508_array(*arr, deps);
                }
            }
        }
    }

    // Poetry: [tool.poetry.dependencies] / dev-dependencies (legacy) /
    //         group.<name>.dependencies (current).
    if (auto* poetry = table["tool"]["poetry"].as_table()) {
        if (auto* dep_tbl = (*poetry)["dependencies"].as_table()) {
            collect_table_keys(*dep_tbl, deps, /*exclude_poetry_python=*/true);
        }
        if (auto* dev_tbl = (*poetry)["dev-dependencies"].as_table()) {
            collect_table_keys(*dev_tbl, deps);
        }
        if (auto* groups = (*poetry)["group"].as_table()) {
            for (const auto& [group_key, group_value] : *groups) {
                if (auto* group_tbl = group_value.as_table()) {
                    if (auto* gdeps = (*group_tbl)["dependencies"].as_table()) {
                        collect_table_keys(*gdeps, deps);
                    }
                }
            }
        }
    }

    return {deps.begin(), deps.end()};
}

std::vector<std::string> extract_cargo(const std::filesystem::path& cargo_toml)
{
    toml::table table;
    try {
        table = toml::parse_file(cargo_toml.string());
    }
    catch (const toml::parse_error&) {
        return {};
    }

    std::set<std::string> deps;
    static constexpr std::array<std::string_view, 3> k_dep_tables = {
        "dependencies", "dev-dependencies", "build-dependencies"};

    for (std::string_view table_name : k_dep_tables) {
        if (auto* tbl = table[table_name].as_table()) {
            collect_table_keys(*tbl, deps);
        }
    }

    return {deps.begin(), deps.end()};
}

namespace {

using vectis::core::trim_ascii;

/// Strip a trailing `// ...` line comment from `line` in-place.
void strip_line_comment(std::string_view& line) noexcept
{
    const std::size_t pos = line.find("//");
    if (pos != std::string_view::npos) {
        line = line.substr(0, pos);
    }
}

/// Module path from one go.mod `require` entry. Returns an empty view
/// if the line is a non-require statement or a bare comment.
std::string_view go_mod_module_path(std::string_view line)
{
    strip_line_comment(line);
    line = trim_ascii(line);
    if (line.starts_with("require ")) {
        line = trim_ascii(line.substr(8));
    }
    if (line.empty()) {
        return {};
    }
    const std::size_t sep = line.find_first_of(" \t");
    return sep == std::string_view::npos ? line : line.substr(0, sep);
}

} // namespace

std::vector<std::string> extract_go_mod(const std::filesystem::path& go_mod)
{
    auto contents = vectis::platform::read_file(go_mod);
    if (!contents) {
        return {};
    }
    const std::string& src = *contents;

    std::set<std::string> deps;
    bool in_block = false;
    std::size_t line_start = 0;
    for (std::size_t i = 0; i <= src.size(); ++i) {
        if (i != src.size() && src[i] != '\n') {
            continue;
        }
        std::string_view line{src.data() + line_start, i - line_start};
        line_start = i + 1;

        std::string_view trimmed = trim_ascii(line);
        strip_line_comment(trimmed);
        trimmed = trim_ascii(trimmed);
        if (trimmed.empty()) {
            continue;
        }

        if (in_block) {
            if (trimmed == ")") {
                in_block = false;
                continue;
            }
            const auto path = go_mod_module_path(trimmed);
            if (!path.empty()) {
                deps.insert(std::string{path});
            }
            continue;
        }
        if (trimmed.starts_with("require (")) {
            in_block = true;
            continue;
        }
        if (trimmed.starts_with("require ")) {
            const auto path = go_mod_module_path(trimmed);
            if (!path.empty()) {
                deps.insert(std::string{path});
            }
        }
    }
    return {deps.begin(), deps.end()};
}

std::vector<std::string> extract_composer(const std::filesystem::path& composer_json)
{
    auto contents = vectis::platform::read_file(composer_json);
    if (!contents) {
        return {};
    }
    auto doc = nlohmann::json::parse(*contents, /*cb=*/nullptr,
                                     /*allow_exceptions=*/false,
                                     /*ignore_comments=*/false);
    if (doc.is_discarded() || !doc.is_object()) {
        return {};
    }

    std::set<std::string> deps;
    // Composer's two top-level dep tables. Both keyed by
    // vendor/package; values are version constraint strings.
    collect_object_keys(doc, "require", deps);
    collect_object_keys(doc, "require-dev", deps);
    // Composer reserves a few keys with version constraints that
    // aren't packages — `php`, extensions (`ext-*`). Drop them so a
    // PHP version requirement isn't confused for a framework.
    for (auto it = deps.begin(); it != deps.end();) {
        if (*it == "php" || it->starts_with("ext-")) {
            it = deps.erase(it);
        }
        else {
            ++it;
        }
    }
    return {deps.begin(), deps.end()};
}

std::vector<std::string> extract_gemfile(const std::filesystem::path& gemfile)
{
    auto contents = vectis::platform::read_file(gemfile);
    if (!contents) {
        return {};
    }
    const std::string& src = *contents;

    std::set<std::string> deps;
    std::size_t line_start = 0;
    for (std::size_t i = 0; i <= src.size(); ++i) {
        if (i != src.size() && src[i] != '\n') {
            continue;
        }
        std::string_view line{src.data() + line_start, i - line_start};
        line_start = i + 1;

        // Strip a trailing `#` comment, then trim.
        const std::size_t hash = line.find('#');
        if (hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        line = trim_ascii(line);

        // Look for a `gem` token followed by a quoted string. The first
        // quoted argument after `gem` is the gem name; ignore version
        // constraints, source overrides, group options.
        if (!line.starts_with("gem ") && !line.starts_with("gem\t")) {
            continue;
        }
        std::string_view rest = trim_ascii(line.substr(3));
        if (rest.empty()) {
            continue;
        }
        const char quote = rest.front();
        if (quote != '"' && quote != '\'') {
            continue;
        }
        const std::size_t end = rest.find(quote, 1);
        if (end == std::string_view::npos) {
            continue;
        }
        deps.insert(std::string{rest.substr(1, end - 1)});
    }
    return {deps.begin(), deps.end()};
}

std::vector<std::string> extract_setup_py(const std::filesystem::path& setup_py)
{
    auto contents = vectis::platform::read_file(setup_py);
    if (!contents) {
        return {};
    }
    const std::string& src = *contents;

    // Locate the first literal `install_requires = [ ... ]` (or
    // `requires = [ ... ]`). Anything past the closing bracket is
    // ignored. We do NOT execute Python — list comprehensions,
    // concatenation, and read_requirements() calls all return empty.
    // `find_word_boundary` rejects the `requires` substring at the
    // tail of `install_requires`.
    std::set<std::string> deps;
    for (std::string_view key :
         {std::string_view{"install_requires"}, std::string_view{"requires"}}) {
        const std::size_t key_pos = vectis::core::find_word_boundary(src, key);
        if (key_pos == std::string::npos) {
            continue;
        }
        const std::size_t eq_pos = src.find('=', key_pos + key.size());
        if (eq_pos == std::string::npos) {
            continue;
        }
        const std::size_t open = src.find('[', eq_pos);
        if (open == std::string::npos) {
            continue;
        }
        const std::size_t close = src.find(']', open);
        if (close == std::string::npos) {
            continue;
        }
        const std::string_view body{src.data() + open + 1, close - open - 1};

        // Scan for quoted PEP 508 specifiers — single or double quotes.
        std::size_t i = 0;
        while (i < body.size()) {
            const char quote = body[i];
            if (quote != '"' && quote != '\'') {
                ++i;
                continue;
            }
            const std::size_t start = i + 1;
            const std::size_t end = body.find(quote, start);
            if (end == std::string_view::npos) {
                break;
            }
            auto name = extract_pep508_name(body.substr(start, end - start));
            if (!name.empty()) {
                deps.insert(std::move(name));
            }
            i = end + 1;
        }
    }
    return {deps.begin(), deps.end()};
}

namespace {

/// Normalise a python distribution name per PEP 503: lowercase, then
/// collapse runs of `[_.-]` to a single `-`. The framework_hints py
/// table is keyed by the canonical lowercase short name (e.g.
/// `django`, `flask`) so a `Django==1.10` line in requirements.txt
/// must canonicalise to `django` to match.
[[nodiscard]] std::string normalise_pep503(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size());
    bool last_was_sep = false;
    for (const char c : raw) {
        if (c == '_' || c == '.' || c == '-') {
            if (!last_was_sep && !out.empty()) {
                out.push_back('-');
            }
            last_was_sep = true;
        }
        else {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            last_was_sep = false;
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out;
}

} // namespace

std::vector<std::string> extract_requirements_txt(const std::filesystem::path& requirements_txt)
{
    auto contents = vectis::platform::read_file(requirements_txt);
    if (!contents) {
        return {};
    }
    const std::string& src = *contents;

    std::set<std::string> deps;
    std::size_t pos = 0;
    while (pos < src.size()) {
        const std::size_t line_end = src.find('\n', pos);
        std::string_view line{src.data() + pos,
                              (line_end == std::string::npos ? src.size() : line_end) - pos};
        pos = (line_end == std::string::npos) ? src.size() : line_end + 1;

        // Strip a trailing `\r` (Windows line endings).
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        // Strip a `#` comment, then leading + trailing whitespace.
        if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            continue;
        }
        // Skip include / constraint / editable / VCS-only lines —
        // they don't carry a parseable short name.
        if (line.starts_with('-') || line.starts_with("git+") || line.starts_with("hg+") ||
            line.starts_with("svn+") || line.starts_with("bzr+") || line.starts_with("http") ||
            line.starts_with("file:")) {
            continue;
        }
        // The package name ends at the first occurrence of any version
        // / extras / env-marker / URL delimiter character.
        std::size_t name_end = 0;
        for (; name_end < line.size(); ++name_end) {
            const char c = line[name_end];
            if (c == '=' || c == '<' || c == '>' || c == '!' || c == '~' || c == '[' || c == ';' ||
                c == ' ' || c == '\t' || c == '@') {
                break;
            }
        }
        const std::string_view name_raw = line.substr(0, name_end);
        std::string canonical = normalise_pep503(name_raw);
        if (!canonical.empty()) {
            deps.insert(std::move(canonical));
        }
    }
    return {deps.begin(), deps.end()};
}

} // namespace vectis::code::deps
