#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "code/language.h"

namespace vectis::code {

/// Kinds of symbols Vectis extracts from source code.
///
/// Step 2 targets only the coarse-grained entities needed for a
/// navigable symbol browser. Finer distinctions (constructors,
/// operators, templates, macros) can be added in later steps
/// without a migration because callers always switch on this enum.
enum class SymbolKind : std::uint8_t
{
    Unknown = 0,
    Function,
    Method,
    Class,
    Struct,
    Interface,
    Enum,
    Type,
    Namespace,
    /// Synthetic per-file symbol attached by the manifest scanner so
    /// agents can locate the POM / csproj / Spring XML / properties
    /// file that registered a dependency edge. One Manifest symbol per
    /// manifest file; carries no signature, members, or complexity.
    Manifest,
};

/// Short human-readable name for a symbol kind, used by the symbol
/// browser for a column label and by the digest export in Step 3.
[[nodiscard]] constexpr std::string_view symbol_kind_name(SymbolKind kind) noexcept
{
    switch (kind) {
    case SymbolKind::Function:
        return "function";
    case SymbolKind::Method:
        return "method";
    case SymbolKind::Class:
        return "class";
    case SymbolKind::Struct:
        return "struct";
    case SymbolKind::Interface:
        return "interface";
    case SymbolKind::Enum:
        return "enum";
    case SymbolKind::Type:
        return "type";
    case SymbolKind::Namespace:
        return "namespace";
    case SymbolKind::Manifest:
        return "manifest";
    case SymbolKind::Unknown:
        return "unknown";
    }
    return "unknown";
}

/// One source file registered with the CodeIndex.
///
/// `path_relative` is relative to the project root the scanner was
/// given, so UIs and serializers can render it without leaking the
/// user's local filesystem prefix. `id` is assigned by
/// `CodeIndex::add_file` — external code must never set it.
struct FileEntry
{
    std::int64_t id = 0;
    std::filesystem::path path_relative;
    Language language = Language::Unknown;
    std::uint64_t size = 0;
    int line_count = 0;
    std::filesystem::file_time_type last_modified;
    std::string content_hash;
    /// Namespaces / packages this file declares (Java `package`, C#
    /// `namespace`, PHP `namespace`, Python module path, …). Kept on
    /// the file so an incremental rescan can rebuild the resolver's
    /// namespace index for unchanged files without re-parsing them.
    std::vector<std::string> declared_namespaces;
};

/// Reverse of `symbol_kind_name`: parses a string back to a SymbolKind.
/// Returns `SymbolKind::Unknown` if the name is not recognized.
[[nodiscard]] constexpr SymbolKind symbol_kind_from_name(std::string_view name) noexcept
{
    if (name == "function") {
        return SymbolKind::Function;
    }
    if (name == "method") {
        return SymbolKind::Method;
    }
    if (name == "class") {
        return SymbolKind::Class;
    }
    if (name == "struct") {
        return SymbolKind::Struct;
    }
    if (name == "interface") {
        return SymbolKind::Interface;
    }
    if (name == "enum") {
        return SymbolKind::Enum;
    }
    if (name == "type") {
        return SymbolKind::Type;
    }
    if (name == "namespace") {
        return SymbolKind::Namespace;
    }
    if (name == "manifest") {
        return SymbolKind::Manifest;
    }
    return SymbolKind::Unknown;
}

/// Visibility / API-surface category for a symbol. Languages encode this
/// very differently — Go via capitalisation, Python via underscore
/// convention, Rust via `pub` keywords, Java/C#/TS via explicit modifiers
/// — so the parser collapses each language's notion into this small fixed
/// set. A consumer that just wants "is this part of the public API" can
/// compare against `Visibility::Public`.
///
///   Public    — exported / part of the documented API.
///   Private   — internal / hidden / leading underscore / lowercase
///               in Go / no `pub` in Rust / explicit `private`.
///   Protected — Java/C#/TS `protected` member.
///   Internal  — C# `internal`, Rust `pub(crate)` / `pub(super)`,
///               Java package-private.
///   Unknown   — language doesn't express visibility / not yet
///               implemented for the language. Treat as Public when
///               filtering. Round-trips to/from the empty string so the
///               on-disk SQLite TEXT column needs no migration.
enum class Visibility : std::uint8_t
{
    Unknown = 0,
    Public,
    Private,
    Protected,
    Internal,
};

/// Short human-readable name for a visibility, used by the digest
/// exporter and `vectis explain`. `Unknown` maps to the empty string so
/// JSON consumers see no `"visibility"` field and the SQLite column's
/// `DEFAULT ''` round-trips losslessly.
[[nodiscard]] constexpr std::string_view visibility_name(Visibility v) noexcept
{
    switch (v) {
    case Visibility::Public:
        return "public";
    case Visibility::Private:
        return "private";
    case Visibility::Protected:
        return "protected";
    case Visibility::Internal:
        return "internal";
    case Visibility::Unknown:
        return "";
    }
    return "";
}

/// Reverse of `visibility_name`. Empty string and unrecognised names
/// both map to `Visibility::Unknown`.
[[nodiscard]] constexpr Visibility visibility_from_name(std::string_view name) noexcept
{
    if (name == "public") {
        return Visibility::Public;
    }
    if (name == "private") {
        return Visibility::Private;
    }
    if (name == "protected") {
        return Visibility::Protected;
    }
    if (name == "internal") {
        return Visibility::Internal;
    }
    return Visibility::Unknown;
}

/// One symbol extracted from a source file.
///
/// `file_id` references `FileEntry::id`. Line numbers are 1-based to
/// match what editors and error messages use.
///
/// `signature` is populated for function and method kinds with the
/// source text between the node start and the body (or trailing
/// semicolon for declarations), whitespace-normalized. Empty for
/// everything else.
///
/// `members` is populated for enum-like symbols (holding enumerator
/// names) and plain-old-data struct symbols (holding public field
/// names). Empty for classes (whose public surface is already
/// exposed via their method symbols), for free functions, and for
/// namespaces.
///
/// `complexity` holds the cyclomatic complexity for Function and
/// Method kinds — 1 for a straight-line function, incrementing for
/// each decision point (if/while/for/case/&&/||/?:). Zero for every
/// non-function kind.
struct Symbol
{
    std::int64_t id = 0;
    std::int64_t file_id = 0;
    std::string name;
    SymbolKind kind = SymbolKind::Unknown;
    int line_start = 0;
    int line_end = 0;
    std::int64_t parent_id = 0; // 0 if top-level
    std::string signature;
    std::vector<std::string> members;
    int complexity = 0;
    Visibility visibility = Visibility::Unknown;
    /// Decorator / annotation text attached to this symbol, in source
    /// order, with the leading `@` stripped. Populated for languages
    /// where decorators are a first-class language feature; empty for
    /// the rest. Useful for filtering "all HTTP route handlers"
    /// (`app.route(...)` in flask), "all tests" (`pytest.fixture`),
    /// dependency-injection markers, etc.
    std::vector<std::string> decorators;
};

} // namespace vectis::code
