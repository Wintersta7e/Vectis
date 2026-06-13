#include <array>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "code/code_index.h"
#include "code/dependency.h"
#include "code/digest_exporter.h"
#include "code/fidelity.h"
#include "code/language.h"

namespace {

using vectis::code::build_digest_string;
using vectis::code::build_fidelity_metadata_json;
using vectis::code::c_cpp_edge_confidence;
using vectis::code::CodeIndex;
using vectis::code::csharp_edge_confidence;
using vectis::code::Dependency;
using vectis::code::DigestFormat;
using vectis::code::EdgeFidelity;
using vectis::code::ExportOptions;
using vectis::code::FileEntry;
using vectis::code::go_edge_confidence;
using vectis::code::java_edge_confidence;
using vectis::code::jsts_edge_confidence;
using vectis::code::k_cinclude_external_bare_confidence;
using vectis::code::k_cinclude_external_path_confidence;
using vectis::code::k_cinclude_resolved_bare_confidence;
using vectis::code::k_cinclude_resolved_path_confidence;
using vectis::code::k_cs_external_system_confidence;
using vectis::code::k_cs_external_thirdparty_confidence;
using vectis::code::k_cs_internal_confidence;
using vectis::code::k_go_external_stdlib_confidence;
using vectis::code::k_go_external_thirdparty_confidence;
using vectis::code::k_go_internal_confidence;
using vectis::code::k_java_dotted_resolved_confidence;
using vectis::code::k_java_external_innertype_confidence;
using vectis::code::k_java_external_jdk_confidence;
using vectis::code::k_java_external_thirdparty_confidence;
using vectis::code::k_java_wildcard_resolved_confidence;
using vectis::code::k_jsts_alias_unresolved_confidence;
using vectis::code::k_jsts_bare_external_confidence;
using vectis::code::k_jsts_relative_resolved_confidence;
using vectis::code::k_jsts_relative_unresolved_confidence;
using vectis::code::k_php_require_external_confidence;
using vectis::code::k_php_require_resolved_confidence;
using vectis::code::k_php_use_external_global_confidence;
using vectis::code::k_php_use_external_namespaced_confidence;
using vectis::code::k_php_use_nsindex_fanout_confidence;
using vectis::code::k_php_use_psr4_confidence;
using vectis::code::k_py_external_dotted_confidence;
using vectis::code::k_py_external_relative_confidence;
using vectis::code::k_py_resolved_confidence;
using vectis::code::k_ruby_external_gem_confidence;
using vectis::code::k_ruby_external_stdlib_confidence;
using vectis::code::k_ruby_relative_explicit_confidence;
using vectis::code::k_ruby_resolved_multi_confidence;
using vectis::code::k_ruby_resolved_single_confidence;
using vectis::code::k_rust_mod_confidence;
using vectis::code::k_rust_mod_unresolved_confidence;
using vectis::code::k_rust_use_extern_confidence;
using vectis::code::k_rust_use_internal_resolved_confidence;
using vectis::code::k_rust_use_internal_unresolved_confidence;
using vectis::code::k_rust_use_std_confidence;
using vectis::code::Language;
using vectis::code::php_edge_confidence;
using vectis::code::python_edge_confidence;
using vectis::code::reconstruct_c_cpp_resolved_by;
using vectis::code::reconstruct_csharp_resolved_by;
using vectis::code::reconstruct_edge_fidelity;
using vectis::code::reconstruct_go_resolved_by;
using vectis::code::reconstruct_java_resolved_by;
using vectis::code::reconstruct_jsts_resolved_by;
using vectis::code::reconstruct_php_resolved_by;
using vectis::code::reconstruct_python_resolved_by;
using vectis::code::reconstruct_ruby_resolved_by;
using vectis::code::reconstruct_rust_resolved_by;
using vectis::code::ruby_edge_confidence;
using vectis::code::rust_edge_confidence;

// --- Strategy reconstruction -------------------------------------------------

TEST(FidelityTest, Reconstruct_RelativeModule)
{
    // from .x  ->  pkg/x.py  (resolved, module)
    EXPECT_EQ(reconstruct_python_resolved_by(".x", "pkg/x.py", /*is_external=*/false),
              "relative-module");
}

TEST(FidelityTest, Reconstruct_RelativePackage)
{
    // from .x  ->  pkg/x/__init__.py  (resolved, package)
    EXPECT_EQ(reconstruct_python_resolved_by(".x", "pkg/x/__init__.py", /*is_external=*/false),
              "relative-package");
}

TEST(FidelityTest, Reconstruct_DottedModule)
{
    // import a.b  ->  a/b.py  (resolved, module)
    EXPECT_EQ(reconstruct_python_resolved_by("a.b", "a/b.py", /*is_external=*/false),
              "dotted-module");
}

TEST(FidelityTest, Reconstruct_DottedPackage)
{
    // import a.b  ->  a/b/__init__.py  (resolved, package)
    EXPECT_EQ(reconstruct_python_resolved_by("a.b", "a/b/__init__.py", /*is_external=*/false),
              "dotted-package");
}

TEST(FidelityTest, Reconstruct_ExternalDotted)
{
    // Unresolved, no leading dot -> external-dotted. Target path is
    // ignored when external.
    EXPECT_EQ(reconstruct_python_resolved_by("numpy", "", /*is_external=*/true), "external-dotted");
    EXPECT_EQ(reconstruct_python_resolved_by("os.path", "ignored.py", /*is_external=*/true),
              "external-dotted");
}

TEST(FidelityTest, Reconstruct_ExternalRelative)
{
    // Unresolved, leading dot -> external-relative.
    EXPECT_EQ(reconstruct_python_resolved_by(".missing", "", /*is_external=*/true),
              "external-relative");
}

TEST(FidelityTest, Reconstruct_EmptyImportStringIsDotted)
{
    // No import string can't be relative (no leading dot); treat as dotted.
    EXPECT_EQ(reconstruct_python_resolved_by("", "", /*is_external=*/true), "external-dotted");
    EXPECT_EQ(reconstruct_python_resolved_by("", "a/b.py", /*is_external=*/false), "dotted-module");
}

TEST(FidelityTest, Reconstruct_GoInternal)
{
    // Resolved Go import (matched the go.mod prefix) -> go-internal.
    EXPECT_EQ(reconstruct_go_resolved_by("example.com/app/handler", /*is_external=*/false),
              "go-internal");
}

TEST(FidelityTest, Reconstruct_GoExternalStdlib)
{
    // Unresolved, first path segment has no dot -> standard library.
    EXPECT_EQ(reconstruct_go_resolved_by("fmt", /*is_external=*/true), "go-external-stdlib");
    EXPECT_EQ(reconstruct_go_resolved_by("database/sql", /*is_external=*/true),
              "go-external-stdlib");
}

TEST(FidelityTest, Reconstruct_GoExternalThirdparty)
{
    // Unresolved, first path segment is a domain -> third-party.
    EXPECT_EQ(reconstruct_go_resolved_by("github.com/gin-gonic/gin", /*is_external=*/true),
              "go-external-thirdparty");
    EXPECT_EQ(reconstruct_go_resolved_by("golang.org/x/sync/errgroup", /*is_external=*/true),
              "go-external-thirdparty");
}

TEST(FidelityTest, Reconstruct_RustMod)
{
    // `mod x;` resolved vs left external (inline / cfg-gated / dir-module).
    EXPECT_EQ(reconstruct_rust_resolved_by("mod", "doc", /*is_external=*/false), "rust-mod");
    EXPECT_EQ(reconstruct_rust_resolved_by("mod", "doc", /*is_external=*/true),
              "rust-mod-unresolved");
}

TEST(FidelityTest, Reconstruct_RustUse)
{
    // std/core/alloc first segment -> rust-use-std regardless of resolved status.
    EXPECT_EQ(reconstruct_rust_resolved_by("use", "std::io", /*is_external=*/true), "rust-use-std");
    EXPECT_EQ(reconstruct_rust_resolved_by("use", "core::mem", /*is_external=*/true),
              "rust-use-std");
    // crate/self/super: unresolved (is_external=true) stays low.
    EXPECT_EQ(reconstruct_rust_resolved_by("use", "crate::foo::Bar", /*is_external=*/true),
              "rust-use-internal-unresolved");
    EXPECT_EQ(reconstruct_rust_resolved_by("use", "super::x", /*is_external=*/true),
              "rust-use-internal-unresolved");
    // External third-party crate path -> rust-use-extern.
    EXPECT_EQ(reconstruct_rust_resolved_by("use", "serde::Serialize", /*is_external=*/true),
              "rust-use-extern");
}

TEST(FidelityTest, RustUseInternalSplitsOnResolvedStatus)
{
    // Resolved in-crate use (is_external=false): high confidence by construction.
    const std::string r =
        reconstruct_rust_resolved_by("use", "crate::net::tcp", /*is_external=*/false);
    EXPECT_EQ(r, "rust-use-internal-resolved");
    EXPECT_GT(rust_edge_confidence(r), 0.5);
    // Unresolved (is_external=true): residual gap — stays low.
    const std::string u =
        reconstruct_rust_resolved_by("use", "crate::macro_made", /*is_external=*/true);
    EXPECT_EQ(u, "rust-use-internal-unresolved");
    EXPECT_LT(rust_edge_confidence(u), 0.5);
}

TEST(FidelityTest, Reconstruct_CInclude)
{
    // resolved/external × path/bare, split on a directory part in the include.
    EXPECT_EQ(reconstruct_c_cpp_resolved_by("a/b.h", /*is_external=*/false),
              "cinclude-resolved-path");
    EXPECT_EQ(reconstruct_c_cpp_resolved_by("b.h", /*is_external=*/false),
              "cinclude-resolved-bare");
    EXPECT_EQ(reconstruct_c_cpp_resolved_by("absl/strings/str_cat.h", /*is_external=*/true),
              "cinclude-external-path");
    EXPECT_EQ(reconstruct_c_cpp_resolved_by("xxhash.h", /*is_external=*/true),
              "cinclude-external-bare");
}

TEST(FidelityTest, Reconstruct_Jsts)
{
    // Only relative imports resolve; bare/alias never do.
    EXPECT_EQ(reconstruct_jsts_resolved_by("./util", /*is_external=*/false),
              "jsts-relative-resolved");
    EXPECT_EQ(reconstruct_jsts_resolved_by("./missing", /*is_external=*/true),
              "jsts-relative-unresolved");
    // `@/` / `~` / `#` are tsconfig path-alias roots, not npm packages.
    EXPECT_EQ(reconstruct_jsts_resolved_by("@/components/Btn", /*is_external=*/true),
              "jsts-alias-unresolved");
    EXPECT_EQ(reconstruct_jsts_resolved_by("~/store", /*is_external=*/true),
              "jsts-alias-unresolved");
    // A scoped npm package (`@scope/pkg`) is bare-external, NOT an alias.
    EXPECT_EQ(reconstruct_jsts_resolved_by("@angular/core", /*is_external=*/true),
              "jsts-bare-external");
    EXPECT_EQ(reconstruct_jsts_resolved_by("react", /*is_external=*/true), "jsts-bare-external");
}

TEST(FidelityTest, Reconstruct_Java)
{
    // Resolved: last segment Uppercase = specific class; lowercase = bare
    // package (the wildcard case, since `.*` is dropped at parse).
    EXPECT_EQ(reconstruct_java_resolved_by("com.app.Owner", /*is_external=*/false),
              "java-dotted-resolved");
    EXPECT_EQ(reconstruct_java_resolved_by("com.app.model", /*is_external=*/false),
              "java-wildcard-resolved");
    // External: JDK takes precedence (even for JDK inner types, which are
    // reliably external); non-JDK Outer.Inner = innertype; else third-party.
    EXPECT_EQ(reconstruct_java_resolved_by("java.util.List", /*is_external=*/true),
              "java-external-jdk");
    EXPECT_EQ(reconstruct_java_resolved_by("java.util.Map.Entry", /*is_external=*/true),
              "java-external-jdk");
    EXPECT_EQ(reconstruct_java_resolved_by("org.springframework.web.bind.annotation.RestController",
                                           /*is_external=*/true),
              "java-external-thirdparty");
    EXPECT_EQ(reconstruct_java_resolved_by("com.google.gson.stream.JsonScope.EMPTY_ARRAY",
                                           /*is_external=*/true),
              "java-external-innertype");
}

TEST(FidelityTest, Reconstruct_Csharp)
{
    EXPECT_EQ(reconstruct_csharp_resolved_by("MyApp.Services", /*is_external=*/false),
              "csharp-internal");
    EXPECT_EQ(reconstruct_csharp_resolved_by("System.Text", /*is_external=*/true),
              "csharp-external-system");
    EXPECT_EQ(reconstruct_csharp_resolved_by("Microsoft.Extensions.Logging", /*is_external=*/true),
              "csharp-external-system");
    EXPECT_EQ(reconstruct_csharp_resolved_by("Newtonsoft.Json", /*is_external=*/true),
              "csharp-external-thirdparty");
}

TEST(FidelityTest, Reconstruct_Php)
{
    // require/include are path-based.
    EXPECT_EQ(reconstruct_php_resolved_by("/helpers.php", "src/helpers.php", "require",
                                          /*is_external=*/false),
              "php-require-resolved");
    EXPECT_EQ(reconstruct_php_resolved_by("vendor/autoload.php", "", "require",
                                          /*is_external=*/true),
              "php-require-external");
    // `use`, resolved: exact PSR-4 path match vs the lossy namespace-index fanout.
    EXPECT_EQ(reconstruct_php_resolved_by("Slim\\Factory\\Foo", "src/Slim/Factory/Foo.php", "use",
                                          /*is_external=*/false),
              "php-use-psr4-exact");
    EXPECT_EQ(reconstruct_php_resolved_by("Illuminate\\Support\\Collection",
                                          "src/Collections/Collection.php", "use",
                                          /*is_external=*/false),
              "php-use-nsindex-fanout");
    // `use`, external: namespaced (`\`) vs a root/global symbol.
    EXPECT_EQ(reconstruct_php_resolved_by("Psr\\Log\\LoggerInterface", "", "use",
                                          /*is_external=*/true),
              "php-use-external-namespaced");
    EXPECT_EQ(reconstruct_php_resolved_by("Closure", "", "use", /*is_external=*/true),
              "php-use-external-global");
}

TEST(FidelityTest, Reconstruct_Ruby)
{
    // Resolved: explicit-relative / multi-segment / single-segment.
    EXPECT_EQ(reconstruct_ruby_resolved_by("../foo", /*is_external=*/false),
              "ruby-relative-explicit");
    EXPECT_EQ(reconstruct_ruby_resolved_by("sinatra/base", /*is_external=*/false),
              "ruby-resolved-multi");
    EXPECT_EQ(reconstruct_ruby_resolved_by("helper", /*is_external=*/false),
              "ruby-resolved-single");
    // External: a stdlib first segment vs a gem.
    EXPECT_EQ(reconstruct_ruby_resolved_by("json", /*is_external=*/true), "ruby-external-stdlib");
    EXPECT_EQ(reconstruct_ruby_resolved_by("set", /*is_external=*/true), "ruby-external-stdlib");
    EXPECT_EQ(reconstruct_ruby_resolved_by("rack/protection", /*is_external=*/true),
              "ruby-external-gem");
}

// --- Confidence lookup -------------------------------------------------------

TEST(FidelityTest, Confidence_ResolvedStrategies)
{
    EXPECT_DOUBLE_EQ(python_edge_confidence("relative-module"), k_py_resolved_confidence);
    EXPECT_DOUBLE_EQ(python_edge_confidence("relative-package"), k_py_resolved_confidence);
    EXPECT_DOUBLE_EQ(python_edge_confidence("dotted-module"), k_py_resolved_confidence);
    EXPECT_DOUBLE_EQ(python_edge_confidence("dotted-package"), k_py_resolved_confidence);
}

TEST(FidelityTest, Confidence_ExternalStrategies)
{
    EXPECT_DOUBLE_EQ(python_edge_confidence("external-relative"),
                     k_py_external_relative_confidence);
    EXPECT_DOUBLE_EQ(python_edge_confidence("external-dotted"), k_py_external_dotted_confidence);
}

TEST(FidelityTest, Confidence_UnknownFailsClosed)
{
    // An unrecognised strategy must not inherit a neighbour's number.
    EXPECT_DOUBLE_EQ(python_edge_confidence("not-a-strategy"), 0.0);
    EXPECT_DOUBLE_EQ(python_edge_confidence(""), 0.0);
}

TEST(FidelityTest, Confidence_GoStrategies)
{
    EXPECT_DOUBLE_EQ(go_edge_confidence("go-internal"), k_go_internal_confidence);
    EXPECT_DOUBLE_EQ(go_edge_confidence("go-external-stdlib"), k_go_external_stdlib_confidence);
    EXPECT_DOUBLE_EQ(go_edge_confidence("go-external-thirdparty"),
                     k_go_external_thirdparty_confidence);
}

TEST(FidelityTest, Confidence_GoUnknownFailsClosed)
{
    // Python strata are not valid Go strategies and vice versa: each lookup
    // only knows its own taxonomy and fails closed otherwise.
    EXPECT_DOUBLE_EQ(go_edge_confidence("relative-module"), 0.0);
    EXPECT_DOUBLE_EQ(go_edge_confidence("not-a-strategy"), 0.0);
    EXPECT_DOUBLE_EQ(go_edge_confidence(""), 0.0);
}

TEST(FidelityTest, Confidence_RustStrategies)
{
    EXPECT_DOUBLE_EQ(rust_edge_confidence("rust-mod"), k_rust_mod_confidence);
    EXPECT_DOUBLE_EQ(rust_edge_confidence("rust-mod-unresolved"), k_rust_mod_unresolved_confidence);
    EXPECT_DOUBLE_EQ(rust_edge_confidence("rust-use-std"), k_rust_use_std_confidence);
    EXPECT_DOUBLE_EQ(rust_edge_confidence("rust-use-internal-resolved"),
                     k_rust_use_internal_resolved_confidence);
    EXPECT_DOUBLE_EQ(rust_edge_confidence("rust-use-internal-unresolved"),
                     k_rust_use_internal_unresolved_confidence);
    EXPECT_DOUBLE_EQ(rust_edge_confidence("rust-use-extern"), k_rust_use_extern_confidence);
    // Old single-stratum key is gone; must fail closed.
    EXPECT_DOUBLE_EQ(rust_edge_confidence("rust-use-internal"), 0.0);
    EXPECT_DOUBLE_EQ(rust_edge_confidence("not-a-strategy"), 0.0);
}

TEST(FidelityTest, Confidence_CIncludeStrategies)
{
    EXPECT_DOUBLE_EQ(c_cpp_edge_confidence("cinclude-resolved-path"),
                     k_cinclude_resolved_path_confidence);
    EXPECT_DOUBLE_EQ(c_cpp_edge_confidence("cinclude-resolved-bare"),
                     k_cinclude_resolved_bare_confidence);
    EXPECT_DOUBLE_EQ(c_cpp_edge_confidence("cinclude-external-path"),
                     k_cinclude_external_path_confidence);
    EXPECT_DOUBLE_EQ(c_cpp_edge_confidence("cinclude-external-bare"),
                     k_cinclude_external_bare_confidence);
    EXPECT_DOUBLE_EQ(c_cpp_edge_confidence("not-a-strategy"), 0.0);
}

TEST(FidelityTest, Confidence_JstsStrategies)
{
    EXPECT_DOUBLE_EQ(jsts_edge_confidence("jsts-relative-resolved"),
                     k_jsts_relative_resolved_confidence);
    EXPECT_DOUBLE_EQ(jsts_edge_confidence("jsts-relative-unresolved"),
                     k_jsts_relative_unresolved_confidence);
    EXPECT_DOUBLE_EQ(jsts_edge_confidence("jsts-alias-unresolved"),
                     k_jsts_alias_unresolved_confidence);
    EXPECT_DOUBLE_EQ(jsts_edge_confidence("jsts-bare-external"), k_jsts_bare_external_confidence);
    EXPECT_DOUBLE_EQ(jsts_edge_confidence("not-a-strategy"), 0.0);
}

TEST(FidelityTest, Confidence_JavaStrategies)
{
    EXPECT_DOUBLE_EQ(java_edge_confidence("java-dotted-resolved"),
                     k_java_dotted_resolved_confidence);
    EXPECT_DOUBLE_EQ(java_edge_confidence("java-wildcard-resolved"),
                     k_java_wildcard_resolved_confidence);
    EXPECT_DOUBLE_EQ(java_edge_confidence("java-external-jdk"), k_java_external_jdk_confidence);
    EXPECT_DOUBLE_EQ(java_edge_confidence("java-external-thirdparty"),
                     k_java_external_thirdparty_confidence);
    EXPECT_DOUBLE_EQ(java_edge_confidence("java-external-innertype"),
                     k_java_external_innertype_confidence);
    EXPECT_DOUBLE_EQ(java_edge_confidence("not-a-strategy"), 0.0);
}

TEST(FidelityTest, Confidence_CsharpStrategies)
{
    EXPECT_DOUBLE_EQ(csharp_edge_confidence("csharp-internal"), k_cs_internal_confidence);
    EXPECT_DOUBLE_EQ(csharp_edge_confidence("csharp-external-system"),
                     k_cs_external_system_confidence);
    EXPECT_DOUBLE_EQ(csharp_edge_confidence("csharp-external-thirdparty"),
                     k_cs_external_thirdparty_confidence);
    EXPECT_DOUBLE_EQ(csharp_edge_confidence("not-a-strategy"), 0.0);
}

TEST(FidelityTest, Confidence_PhpStrategies)
{
    EXPECT_DOUBLE_EQ(php_edge_confidence("php-require-resolved"),
                     k_php_require_resolved_confidence);
    EXPECT_DOUBLE_EQ(php_edge_confidence("php-require-external"),
                     k_php_require_external_confidence);
    EXPECT_DOUBLE_EQ(php_edge_confidence("php-use-psr4-exact"), k_php_use_psr4_confidence);
    EXPECT_DOUBLE_EQ(php_edge_confidence("php-use-nsindex-fanout"),
                     k_php_use_nsindex_fanout_confidence);
    EXPECT_DOUBLE_EQ(php_edge_confidence("php-use-external-global"),
                     k_php_use_external_global_confidence);
    EXPECT_DOUBLE_EQ(php_edge_confidence("php-use-external-namespaced"),
                     k_php_use_external_namespaced_confidence);
    EXPECT_DOUBLE_EQ(php_edge_confidence("not-a-strategy"), 0.0);
}

TEST(FidelityTest, Confidence_RubyStrategies)
{
    EXPECT_DOUBLE_EQ(ruby_edge_confidence("ruby-relative-explicit"),
                     k_ruby_relative_explicit_confidence);
    EXPECT_DOUBLE_EQ(ruby_edge_confidence("ruby-resolved-multi"), k_ruby_resolved_multi_confidence);
    EXPECT_DOUBLE_EQ(ruby_edge_confidence("ruby-resolved-single"),
                     k_ruby_resolved_single_confidence);
    EXPECT_DOUBLE_EQ(ruby_edge_confidence("ruby-external-stdlib"),
                     k_ruby_external_stdlib_confidence);
    EXPECT_DOUBLE_EQ(ruby_edge_confidence("ruby-external-gem"), k_ruby_external_gem_confidence);
    EXPECT_DOUBLE_EQ(ruby_edge_confidence("not-a-strategy"), 0.0);
}

// --- Dispatcher --------------------------------------------------------------

TEST(FidelityTest, Dispatch_PythonAndGoImportEdges)
{
    const auto py = reconstruct_edge_fidelity("pkg/a.py", "import", ".b", "pkg/b.py",
                                              /*is_external=*/false);
    ASSERT_TRUE(py.has_value());
    EXPECT_EQ(py->resolved_by, "relative-module");
    EXPECT_DOUBLE_EQ(py->confidence, k_py_resolved_confidence);

    const auto go = reconstruct_edge_fidelity("cmd/main.go", "import", "example.com/app/x",
                                              "x/x.go", /*is_external=*/false);
    ASSERT_TRUE(go.has_value());
    EXPECT_EQ(go->resolved_by, "go-internal");
    EXPECT_DOUBLE_EQ(go->confidence, k_go_internal_confidence);
}

TEST(FidelityTest, Dispatch_UncalibratedReturnsNullopt)
{
    // An uncalibrated language (SQL has no import semantics) yields nothing.
    EXPECT_FALSE(reconstruct_edge_fidelity("db/schema.sql", "import", "other", "other.sql", false)
                     .has_value());
    // Right extension, but a kind that language doesn't calibrate.
    EXPECT_FALSE(reconstruct_edge_fidelity("a.py", "call", "b", "b.py", false).has_value());
}

TEST(FidelityTest, Dispatch_SharedKindGatesOnExtension)
{
    // The edge kinds `include`, `require`, and `use` are each shared across
    // languages, so dispatch must key on the source extension, never the kind
    // alone. Guard every collision: each edge must route to its language.
    const auto strat = [](std::string_view src, std::string_view kind, std::string_view imp,
                          std::string_view target, bool is_external) {
        const auto fidelity = reconstruct_edge_fidelity(src, kind, imp, target, is_external);
        return fidelity ? fidelity->resolved_by : std::string{"<none>"};
    };

    // include: PHP vs C/C++.
    EXPECT_EQ(strat("a/b.php", "include", "x.php", "x.php", false), "php-require-resolved");
    EXPECT_EQ(strat("a/b.h", "include", "x.h", "x.h", false), "cinclude-resolved-bare");
    // require: JS/TS vs Ruby vs PHP.
    EXPECT_EQ(strat("a/b.ts", "require", "react", "", true), "jsts-bare-external");
    EXPECT_EQ(strat("a/b.rb", "require", "set", "", true), "ruby-external-stdlib");
    // use: C# vs PHP vs Rust.
    EXPECT_EQ(strat("a/b.cs", "use", "System.Text", "", true), "csharp-external-system");
    EXPECT_EQ(strat("a/b.php", "use", "Closure", "", true), "php-use-external-global");
    EXPECT_EQ(strat("a/b.rs", "use", "std::io", "", true), "rust-use-std");
}

// --- fidelity_metadata block -------------------------------------------------

TEST(FidelityTest, Metadata_HasExpectedShape)
{
    const nlohmann::json meta = build_fidelity_metadata_json();

    // Shared top-level caveat, then a per-language `languages` map; each
    // language carries its own `provisional` flag.
    ASSERT_FALSE(meta.contains("provisional")) << "provisional moved per-language";
    ASSERT_TRUE(meta.contains("caveat"));
    EXPECT_NE(meta["caveat"].get<std::string>().find("NOT a per-repo guarantee"),
              std::string::npos);
    ASSERT_TRUE(meta.contains("languages"));

    const auto& py = meta["languages"]["python"];
    ASSERT_TRUE(py.contains("version"));
    EXPECT_EQ(py["method"], "per-strategy empirical precision vs manual ground truth (offline)");
    EXPECT_EQ(py["scope"], "python-import-edges");
    EXPECT_EQ(py["provisional"], true);
    EXPECT_EQ(py["corpus"]["projects"], 2);
    EXPECT_EQ(py["corpus"]["labeled_edges"], 112);
    const auto& py_exp = py["expected_precision"];
    EXPECT_DOUBLE_EQ(py_exp["relative-module"].get<double>(), k_py_resolved_confidence);
    EXPECT_DOUBLE_EQ(py_exp["dotted-package"].get<double>(), k_py_resolved_confidence);
    EXPECT_DOUBLE_EQ(py_exp["external-dotted"].get<double>(), k_py_external_dotted_confidence);
    EXPECT_DOUBLE_EQ(py_exp["external-relative"].get<double>(), k_py_external_relative_confidence);

    const auto& go = meta["languages"]["go"];
    ASSERT_TRUE(go.contains("version"));
    EXPECT_EQ(go["scope"], "go-import-edges");
    EXPECT_EQ(go["provisional"], false) << "Go de-provisionalized after corpus expansion";
    EXPECT_EQ(go["corpus"]["projects"], 11);
    EXPECT_EQ(go["corpus"]["labeled_edges"], 90);
    const auto& go_exp = go["expected_precision"];
    EXPECT_DOUBLE_EQ(go_exp["go-internal"].get<double>(), k_go_internal_confidence);
    EXPECT_DOUBLE_EQ(go_exp["go-external-stdlib"].get<double>(), k_go_external_stdlib_confidence);
    EXPECT_DOUBLE_EQ(go_exp["go-external-thirdparty"].get<double>(),
                     k_go_external_thirdparty_confidence);

    const auto& rust = meta["languages"]["rust"];
    EXPECT_EQ(rust["scope"], "rust-use-mod-edges");
    EXPECT_EQ(rust["provisional"], false) << "Rust de-provisionalized after 11-crate recalibration";
    EXPECT_EQ(rust["corpus"]["projects"], 11);
    EXPECT_EQ(rust["corpus"]["labeled_edges"], 7342);
    EXPECT_DOUBLE_EQ(rust["expected_precision"]["rust-use-internal-resolved"].get<double>(),
                     k_rust_use_internal_resolved_confidence);
    EXPECT_DOUBLE_EQ(rust["expected_precision"]["rust-use-internal-unresolved"].get<double>(),
                     k_rust_use_internal_unresolved_confidence);

    const auto& cpp = meta["languages"]["c_cpp"];
    EXPECT_EQ(cpp["scope"], "c-cpp-include-edges");
    EXPECT_EQ(cpp["provisional"], true);
    EXPECT_DOUBLE_EQ(cpp["expected_precision"]["cinclude-external-bare"].get<double>(),
                     k_cinclude_external_bare_confidence);

    // JS and TS share one calibration block, registered under both names.
    EXPECT_EQ(meta["languages"]["javascript"], meta["languages"]["typescript"]);
    const auto& jsts = meta["languages"]["typescript"];
    EXPECT_EQ(jsts["scope"], "javascript-typescript-import-edges");
    EXPECT_EQ(jsts["provisional"], true);
    EXPECT_DOUBLE_EQ(jsts["expected_precision"]["jsts-alias-unresolved"].get<double>(),
                     k_jsts_alias_unresolved_confidence);

    const auto& java = meta["languages"]["java"];
    EXPECT_EQ(java["scope"], "java-import-edges");
    EXPECT_EQ(java["provisional"], true);
    EXPECT_DOUBLE_EQ(java["expected_precision"]["java-external-innertype"].get<double>(),
                     k_java_external_innertype_confidence);

    const auto& cs = meta["languages"]["csharp"];
    EXPECT_EQ(cs["scope"], "csharp-using-edges");
    EXPECT_EQ(cs["provisional"], true);
    EXPECT_DOUBLE_EQ(cs["expected_precision"]["csharp-external-thirdparty"].get<double>(),
                     k_cs_external_thirdparty_confidence);

    const auto& php = meta["languages"]["php"];
    EXPECT_EQ(php["scope"], "php-import-edges");
    EXPECT_EQ(php["provisional"], true);
    EXPECT_DOUBLE_EQ(php["expected_precision"]["php-use-nsindex-fanout"].get<double>(),
                     k_php_use_nsindex_fanout_confidence);

    const auto& ruby = meta["languages"]["ruby"];
    EXPECT_EQ(ruby["scope"], "ruby-require-edges");
    EXPECT_EQ(ruby["provisional"], true);
    EXPECT_DOUBLE_EQ(ruby["expected_precision"]["ruby-resolved-single"].get<double>(),
                     k_ruby_resolved_single_confidence);
}

// --- Digest integration ------------------------------------------------------

ExportOptions make_options(DigestFormat format)
{
    ExportOptions options;
    options.format = format;
    options.project_root = "/fake/project";
    options.project_name = "fidelity-test";
    return options;
}

/// Two Python files plus a C++ pair, so the digest carries both a
/// Python import edge (gets resolved_by/confidence) and a non-Python
/// edge (must stay untouched).
void populate_mixed_index(CodeIndex& index, std::int64_t& py_src, std::int64_t& py_dst)
{
    FileEntry a;
    a.path_relative = "pkg/a.py";
    a.language = Language::Python;
    py_src = index.add_file(std::move(a));

    FileEntry b;
    b.path_relative = "pkg/b.py";
    b.language = Language::Python;
    py_dst = index.add_file(std::move(b));

    FileEntry c;
    c.path_relative = "src/x.cpp";
    c.language = Language::Cpp;
    const std::int64_t c_id = index.add_file(std::move(c));

    FileEntry d;
    d.path_relative = "src/y.cpp";
    d.language = Language::Cpp;
    const std::int64_t d_id = index.add_file(std::move(d));

    // Resolved Python relative import: pkg/a.py  from .b  -> pkg/b.py
    Dependency py;
    py.source_file_id = py_src;
    py.target_file_id = py_dst;
    py.import_string = ".b";
    py.kind = "import";

    // C++ include — must NOT gain fidelity fields.
    Dependency cpp;
    cpp.source_file_id = c_id;
    cpp.target_file_id = d_id;
    cpp.import_string = "y.h";
    cpp.kind = "include";

    const std::array<Dependency, 2> batch = {py, cpp};
    index.add_dependencies(batch);
}

TEST(FidelityTest, FullJson_PythonEdgeCarriesResolvedByAndConfidence)
{
    CodeIndex index;
    std::int64_t py_src = 0;
    std::int64_t py_dst = 0;
    populate_mixed_index(index, py_src, py_dst);

    const auto parsed =
        nlohmann::json::parse(build_digest_string(index, make_options(DigestFormat::Json)));

    bool saw_python = false;
    bool saw_cpp = false;
    for (const auto& e : parsed["dependency_graph"]["edges"]) {
        if (e["kind"] == "import") {
            saw_python = true;
            ASSERT_TRUE(e.contains("resolved_by"));
            ASSERT_TRUE(e.contains("confidence"));
            // pkg/a.py `from .b` resolving to pkg/b.py (a module) is
            // relative-module at the resolved confidence.
            EXPECT_EQ(e["resolved_by"], "relative-module");
            EXPECT_DOUBLE_EQ(e["confidence"].get<double>(), k_py_resolved_confidence);
        }
        if (e["kind"] == "include") {
            saw_cpp = true;
            // C/C++ includes are calibrated too: src/x.cpp #include "y.h"
            // (a bare, resolved include) → cinclude-resolved-bare.
            EXPECT_EQ(e["resolved_by"], "cinclude-resolved-bare");
            EXPECT_DOUBLE_EQ(e["confidence"].get<double>(), k_cinclude_resolved_bare_confidence);
        }
    }
    EXPECT_TRUE(saw_python);
    EXPECT_TRUE(saw_cpp);
}

TEST(FidelityTest, FullJson_ExternalPythonEdgeIsExternalDotted)
{
    CodeIndex index;
    FileEntry a;
    a.path_relative = "app/main.py";
    a.language = Language::Python;
    const std::int64_t a_id = index.add_file(std::move(a));

    Dependency ext;
    ext.source_file_id = a_id;
    ext.target_file_id = 0; // unresolved external
    ext.import_string = "numpy";
    ext.kind = "import";
    index.add_dependency(std::move(ext));

    const auto parsed =
        nlohmann::json::parse(build_digest_string(index, make_options(DigestFormat::Json)));

    const auto& edges = parsed["dependency_graph"]["edges"];
    ASSERT_EQ(edges.size(), 1U);
    EXPECT_EQ(edges[0]["resolved_by"], "external-dotted");
    EXPECT_DOUBLE_EQ(edges[0]["confidence"].get<double>(), k_py_external_dotted_confidence);
}

TEST(FidelityTest, FullJson_GoEdgesCarryStrategyAndConfidence)
{
    CodeIndex index;

    FileEntry main_go;
    main_go.path_relative = "cmd/main.go";
    main_go.language = Language::Go;
    const std::int64_t main_id = index.add_file(std::move(main_go));

    FileEntry handler_go;
    handler_go.path_relative = "internal/handler/handler.go";
    handler_go.language = Language::Go;
    const std::int64_t handler_id = index.add_file(std::move(handler_go));

    // Resolved internal import (matched the go.mod prefix) -> go-internal.
    Dependency internal_dep;
    internal_dep.source_file_id = main_id;
    internal_dep.target_file_id = handler_id;
    internal_dep.import_string = "example.com/app/internal/handler";
    internal_dep.kind = "import";

    // Unresolved standard-library import -> go-external-stdlib.
    Dependency stdlib;
    stdlib.source_file_id = main_id;
    stdlib.target_file_id = 0;
    stdlib.import_string = "database/sql";
    stdlib.kind = "import";

    // Unresolved third-party import -> go-external-thirdparty.
    Dependency thirdparty;
    thirdparty.source_file_id = main_id;
    thirdparty.target_file_id = 0;
    thirdparty.import_string = "github.com/gin-gonic/gin";
    thirdparty.kind = "import";

    const std::array<Dependency, 3> batch = {internal_dep, stdlib, thirdparty};
    index.add_dependencies(batch);

    const auto parsed =
        nlohmann::json::parse(build_digest_string(index, make_options(DigestFormat::Json)));

    bool saw_internal = false;
    bool saw_stdlib = false;
    bool saw_thirdparty = false;
    for (const auto& e : parsed["dependency_graph"]["edges"]) {
        if (e["kind"] != "import") {
            continue;
        }
        ASSERT_TRUE(e.contains("resolved_by"));
        ASSERT_TRUE(e.contains("confidence"));
        if (e["resolved_by"] == "go-internal") {
            saw_internal = true;
            EXPECT_DOUBLE_EQ(e["confidence"].get<double>(), k_go_internal_confidence);
        }
        else if (e["resolved_by"] == "go-external-stdlib") {
            saw_stdlib = true;
            EXPECT_DOUBLE_EQ(e["confidence"].get<double>(), k_go_external_stdlib_confidence);
        }
        else if (e["resolved_by"] == "go-external-thirdparty") {
            saw_thirdparty = true;
            EXPECT_DOUBLE_EQ(e["confidence"].get<double>(), k_go_external_thirdparty_confidence);
        }
    }
    EXPECT_TRUE(saw_internal);
    EXPECT_TRUE(saw_stdlib);
    EXPECT_TRUE(saw_thirdparty);
}

TEST(FidelityTest, FullJson_RustEdgesCarryStrategyAndConfidence)
{
    CodeIndex index;

    FileEntry lib;
    lib.path_relative = "src/lib.rs";
    lib.language = Language::Rust;
    const std::int64_t lib_id = index.add_file(std::move(lib));

    FileEntry foo;
    foo.path_relative = "src/foo.rs";
    foo.language = Language::Rust;
    const std::int64_t foo_id = index.add_file(std::move(foo));

    // `mod foo;` resolved to a sibling file -> rust-mod.
    Dependency mod_dep;
    mod_dep.source_file_id = lib_id;
    mod_dep.target_file_id = foo_id;
    mod_dep.import_string = "foo";
    mod_dep.kind = "mod";

    // `use serde::Serialize;` — in-crate `use` now resolves, but a crate
    // dependency like serde stays external -> rust-use-extern.
    Dependency use_dep;
    use_dep.source_file_id = lib_id;
    use_dep.target_file_id = 0;
    use_dep.import_string = "serde::Serialize";
    use_dep.kind = "use";

    const std::array<Dependency, 2> batch = {mod_dep, use_dep};
    index.add_dependencies(batch);

    const auto parsed =
        nlohmann::json::parse(build_digest_string(index, make_options(DigestFormat::Json)));

    bool saw_mod = false;
    bool saw_use = false;
    for (const auto& e : parsed["dependency_graph"]["edges"]) {
        if (e["kind"] == "mod") {
            saw_mod = true;
            EXPECT_EQ(e["resolved_by"], "rust-mod");
            EXPECT_DOUBLE_EQ(e["confidence"].get<double>(), k_rust_mod_confidence);
        }
        else if (e["kind"] == "use") {
            saw_use = true;
            EXPECT_EQ(e["resolved_by"], "rust-use-extern");
            EXPECT_DOUBLE_EQ(e["confidence"].get<double>(), k_rust_use_extern_confidence);
        }
    }
    EXPECT_TRUE(saw_mod);
    EXPECT_TRUE(saw_use);
}

TEST(FidelityTest, FullJson_JstsEdgesCarryStrategyAndConfidence)
{
    CodeIndex index;

    FileEntry app;
    app.path_relative = "src/app.ts";
    app.language = Language::TypeScript;
    const std::int64_t app_id = index.add_file(std::move(app));

    FileEntry util;
    util.path_relative = "src/util.ts";
    util.language = Language::TypeScript;
    const std::int64_t util_id = index.add_file(std::move(util));

    // Resolved relative import -> jsts-relative-resolved.
    Dependency rel;
    rel.source_file_id = app_id;
    rel.target_file_id = util_id;
    rel.import_string = "./util";
    rel.kind = "import";

    // Unresolved tsconfig path alias -> jsts-alias-unresolved (detector).
    Dependency alias;
    alias.source_file_id = app_id;
    alias.target_file_id = 0;
    alias.import_string = "@/store";
    alias.kind = "import";

    // CJS require of an npm package -> jsts-bare-external (exercises require kind).
    Dependency bare;
    bare.source_file_id = app_id;
    bare.target_file_id = 0;
    bare.import_string = "react";
    bare.kind = "require";

    const std::array<Dependency, 3> batch = {rel, alias, bare};
    index.add_dependencies(batch);

    const auto parsed =
        nlohmann::json::parse(build_digest_string(index, make_options(DigestFormat::Json)));

    bool saw_rel = false;
    bool saw_alias = false;
    bool saw_bare = false;
    for (const auto& e : parsed["dependency_graph"]["edges"]) {
        const auto by = e.value("resolved_by", std::string{});
        if (by == "jsts-relative-resolved") {
            saw_rel = true;
            EXPECT_DOUBLE_EQ(e["confidence"].get<double>(), k_jsts_relative_resolved_confidence);
        }
        else if (by == "jsts-alias-unresolved") {
            saw_alias = true;
            EXPECT_DOUBLE_EQ(e["confidence"].get<double>(), k_jsts_alias_unresolved_confidence);
        }
        else if (by == "jsts-bare-external") {
            saw_bare = true;
            EXPECT_DOUBLE_EQ(e["confidence"].get<double>(), k_jsts_bare_external_confidence);
        }
    }
    EXPECT_TRUE(saw_rel);
    EXPECT_TRUE(saw_alias);
    EXPECT_TRUE(saw_bare);
}

TEST(FidelityTest, BothFormats_CarryFidelityMetadata)
{
    CodeIndex index;
    std::int64_t py_src = 0;
    std::int64_t py_dst = 0;
    populate_mixed_index(index, py_src, py_dst);

    for (const DigestFormat fmt : {DigestFormat::Json, DigestFormat::SlimJson}) {
        const auto parsed = nlohmann::json::parse(build_digest_string(index, make_options(fmt)));
        ASSERT_TRUE(parsed.contains("fidelity_metadata")) << "format=" << static_cast<int>(fmt);
        const auto& meta = parsed["fidelity_metadata"];
        EXPECT_TRUE(meta.contains("caveat"));
        ASSERT_TRUE(meta.contains("languages"));
        for (const char* lang : {"python", "go"}) {
            const auto& block = meta["languages"][lang];
            EXPECT_TRUE(block.contains("version")) << "lang=" << lang;
            EXPECT_TRUE(block.contains("scope")) << "lang=" << lang;
            EXPECT_TRUE(block.contains("expected_precision")) << "lang=" << lang;
            EXPECT_TRUE(block.contains("provisional")) << "lang=" << lang;
        }
        EXPECT_EQ(meta["languages"]["go"]["scope"], "go-import-edges");
    }
}

TEST(FidelityTest, SlimJson_EdgeTuplesStayFrozen)
{
    // Slim must get fidelity_metadata but NOT per-edge confidence — its
    // edge tuples stay at 4 positional elements (schema version 2).
    CodeIndex index;
    std::int64_t py_src = 0;
    std::int64_t py_dst = 0;
    populate_mixed_index(index, py_src, py_dst);

    const auto parsed =
        nlohmann::json::parse(build_digest_string(index, make_options(DigestFormat::SlimJson)));

    EXPECT_EQ(parsed["_schema"]["version"], 2);
    const auto& edges = parsed["dependency_graph"]["edges"];
    ASSERT_FALSE(edges.empty());
    for (const auto& e : edges) {
        ASSERT_TRUE(e.is_array()) << "slim edges stay positional tuples";
        EXPECT_EQ(e.size(), 4U) << "slim tuple arity must not grow";
    }
}

} // namespace
