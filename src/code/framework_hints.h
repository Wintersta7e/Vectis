#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vectis::code::hints {

/// Coarse, additive corroborators emitted by the dep-keyword matcher.
/// A single project can yield multiple hints — a Java backend with a
/// React frontend produces both `WebBackend` and `WebFrontend`. The
/// architecture detector consumes these as evidence in its existing
/// branches; it does NOT introduce new primary labels off them.
enum class FrameworkHint : std::uint8_t
{
    WebBackend,  ///< Server-side HTTP framework (Spring Boot, Django, Express, …).
    WebFrontend, ///< Browser-rendering framework (React, Vue, Svelte, Angular, …).
    DesktopUI,   ///< Native or quasi-native desktop UI (Electron, Tauri, Avalonia, MAUI).
    /// Enterprise Integration Patterns runtime (Apache Camel, Spring
    /// Integration, Mule, ServiceMix). These projects route and
    /// transform messages between systems rather than serving HTTP
    /// requests directly — they share the `handlers/` / `routes/`
    /// shape with API backends but are functionally distinct, so the
    /// architecture detector uses this hint to disambiguate the label.
    Integration,
};

/// Build/runtime ecosystem the keyword table is keyed off. Coarser
/// than `architecture_detector`'s internal `Runtime` because the
/// matcher cares only about which keyword table to consult — Maven
/// and Gradle Java projects look the same to the framework matcher;
/// pyproject and setup.py both feed the Python table.
enum class Ecosystem : std::uint8_t
{
    Npm,       ///< `package.json` deps (npm/pnpm/yarn).
    Pyproject, ///< pyproject.toml / setup.py extracted PEP 508 names.
    Cargo,     ///< `Cargo.toml` `[dependencies]` keys.
    GoMod,     ///< `go.mod` `require` module paths.
    Composer,  ///< `composer.json` `require` / `require-dev` keys.
    Gemfile,   ///< `Gemfile` `gem '…'` names.
    Maven,  ///< pom.xml / build.gradle{,.kts} (Maven graph edges, `groupId:artifactId[:version]`).
    DotNet, ///< .csproj / .sln (PackageReference, `<Name>:<Version>`).
};

/// One token per hint, formatted for the `signals[]` array. Tokens use
/// the existing `category:value` convention so agents can grep them
/// alongside `dir:*` and `manifest:*` tokens.
[[nodiscard]] std::string_view hint_signal(FrameworkHint h) noexcept;

/// Match `deps` against the keyword table for `ecosystem` and return
/// the deduped set of fired hints. The matcher normalises each
/// dep string per-ecosystem before lookup:
///   * Maven: `groupId:artifactId:version` → `groupId:artifactId`
///   * DotNet: `PackageId:Version` → `PackageId`
///   * GoMod: trailing `/v<N>` major-version suffix stripped
///   * other ecosystems: passed through verbatim
[[nodiscard]] std::vector<FrameworkHint> match(Ecosystem ecosystem,
                                               std::span<const std::string> deps);

/// Tally framework annotations across the symbol index and return the
/// hints corroborated by the tally. Independent from `match()` — it
/// works on the symbol AST's `decorators[]` field, so it fires on
/// projects whose manifest we can't read (custom internal framework,
/// missing dep declarations, unsupported manifest format).
///
/// `annotations` is a flat list of decorator strings as the parser
/// stores them (language sigils already stripped — `@RestController`
/// arrives as `RestController`, `@app.route("/")` as `app.route("/")`,
/// `#[get("/users")]` as `get("/users")`). The matcher strips the
/// trailing `(...)` call expression before lookup.
///
/// A hint fires only when at least three symbols carry a matching
/// annotation. The threshold suppresses a false positive from a
/// project that happens to define a single custom `@RestController`-
/// named annotation; a real REST API has dozens of route handlers.
[[nodiscard]] std::vector<FrameworkHint>
match_annotations(std::span<const std::string> annotations);

} // namespace vectis::code::hints
