#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <gtest/gtest.h>

#include "code/architecture_detector.h"
#include "code/code_index.h"
#include "code/dependency.h"
#include "code/exclude_dirs.h"
#include "code/language.h"
#include "code/manifest_scanner.h"
#include "code/parser.h"
#include "code/properties_reader.h"
#include "code/scanner.h"
#include "code/symbol.h"
#include "core/task_queue.h"
#include "platform/file_io.h"

// VECTIS_FIXTURE_DIR is injected as a compile-time definition from
// tests/CMakeLists.txt so the tests can find `tests/fixtures/code/`
// regardless of where the binary is invoked from.
#ifndef VECTIS_FIXTURE_DIR
#error "VECTIS_FIXTURE_DIR is not defined — tests/CMakeLists.txt must pass it"
#endif

namespace {

using vectis::code::CodeIndex;
using vectis::code::Language;
using vectis::code::ScanConfig;
using vectis::code::Scanner;
using vectis::code::ScanProgress;
using vectis::code::ScanSummary;
using vectis::code::Symbol;
using vectis::code::SymbolKind;
using vectis::code::TreeSitterParser;
using vectis::core::CancellationToken;

/// Helper: does the index contain a symbol by this name (any kind)?
bool index_has_symbol(const CodeIndex& index, std::string_view name)
{
    const auto matches = index.search_symbols(name);
    return std::any_of(matches.begin(), matches.end(),
                       [&](const Symbol& s) { return s.name == name; });
}

/// Outcome of the env-var gating shared by every `Optional*Corpus_*`
/// smoke test. Either `skip_reason` is populated (caller emits
/// `GTEST_SKIP()` with the message — gtest's skip macro must be invoked
/// from the test body, not the helper, because it only short-circuits
/// the enclosing function) or `corpus_root` holds the validated path.
struct CorpusEnvResult
{
    std::optional<std::string> skip_reason;
    std::filesystem::path corpus_root;
};

/// Validate the corpus-smoke env vars: `VECTIS_RUN_CORPUS_SMOKE=1` plus
/// the per-corpus `dir_env_name`. `dir_kind` is a noun phrase appended
/// to the skip hint (e.g. "a multi-module POM corpus") so the skip
/// reason explains what shape of corpus to point at.
[[nodiscard]] CorpusEnvResult resolve_corpus_dir(const char* dir_env_name,
                                                 std::string_view dir_kind)
{
    const char* enable = std::getenv("VECTIS_RUN_CORPUS_SMOKE");
    if (enable == nullptr || std::string_view{enable} != "1") {
        return {"set VECTIS_RUN_CORPUS_SMOKE=1 to run corpus smoke tests", {}};
    }
    const char* env_dir = std::getenv(dir_env_name);
    if (env_dir == nullptr || *env_dir == '\0') {
        return {std::string{"set "} + dir_env_name + " to point at " + std::string{dir_kind}, {}};
    }
    std::filesystem::path corpus_root{env_dir};
    if (!std::filesystem::is_directory(corpus_root)) {
        return {std::string{dir_env_name} + "='" + corpus_root.string() + "' is not a directory",
                {}};
    }
    // Canonicalise so the env-supplied string isn't passed verbatim into
    // later filesystem operations — same hygiene step the digest CLI
    // applies to `--output`. Breaks the CodeQL cpp/path-injection
    // taint flow that would otherwise track env_dir into every file
    // read the test does against the corpus tree.
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(corpus_root, ec);
    if (!ec) {
        corpus_root = std::move(canonical);
    }
    return {std::nullopt, std::move(corpus_root)};
}

/// Run a source-only scan of a fixture subdirectory.
void scan_fixture(std::string_view fixture_name, CodeIndex& index)
{
    const std::filesystem::path fixture_root =
        std::filesystem::path{VECTIS_FIXTURE_DIR} / "code" / std::string{fixture_name};

    TreeSitterParser parser;
    parser.register_builtin_languages();

    ScanConfig cfg;
    cfg.root = fixture_root;
    cfg.epoch = 1;

    std::atomic<std::int64_t> epoch{1};
    const CancellationToken token{};

    const auto result = Scanner::run(
        cfg, index, parser, [](const ScanProgress&) {}, [](const ScanSummary&) {}, token, epoch);
    EXPECT_TRUE(result.has_value()) << "scan of fixture '" << fixture_name << "' failed";
}

/// Run the full CLI digest path against a fixture: source scan, then
/// manifest pass with the default handler set, then edge resolution.
/// Mirrors what `run_uncached` does in `cli_main.cpp`, so any future
/// pipeline tweak the CLI picks up flows into these fixture
/// assertions automatically.
void scan_fixture_full_digest(std::string_view fixture_name, CodeIndex& index)
{
    const std::filesystem::path fixture_root =
        std::filesystem::path{VECTIS_FIXTURE_DIR} / "code" / std::string{fixture_name};

    TreeSitterParser parser;
    parser.register_builtin_languages();

    ScanConfig cfg;
    cfg.root = fixture_root;
    cfg.epoch = 1;

    std::atomic<std::int64_t> epoch{1};
    const CancellationToken token{};

    auto collect =
        Scanner::run_collect(cfg, index, parser, [](const ScanProgress&) {}, token, epoch);
    ASSERT_TRUE(collect.has_value()) << "collect of '" << fixture_name << "' failed";

    vectis::code::manifest_scanner::Config mc;
    mc.root = fixture_root;
    mc.epoch = 1;
    vectis::code::manifest_scanner::scan_manifests(
        mc, index, collect->visited_paths, vectis::code::manifest_scanner::default_handlers());

    Scanner::resolve(index, fixture_root, collect->per_file_imports);
}

TEST(FixturesTest, SamplePython_ScansAndExtractsSymbols)
{
    CodeIndex index;
    scan_fixture("sample-python", index);

    // 3 files: main.py, models/user.py, utils/helpers.py
    EXPECT_EQ(index.file_count(), 3U);
    EXPECT_EQ(index.language_count(), 1U);

    // A handful of symbols we know should be there.
    EXPECT_TRUE(index_has_symbol(index, "run"));
    EXPECT_TRUE(index_has_symbol(index, "main"));
    EXPECT_TRUE(index_has_symbol(index, "User"));
    EXPECT_TRUE(index_has_symbol(index, "display_name"));
    EXPECT_TRUE(index_has_symbol(index, "format_greeting"));
    EXPECT_TRUE(index_has_symbol(index, "shout"));
}

TEST(FixturesTest, SampleTypeScript_ExtractsClassesAndInterfaces)
{
    CodeIndex index;
    scan_fixture("sample-typescript", index);

    // 3 .ts files under src/
    EXPECT_GE(index.file_count(), 3U);
    EXPECT_TRUE(index_has_symbol(index, "UserService"));
    EXPECT_TRUE(index_has_symbol(index, "findById"));
    EXPECT_TRUE(index_has_symbol(index, "User"));
    EXPECT_TRUE(index_has_symbol(index, "Repository"));
}

TEST(FixturesTest, SampleCpp_ExtractsWidgetClass)
{
    CodeIndex index;
    scan_fixture("sample-cpp", index);

    EXPECT_GE(index.file_count(), 3U);
    EXPECT_TRUE(index_has_symbol(index, "Widget"));
    EXPECT_TRUE(index_has_symbol(index, "Rect"));
    EXPECT_TRUE(index_has_symbol(index, "demo"));
}

TEST(FixturesTest, SampleRust_ExtractsTypesAndTraits)
{
    CodeIndex index;
    scan_fixture("sample-rust", index);

    EXPECT_GE(index.file_count(), 2U);
    EXPECT_TRUE(index_has_symbol(index, "Config"));
    EXPECT_TRUE(index_has_symbol(index, "Handler"));
    EXPECT_TRUE(index_has_symbol(index, "default_config"));
}

TEST(FixturesTest, MixedProject_RecognizesMultipleLanguages)
{
    CodeIndex index;
    scan_fixture("mixed", index);

    EXPECT_GE(index.file_count(), 4U);
    // Python server + TypeScript app + Java worker + SQL migration
    EXPECT_GE(index.language_count(), 3U);
}

TEST(FixturesTest, SampleCpp_ScannerPopulatesDependencies)
{
    CodeIndex index;
    scan_fixture("sample-cpp", index);

    // widget.cpp includes widget.hpp, main.cpp includes widget.hpp.
    // The scanner should register both as dependency edges.
    EXPECT_GE(index.dependency_count(), 2U) << "expected at least 2 #include edges in sample-cpp";

    // Find the widget.hpp file id and assert someone depends on it.
    std::int64_t widget_hpp_id = 0;
    for (const auto& f : index.snapshot_files()) {
        if (f.path_relative.filename().string() == "widget.hpp") {
            widget_hpp_id = f.id;
            break;
        }
    }
    ASSERT_NE(widget_hpp_id, 0);
    const auto dependents = index.dependents_of(widget_hpp_id);
    EXPECT_GE(dependents.size(), 2U)
        << "widget.hpp should be included by both main.cpp and widget.cpp";
}

TEST(FixturesTest, SamplePython_ScannerResolvesImports)
{
    CodeIndex index;
    scan_fixture("sample-python", index);

    // main.py imports models.user and utils.helpers; utils/helpers.py
    // imports models.user. All three should resolve internally.
    std::int64_t user_py_id = 0;
    for (const auto& f : index.snapshot_files()) {
        if (f.path_relative.generic_string() == "models/user.py") {
            user_py_id = f.id;
            break;
        }
    }
    ASSERT_NE(user_py_id, 0);
    const auto user_dependents = index.dependents_of(user_py_id);
    // Both main.py and helpers.py should depend on models/user.py.
    EXPECT_GE(user_dependents.size(), 2U);
}

/// Optional smoke test against a real multi-module Maven corpus.
/// Activated when both `VECTIS_RUN_CORPUS_SMOKE=1` and
/// `VECTIS_JAVA_CORPUS_DIR=<path>` are set; otherwise skipped so the
/// suite stays sub-second on default runs. The corpus this was
/// calibrated against is documented in `docs/plans/` (gitignored).
TEST(FixturesTest, OptionalJavaCorpus_SmokesPomCountAndKinds)
{
    auto env = resolve_corpus_dir("VECTIS_JAVA_CORPUS_DIR", "a multi-module POM corpus");
    if (env.skip_reason) {
        GTEST_SKIP() << *env.skip_reason;
    }
    const std::filesystem::path& corpus_root = env.corpus_root;

    // Manifest-only run — bypasses the source scanner entirely so the
    // smoke test stays sub-minute on a 36k-file corpus. The assertions
    // below only inspect POM-derived state, so the source pass would
    // just be expensive noise.
    CodeIndex index;
    std::unordered_set<std::string> visited;
    vectis::code::manifest_scanner::Config mc;
    mc.root = corpus_root;
    mc.epoch = 1;
    vectis::code::manifest_scanner::scan_manifests(
        mc, index, visited, vectis::code::manifest_scanner::default_handlers());

    std::size_t pom_files = 0;
    for (const auto& f : index.snapshot_files()) {
        if (f.language == Language::MavenPom) {
            ++pom_files;
        }
    }
    EXPECT_GT(pom_files, 500U) << "expected a substantial multi-module POM tree";

    std::size_t parent_internal = 0;
    std::size_t parent_external = 0;
    std::size_t maven_count = 0;
    std::size_t managed_count = 0;
    std::size_t bom_count = 0;
    std::size_t module_count = 0;
    for (const auto& d : index.all_dependencies()) {
        if (d.kind == "maven-parent") {
            (d.target_file_id != 0 ? parent_internal : parent_external)++;
        }
        else if (d.kind == "maven") {
            ++maven_count;
        }
        else if (d.kind == "maven-managed") {
            ++managed_count;
        }
        else if (d.kind == "maven-bom") {
            ++bom_count;
        }
        else if (d.kind == "maven-module") {
            ++module_count;
        }
    }
    // Every POM except the root traces internally; the root POM's
    // parent lives off-repo, so exactly one external parent expected.
    EXPECT_EQ(parent_external, 1U) << "root POM's parent is the only off-repo parent reference";
    EXPECT_GT(parent_internal, 0U);
    EXPECT_GT(managed_count, 0U) << "corpus uses <dependencyManagement> heavily";
    EXPECT_GT(maven_count, 0U);
    EXPECT_GT(module_count, 0U);
    EXPECT_GT(bom_count, 0U) << "corpus imports BOM-style dependencies";
    EXPECT_NE(maven_count, managed_count)
        << "managed vs live dep counts must diverge — the kinds are distinct";
}

/// Optional smoke test against a real .NET solution corpus with
/// Central Package Management active. Activated when both
/// `VECTIS_RUN_CORPUS_SMOKE=1` and `VECTIS_DOTNET_CORPUS_DIR=<path>`
/// are set. The corpus this was calibrated against is documented in
/// `docs/plans/` (gitignored).
TEST(FixturesTest, OptionalDotnetCorpus_SmokesCsprojCountAndCpmResolution)
{
    auto env = resolve_corpus_dir("VECTIS_DOTNET_CORPUS_DIR", "a .NET solution corpus");
    if (env.skip_reason) {
        GTEST_SKIP() << *env.skip_reason;
    }
    const std::filesystem::path& corpus_root = env.corpus_root;

    CodeIndex index;
    std::unordered_set<std::string> visited;
    vectis::code::manifest_scanner::Config mc;
    mc.root = corpus_root;
    mc.epoch = 1;
    vectis::code::manifest_scanner::scan_manifests(
        mc, index, visited, vectis::code::manifest_scanner::default_handlers());

    std::size_t csproj_files = 0;
    std::size_t sln_files = 0;
    std::size_t slnx_files = 0;
    for (const auto& f : index.snapshot_files()) {
        if (f.language == Language::Csproj) {
            ++csproj_files;
        }
        else if (f.language == Language::DotNetSolution) {
            const auto ext = f.path_relative.extension().string();
            if (ext == ".sln") {
                ++sln_files;
            }
            else if (ext == ".slnx") {
                ++slnx_files;
            }
        }
    }
    EXPECT_GT(csproj_files, 50U) << "expected a substantial .NET project graph";
    EXPECT_GT(sln_files + slnx_files, 0U);

    std::size_t pkg_empty_version = 0;
    std::size_t pkg_resolved = 0;
    std::size_t pkg_remove_leaked = 0;
    for (const auto& d : index.all_dependencies()) {
        if (d.kind != "csproj-package") {
            continue;
        }
        // Format is "name:version"; empty version → trailing colon.
        if (!d.import_string.empty() && d.import_string.back() == ':') {
            ++pkg_empty_version;
        }
        else {
            ++pkg_resolved;
        }
        // Directory.Build.targets uses <PackageReference Remove=> to
        // drop refs; none of those should have leaked into the
        // csproj-package set.
        if (d.import_string.find("Remove") != std::string::npos) {
            ++pkg_remove_leaked;
        }
    }
    EXPECT_GT(pkg_resolved, 0U);
    EXPECT_EQ(pkg_empty_version, 0U)
        << "every PackageReference must resolve to a version via Directory.Packages.props";
    EXPECT_EQ(pkg_remove_leaked, 0U) << "Remove= and Update= entries must not produce edges";

    std::size_t proj_internal = 0;
    std::size_t imp_internal = 0;
    std::size_t sln_internal = 0;
    for (const auto& d : index.all_dependencies()) {
        if (d.kind == "csproj-project" && d.target_file_id != 0) {
            ++proj_internal;
        }
        else if (d.kind == "csproj-import" && d.target_file_id != 0) {
            ++imp_internal;
        }
        else if (d.kind == "sln-project" && d.target_file_id != 0) {
            ++sln_internal;
        }
    }
    EXPECT_GT(proj_internal, 300U) << "the bulk of <ProjectReference> must resolve to siblings";
    EXPECT_GT(imp_internal, 100U) << "shared .props imports must resolve via $(RepoRoot)";
    EXPECT_GT(sln_internal, 100U)
        << "Solution.sln + .slnx must produce many internal sln-project edges";
}

TEST(FixturesTest, SampleMavenMultimodule_RegistersThreePomsAndEmitsMavenEdges)
{
    CodeIndex index;
    scan_fixture_full_digest("sample-maven-multimodule", index);

    // 3 POMs + 2 Java sources.
    EXPECT_EQ(index.file_count(), 5U);

    const auto root_id = index.file_id_for_path("pom.xml");
    const auto app_id = index.file_id_for_path("app/pom.xml");
    const auto lib_id = index.file_id_for_path("lib/pom.xml");
    ASSERT_NE(root_id, 0);
    ASSERT_NE(app_id, 0);
    ASSERT_NE(lib_id, 0);

    // Collect edge categories.
    const auto all_deps = index.all_dependencies();
    std::size_t module_internal = 0;
    std::size_t parent_internal = 0;
    std::size_t maven_internal = 0;
    std::size_t maven_external_junit = 0;
    for (const auto& d : all_deps) {
        if (d.kind == "maven-module" && d.target_file_id != 0) {
            ++module_internal;
        }
        else if (d.kind == "maven-parent" && d.target_file_id != 0) {
            ++parent_internal;
        }
        else if (d.kind == "maven" && d.target_file_id != 0) {
            ++maven_internal;
        }
        else if (d.kind == "maven" && d.target_file_id == 0 &&
                 d.import_string == "org.junit.jupiter:junit-jupiter:5.10.0") {
            // Confirms the ${junit-version} placeholder resolved via
            // the parent POM's <properties> — one-hop substitution.
            ++maven_external_junit;
        }
    }
    EXPECT_EQ(module_internal, 2U) << "root → app and root → lib";
    EXPECT_EQ(parent_internal, 2U)
        << "missing <relativePath> must default to ../pom.xml and resolve internally";
    EXPECT_EQ(maven_internal, 1U) << "app → lib via in-repo coordinate";
    EXPECT_EQ(maven_external_junit, 1U)
        << "external junit coord with property resolved via parent <properties>";
}

TEST(FixturesTest, SampleDotnetCpm_RegistersFiveManifestsAndResolvesViaCPM)
{
    CodeIndex index;
    scan_fixture_full_digest("sample-dotnet-cpm", index);

    // 5 manifests + 2 C# sources.
    EXPECT_EQ(index.file_count(), 7U);

    const auto app_id = index.file_id_for_path("App/App.csproj");
    const auto lib_id = index.file_id_for_path("Lib/Lib.csproj");
    const auto sln_id = index.file_id_for_path("Solution.sln");
    const auto cpm_id = index.file_id_for_path("Directory.Packages.props");
    const auto common_id = index.file_id_for_path("Common.props");
    ASSERT_NE(app_id, 0);
    ASSERT_NE(lib_id, 0);
    ASSERT_NE(sln_id, 0);
    ASSERT_NE(cpm_id, 0);
    ASSERT_NE(common_id, 0);

    const auto all_deps = index.all_dependencies();
    bool project_ref = false;
    bool package_resolved_via_cpm = false;
    bool import_internal = false;
    std::size_t sln_internal = 0;
    for (const auto& d : all_deps) {
        if (d.kind == "csproj-project" && d.source_file_id == app_id &&
            d.target_file_id == lib_id) {
            project_ref = true;
        }
        else if (d.kind == "csproj-package" && d.source_file_id == app_id &&
                 d.import_string == "Newtonsoft.Json:13.0.3") {
            package_resolved_via_cpm = true;
        }
        else if (d.kind == "csproj-import" && d.source_file_id == app_id &&
                 d.target_file_id == common_id) {
            import_internal = true;
        }
        else if (d.kind == "sln-project" && d.source_file_id == sln_id && d.target_file_id != 0) {
            ++sln_internal;
        }
    }
    EXPECT_TRUE(project_ref) << "App → Lib via <ProjectReference>";
    EXPECT_TRUE(package_resolved_via_cpm)
        << "PackageReference with no version must resolve to 13.0.3 via Directory.Packages.props";
    EXPECT_TRUE(import_internal) << "$(RepoRoot)Common.props must resolve internally";
    EXPECT_EQ(sln_internal, 2U) << "Solution.sln → App.csproj and Lib.csproj";
}

TEST(FixturesTest, SampleDotnetWpf_EmitsSdkFlagEdgesForUseWpfAndSdk)
{
    // SDK-only WPF apps carry no PackageReference, so the handler emits
    // `csproj-sdk-flag` markers from <UseWPF> and the root Sdk attribute
    // to keep the desktop-UI hint detectable end-to-end.
    CodeIndex index;
    scan_fixture_full_digest("sample-dotnet-wpf", index);

    const auto csproj_id = index.file_id_for_path("WpfApp.csproj");
    ASSERT_NE(csproj_id, 0);

    bool sdk_marker = false;
    bool wpf_marker = false;
    for (const auto& d : index.all_dependencies()) {
        if (d.kind != "csproj-sdk-flag" || d.source_file_id != csproj_id) {
            continue;
        }
        if (d.import_string == "Microsoft.NET.Sdk.WindowsDesktop") {
            sdk_marker = true;
        }
        else if (d.import_string == "Microsoft.NET.Sdk.WindowsDesktop.WPF") {
            wpf_marker = true;
        }
    }
    EXPECT_TRUE(sdk_marker) << "Sdk=Microsoft.NET.Sdk.WindowsDesktop must emit a sdk-flag edge";
    EXPECT_TRUE(wpf_marker) << "<UseWPF>true</UseWPF> must emit a sdk-flag edge";
}

TEST(FixturesTest, SampleDotnetFlagsFalse_EmitsNoSdkFlagEdgesWhenAllUseFlagsFalse)
{
    // Regression test for the property_is_true negative path: an
    // explicit `<UseWPF>false</UseWPF>` (and siblings) must NOT emit
    // a csproj-sdk-flag edge. A regression in property_is_true that
    // treated "false" as truthy would otherwise misclassify every
    // class library that explicitly opts out of WPF/WinForms/WinUI.
    CodeIndex index;
    scan_fixture_full_digest("sample-dotnet-flags-false", index);

    const auto csproj_id = index.file_id_for_path("PlainLib.csproj");
    ASSERT_NE(csproj_id, 0);

    bool any_sdk_flag = false;
    for (const auto& d : index.all_dependencies()) {
        if (d.kind == "csproj-sdk-flag" && d.source_file_id == csproj_id) {
            any_sdk_flag = true;
            break;
        }
    }
    EXPECT_FALSE(any_sdk_flag)
        << "explicit <UseWPF>false</UseWPF> et al must NOT emit any sdk-flag edge";
}

TEST(FixturesTest, SampleDotnetWinuiSlnx_FiresDesktopUiHintViaSlnxRoot)
{
    // Mirrors the canonical modern-WinUI-3 layout: an .slnx root manifest
    // alongside csprojs that opt in via <UseWinUI>true</UseWinUI>. Pins
    // three regressions at once: (1) the .slnx extension is recognised
    // as a .NET solution by detect_root_manifest, (2) the WinUI flag is
    // emitted as a `csproj-sdk-flag` edge, and (3) the framework hint
    // matcher fires DesktopUI on that marker so detect_architecture
    // attaches `hint:desktop-ui` to the signals.
    CodeIndex index;
    scan_fixture_full_digest("sample-dotnet-winui-slnx", index);

    const auto csproj_id = index.file_id_for_path("src/App/App.csproj");
    ASSERT_NE(csproj_id, 0);
    bool winui_marker = false;
    for (const auto& d : index.all_dependencies()) {
        if (d.kind == "csproj-sdk-flag" && d.source_file_id == csproj_id &&
            d.import_string == "Microsoft.NET.Sdk.WinUI") {
            winui_marker = true;
            break;
        }
    }
    EXPECT_TRUE(winui_marker) << "<UseWinUI>true</UseWinUI> must emit a sdk-flag edge";

    const auto arch = vectis::code::detect_architecture(
        index, std::filesystem::path{VECTIS_FIXTURE_DIR} / "code" / "sample-dotnet-winui-slnx", {});
    const bool has_desktop_ui = std::ranges::any_of(
        arch.signals, [](const std::string& s) { return s == "hint:desktop-ui"; });
    EXPECT_TRUE(has_desktop_ui)
        << "hint:desktop-ui must fire on .slnx-rooted projects that mark <UseWinUI>";
}

TEST(FixturesTest, SampleSpringXml_RegistersXmlFilesAndEmitsSpringEdges)
{
    CodeIndex index;
    scan_fixture_full_digest("sample-spring-xml", index);

    // 2 Spring XML files + 1 Java source.
    EXPECT_EQ(index.file_count(), 3U);

    const auto ctx_id = index.file_id_for_path("applicationContext.xml");
    const auto inner_id = index.file_id_for_path("inner-ctx.xml");
    const auto svc_id = index.file_id_for_path("src/main/java/com/example/svc/MyService.java");
    ASSERT_NE(ctx_id, 0);
    ASSERT_NE(inner_id, 0) << "DTD-style inner-ctx.xml must be detected as Spring XML";
    ASSERT_NE(svc_id, 0);

    std::size_t total_ctx_bean_edges = 0;
    std::size_t bean_internal = 0;
    std::size_t bean_external = 0;
    std::size_t import_internal = 0;
    std::size_t component_scan = 0;
    for (const auto& d : index.all_dependencies()) {
        if (d.kind == "spring-bean" && d.source_file_id == ctx_id) {
            ++total_ctx_bean_edges;
            if (d.target_file_id == svc_id && d.import_string == "com.example.svc.MyService") {
                ++bean_internal;
            }
            else if (d.target_file_id == 0) {
                ++bean_external;
            }
        }
        else if (d.kind == "spring-import" && d.source_file_id == ctx_id &&
                 d.target_file_id == inner_id) {
            ++import_internal;
        }
        else if (d.kind == "spring-component-scan" && d.source_file_id == ctx_id) {
            ++component_scan;
        }
    }
    // Partition guard: bean_internal + bean_external must account for every
    // spring-bean edge, so a bean mis-resolved to a wrong internal file
    // can't slip through counted in neither bucket.
    EXPECT_EQ(total_ctx_bean_edges, 3U)
        << "all three <bean class=...> entries must surface as edges, resolved or not";
    EXPECT_EQ(bean_internal, 1U) << "com.example.svc.MyService resolves to the scanned .java";
    EXPECT_EQ(bean_external, 2U) << "the two org.example.external.* beans stay external";
    EXPECT_EQ(import_internal, 1U) << "classpath:inner-ctx.xml resolves to the sibling XML";
    EXPECT_EQ(component_scan, 2U) << "comma-separated base-package splits to two edges";
}

/// Optional smoke test against a real Spring-XML-heavy corpus.
/// Activated when both `VECTIS_RUN_CORPUS_SMOKE=1` and
/// `VECTIS_SPRING_CORPUS_DIR=<path>` are set; otherwise skipped so the
/// suite stays sub-second on default runs. The corpus this was
/// calibrated against is documented in `docs/plans/` (gitignored).
TEST(FixturesTest, OptionalSpringCorpus_SmokesBeansFileDetection)
{
    auto env = resolve_corpus_dir("VECTIS_SPRING_CORPUS_DIR", "a Spring-XML corpus");
    if (env.skip_reason) {
        GTEST_SKIP() << *env.skip_reason;
    }
    const std::filesystem::path& corpus_root = env.corpus_root;

    // Manifest-only run — bypasses the source scanner so the smoke
    // test stays fast on a large corpus.
    CodeIndex index;
    std::unordered_set<std::string> visited;
    vectis::code::manifest_scanner::Config mc;
    mc.root = corpus_root;
    mc.epoch = 1;
    vectis::code::manifest_scanner::scan_manifests(
        mc, index, visited, vectis::code::manifest_scanner::default_handlers());

    std::size_t spring_xml_files = 0;
    for (const auto& f : index.snapshot_files()) {
        if (f.language == Language::SpringXml) {
            ++spring_xml_files;
        }
    }
    EXPECT_GT(spring_xml_files, 100U) << "expected a substantial Spring XML corpus";

    std::size_t bean_edges = 0;
    std::size_t bean_internal = 0;
    std::size_t import_edges = 0;
    std::size_t scan_edges = 0;
    for (const auto& d : index.all_dependencies()) {
        if (d.kind == "spring-bean") {
            ++bean_edges;
            if (d.target_file_id != 0) {
                ++bean_internal;
            }
        }
        else if (d.kind == "spring-import") {
            ++import_edges;
        }
        else if (d.kind == "spring-component-scan") {
            ++scan_edges;
        }
    }
    EXPECT_GT(bean_edges, 0U) << "corpus declares <bean class=...> wiring";
    EXPECT_GT(import_edges + scan_edges, 0U)
        << "corpus uses <import resource> and/or <context:component-scan>";
    // Manifest-only run: no .java files indexed, so bean_internal is
    // expected to be 0 here — internal resolution is covered by the
    // SampleSpringXml fixture test, which runs the source scanner too.
    EXPECT_EQ(bean_internal, 0U)
        << "manifest-only run indexes no .java targets; internal counts come from the fixture test";
}

TEST(FixturesTest, SampleProperties_RegistersFilesAndEmitsIncludeEdges)
{
    CodeIndex index;
    scan_fixture_full_digest("sample-properties", index);

    // 6 .properties files (no other file types in the fixture).
    EXPECT_EQ(index.file_count(), 6U);

    const auto app_id = index.file_id_for_path("application.properties");
    const auto sec_id = index.file_id_for_path("secrets.properties");
    const auto audit_id = index.file_id_for_path("audit.properties");
    const auto audit_extra_id = index.file_id_for_path("audit-extra.properties");
    const auto msg_id = index.file_id_for_path("Messages_en.properties");
    const auto weird_id = index.file_id_for_path("weird.properties");
    ASSERT_NE(app_id, 0);
    ASSERT_NE(sec_id, 0);
    ASSERT_NE(audit_id, 0);
    ASSERT_NE(audit_extra_id, 0);
    ASSERT_NE(msg_id, 0) << "i18n bundles must still appear in files[]";
    ASSERT_NE(weird_id, 0);

    std::size_t total_include_edges = 0;
    std::size_t app_to_secrets = 0;
    std::size_t log_to_extra = 0;
    std::size_t edges_from_weird = 0;
    std::size_t edges_from_messages = 0;
    for (const auto& d : index.all_dependencies()) {
        if (d.kind != "properties-include") {
            continue;
        }
        ++total_include_edges;
        if (d.source_file_id == app_id && d.target_file_id == sec_id) {
            ++app_to_secrets;
        }
        else if (d.source_file_id == audit_id && d.target_file_id == audit_extra_id) {
            ++log_to_extra;
        }
        else if (d.source_file_id == weird_id) {
            ++edges_from_weird;
        }
        else if (d.source_file_id == msg_id) {
            ++edges_from_messages;
        }
    }
    EXPECT_EQ(total_include_edges, 2U)
        << "expected exactly two include edges: application->secrets and audit->audit-extra";
    EXPECT_EQ(app_to_secrets, 1U);
    EXPECT_EQ(log_to_extra, 1U);
    EXPECT_EQ(edges_from_weird, 0U)
        << "filterParameters.include / dataset.container.version.include must NOT emit edges";
    EXPECT_EQ(edges_from_messages, 0U) << "i18n bundle must emit no edges";
}

/// Optional smoke test against a real `.properties`-heavy corpus.
/// Activated when both `VECTIS_RUN_CORPUS_SMOKE=1` and
/// `VECTIS_PROPERTIES_CORPUS_DIR=<path>` are set; otherwise skipped so
/// the suite stays sub-second on default runs. Calibrated against
/// a properties-heavy corpus documented in `docs/plans/` (gitignored).
TEST(FixturesTest, OptionalPropertiesCorpus_SmokesPropertiesFileDetection)
{
    auto env = resolve_corpus_dir("VECTIS_PROPERTIES_CORPUS_DIR", "a .properties-heavy corpus");
    if (env.skip_reason) {
        GTEST_SKIP() << *env.skip_reason;
    }
    const std::filesystem::path& corpus_root = env.corpus_root;

    // Default-exclude basenames used by BOTH the manifest scan and
    // the independent filesystem walk below. Sharing the same set
    // keeps the two counts apples-to-apples — a corpus with
    // `.properties` under `target` / `build` / `node_modules` is
    // skipped consistently in both, so the equality assertion holds.
    const auto& default_excludes = vectis::code::default_scanner_exclude_dir_names();

    // Manifest-only run — bypasses the source scanner for speed.
    CodeIndex index;
    std::unordered_set<std::string> visited;
    vectis::code::manifest_scanner::Config mc;
    mc.root = corpus_root;
    mc.epoch = 1;
    mc.exclude_dir_names = default_excludes;
    vectis::code::manifest_scanner::scan_manifests(
        mc, index, visited, vectis::code::manifest_scanner::default_handlers());

    // Independent recursive `find`-equivalent count of .properties
    // files in the corpus, respecting the same default-exclude
    // basenames the manifest scanner uses (apples-to-apples). Spec
    // assertion: the index's Properties count must MATCH this count
    // exactly — every physical file should be registered.
    std::size_t physical_count = 0;
    {
        std::error_code walk_ec;
        using DirIter = std::filesystem::recursive_directory_iterator;
        const auto walk_options = std::filesystem::directory_options::skip_permission_denied;
        DirIter walk_it{corpus_root, walk_options, walk_ec};
        const DirIter walk_end{};
        while (walk_it != walk_end) {
            const auto& walk_entry = *walk_it;
            if (walk_entry.is_directory(walk_ec) && !walk_ec) {
                if (default_excludes.contains(walk_entry.path().filename().string())) {
                    walk_it.disable_recursion_pending();
                }
            }
            else if (walk_entry.is_regular_file(walk_ec) && !walk_ec &&
                     walk_entry.path().extension() == ".properties") {
                ++physical_count;
            }
            walk_ec.clear();
            walk_it.increment(walk_ec);
            if (walk_ec) {
                // Fail loudly rather than silently undercounting — a
                // partial walk would make `properties_files ==
                // physical_count` pass on low-vs-low and hide a real
                // coverage drop.
                ADD_FAILURE() << "directory iteration aborted mid-walk: " << walk_ec.message();
                walk_ec.clear();
                break;
            }
        }
    }

    std::size_t properties_files = 0;
    for (const auto& f : index.snapshot_files()) {
        if (f.language == Language::Properties) {
            ++properties_files;
        }
    }
    EXPECT_EQ(properties_files, physical_count)
        << "every .properties file under the corpus (respecting default excludes) "
           "must be registered with Language::Properties — index count must match "
           "the independent filesystem walk count";

    // Per-edge source-key validation: every properties-include edge
    // must originate from an exact `spring.config.import` OR `include`
    // key in the source file, with the edge's import_string equal to
    // that key's value. A regression to substring matching (e.g.
    // emitting an edge from `filterParameters.include=foo`) would
    // fail this assertion immediately because the source file does
    // not contain an exact-key entry with value `foo`.
    const auto files_snapshot = index.snapshot_files();
    std::size_t include_edges = 0;
    std::size_t spurious_edges = 0;
    for (const auto& d : index.all_dependencies()) {
        if (d.kind != "properties-include") {
            continue;
        }
        ++include_edges;
        const vectis::code::FileEntry* source = nullptr;
        for (const auto& f : files_snapshot) {
            if (f.id == d.source_file_id) {
                source = &f;
                break;
            }
        }
        if (source == nullptr) {
            ++spurious_edges;
            continue;
        }
        auto content_result = vectis::platform::read_file(corpus_root / source->path_relative);
        if (!content_result) {
            ++spurious_edges;
            continue;
        }
        const auto entries = vectis::code::properties::parse_properties(*content_result);
        const bool legit = std::ranges::any_of(entries, [&](const auto& kv) {
            return (kv.key == "spring.config.import" || kv.key == "include") &&
                   kv.value == d.import_string;
        });
        if (!legit) {
            ++spurious_edges;
        }
    }
    EXPECT_EQ(spurious_edges, 0U) << "every properties-include edge must come from an exact "
                                     "spring.config.import / include key with matching value "
                                     "(substring matches and value mismatches are forbidden)";
    EXPECT_LT(include_edges, 50U)
        << "exact-key matching must keep include edges sparse on the calibration corpus "
           "(<50 per spec); an explosion suggests substring matching has crept back in";
}

} // namespace
