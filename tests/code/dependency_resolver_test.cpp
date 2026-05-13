#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "code/code_index.h"
#include "code/dependency.h"
#include "code/dependency_resolver.h"
#include "code/language.h"
#include "code/parser.h"
#include "code/symbol.h"

namespace {

using vectis::code::CodeIndex;
using vectis::code::Dependency;
using vectis::code::FileEntry;
using vectis::code::FileImports;
using vectis::code::Language;
using vectis::code::RawImport;
using vectis::code::resolve_all;

/// Helper: add a file to the index with the given relative path and
/// return its assigned id.
std::int64_t add(CodeIndex& idx, const std::string& relative, Language lang)
{
    FileEntry f;
    f.path_relative = relative;
    f.language = lang;
    return idx.add_file(std::move(f));
}

/// Helper: build a FileImports record for the given source file.
FileImports make_fi(std::int64_t file_id, Language lang, const std::string& relative_path,
                    std::vector<RawImport> imports, std::vector<std::string> namespaces = {})
{
    FileImports fi;
    fi.file_id = file_id;
    fi.language = lang;
    fi.relative_path = relative_path;
    fi.imports = std::move(imports);
    fi.declared_namespaces = std::move(namespaces);
    return fi;
}

TEST(DependencyResolverTest, Cpp_ResolvesQuotedIncludeByEndsWith)
{
    CodeIndex idx;
    const auto app_cpp = add(idx, "core/app.cpp", Language::Cpp);
    const auto log_h = add(idx, "core/log.h", Language::Cpp);

    std::vector<FileImports> per_file;
    per_file.push_back(
        make_fi(app_cpp, Language::Cpp, "core/app.cpp", {RawImport{"core/log.h", "include", 5}}));

    resolve_all(idx, "/fake/project", per_file);

    const auto deps = idx.dependencies_of(app_cpp);
    ASSERT_EQ(deps.size(), 1U);
    EXPECT_EQ(deps[0].target_file_id, log_h);
    EXPECT_EQ(deps[0].kind, "include");
}

TEST(DependencyResolverTest, Cpp_IncludeInSameDirectoryResolves)
{
    CodeIndex idx;
    const auto widget_cpp = add(idx, "src/widget.cpp", Language::Cpp);
    const auto widget_hpp = add(idx, "src/widget.hpp", Language::Cpp);

    std::vector<FileImports> per_file;
    per_file.push_back(make_fi(widget_cpp, Language::Cpp, "src/widget.cpp",
                               {RawImport{"widget.hpp", "include", 1}}));

    resolve_all(idx, "/fake/project", per_file);

    const auto deps = idx.dependencies_of(widget_cpp);
    ASSERT_EQ(deps.size(), 1U);
    EXPECT_EQ(deps[0].target_file_id, widget_hpp);
}

TEST(DependencyResolverTest, Cpp_StdlibIncludeIsExternal)
{
    CodeIndex idx;
    const auto app_cpp = add(idx, "core/app.cpp", Language::Cpp);

    std::vector<FileImports> per_file;
    per_file.push_back(make_fi(app_cpp, Language::Cpp, "core/app.cpp",
                               {RawImport{"nonexistent/totally-not-here.h", "include", 1}}));

    resolve_all(idx, "/fake/project", per_file);

    const auto deps = idx.dependencies_of(app_cpp);
    ASSERT_EQ(deps.size(), 1U);
    EXPECT_EQ(deps[0].target_file_id, 0);
    EXPECT_EQ(deps[0].import_string, "nonexistent/totally-not-here.h");
}

TEST(DependencyResolverTest, Python_DottedNameResolvesToFile)
{
    CodeIndex idx;
    const auto main_py = add(idx, "main.py", Language::Python);
    const auto user_py = add(idx, "models/user.py", Language::Python);
    const auto helpers_py = add(idx, "utils/helpers.py", Language::Python);

    std::vector<FileImports> per_file;
    per_file.push_back(
        make_fi(main_py, Language::Python, "main.py",
                {
                    RawImport{"models.user", "import", 1}, RawImport{"utils.helpers", "import", 2},
                    RawImport{"os", "import", 3}, // external
                }));

    resolve_all(idx, "/fake/project", per_file);

    const auto deps = idx.dependencies_of(main_py);
    ASSERT_EQ(deps.size(), 3U);

    bool saw_user = false;
    bool saw_helpers = false;
    bool saw_os = false;
    for (const auto& d : deps) {
        if (d.import_string == "models.user") {
            saw_user = true;
            EXPECT_EQ(d.target_file_id, user_py);
        }
        if (d.import_string == "utils.helpers") {
            saw_helpers = true;
            EXPECT_EQ(d.target_file_id, helpers_py);
        }
        if (d.import_string == "os") {
            saw_os = true;
            EXPECT_EQ(d.target_file_id, 0);
        }
    }
    EXPECT_TRUE(saw_user);
    EXPECT_TRUE(saw_helpers);
    EXPECT_TRUE(saw_os);
}

TEST(DependencyResolverTest, Python_RelativeImport_SameDirectorySibling)
{
    CodeIndex idx;
    const auto foo_py = add(idx, "src/pkg/foo.py", Language::Python);
    const auto bar_py = add(idx, "src/pkg/bar.py", Language::Python);

    std::vector<FileImports> per_file;
    per_file.push_back(
        make_fi(foo_py, Language::Python, "src/pkg/foo.py", {RawImport{".bar", "import", 1}}));

    resolve_all(idx, "/fake/project", per_file);

    const auto deps = idx.dependencies_of(foo_py);
    ASSERT_EQ(deps.size(), 1U);
    EXPECT_EQ(deps[0].target_file_id, bar_py);
    EXPECT_EQ(deps[0].import_string, ".bar");
}

TEST(DependencyResolverTest, Python_RelativeImport_ParentPackageModule)
{
    CodeIndex idx;
    const auto sub_foo = add(idx, "src/pkg/sub/foo.py", Language::Python);
    const auto models = add(idx, "src/pkg/models.py", Language::Python);

    std::vector<FileImports> per_file;
    per_file.push_back(make_fi(sub_foo, Language::Python, "src/pkg/sub/foo.py",
                               {RawImport{"..models", "import", 1}}));

    resolve_all(idx, "/fake/project", per_file);

    const auto deps = idx.dependencies_of(sub_foo);
    ASSERT_EQ(deps.size(), 1U);
    EXPECT_EQ(deps[0].target_file_id, models);
}

TEST(DependencyResolverTest, Python_RelativeImport_ParentPackageSubmodule)
{
    CodeIndex idx;
    const auto sub_foo = add(idx, "src/pkg/sub/foo.py", Language::Python);
    const auto user = add(idx, "src/pkg/models/user.py", Language::Python);

    std::vector<FileImports> per_file;
    per_file.push_back(make_fi(sub_foo, Language::Python, "src/pkg/sub/foo.py",
                               {RawImport{"..models.user", "import", 1}}));

    resolve_all(idx, "/fake/project", per_file);

    const auto deps = idx.dependencies_of(sub_foo);
    ASSERT_EQ(deps.size(), 1U);
    EXPECT_EQ(deps[0].target_file_id, user);
}

TEST(DependencyResolverTest, Python_RelativeImport_ResolvesToPackageInit)
{
    CodeIndex idx;
    const auto sub_foo = add(idx, "src/pkg/sub/foo.py", Language::Python);
    const auto models_pk = add(idx, "src/pkg/models/__init__.py", Language::Python);

    std::vector<FileImports> per_file;
    per_file.push_back(make_fi(sub_foo, Language::Python, "src/pkg/sub/foo.py",
                               {RawImport{"..models", "import", 1}}));

    resolve_all(idx, "/fake/project", per_file);

    const auto deps = idx.dependencies_of(sub_foo);
    ASSERT_EQ(deps.size(), 1U);
    EXPECT_EQ(deps[0].target_file_id, models_pk);
}

TEST(DependencyResolverTest, Python_RelativeImport_ThreeDotsWalksUpTwoLevels)
{
    CodeIndex idx;
    const auto deep_py = add(idx, "src/a/b/c/leaf.py", Language::Python);
    const auto top_py = add(idx, "src/a/shared.py", Language::Python);

    std::vector<FileImports> per_file;
    per_file.push_back(make_fi(deep_py, Language::Python, "src/a/b/c/leaf.py",
                               {RawImport{"...shared", "import", 1}}));

    resolve_all(idx, "/fake/project", per_file);

    const auto deps = idx.dependencies_of(deep_py);
    ASSERT_EQ(deps.size(), 1U);
    EXPECT_EQ(deps[0].target_file_id, top_py);
}

TEST(DependencyResolverTest, Python_RelativeImport_NotConfusedWithTopLevelModule)
{
    // Regression guard: "..models" from a sub-package must NOT resolve
    // to a top-level `models.py`. Before the relative-import fix,
    // split_dotted dropped the leading dots and matched the repo-root
    // file — corrupting the internal graph.
    CodeIndex idx;
    const auto sub_foo = add(idx, "src/pkg/sub/foo.py", Language::Python);
    const auto parent_models = add(idx, "src/pkg/models.py", Language::Python);
    const auto root_models = add(idx, "models.py", Language::Python);

    std::vector<FileImports> per_file;
    per_file.push_back(make_fi(sub_foo, Language::Python, "src/pkg/sub/foo.py",
                               {RawImport{"..models", "import", 1}}));

    resolve_all(idx, "/fake/project", per_file);

    const auto deps = idx.dependencies_of(sub_foo);
    ASSERT_EQ(deps.size(), 1U);
    EXPECT_EQ(deps[0].target_file_id, parent_models);
    EXPECT_NE(deps[0].target_file_id, root_models);
}

TEST(DependencyResolverTest, Python_RelativeImport_WalkingAboveRootStaysExternal)
{
    CodeIndex idx;
    const auto main_py = add(idx, "main.py", Language::Python);

    std::vector<FileImports> per_file;
    per_file.push_back(
        make_fi(main_py, Language::Python, "main.py", {RawImport{"..outside", "import", 1}}));

    resolve_all(idx, "/fake/project", per_file);

    const auto deps = idx.dependencies_of(main_py);
    ASSERT_EQ(deps.size(), 1U);
    EXPECT_EQ(deps[0].target_file_id, 0); // external / unresolved
}

TEST(DependencyResolverTest, TypeScript_RelativeImportResolvesWithExtension)
{
    CodeIndex idx;
    const auto index_ts = add(idx, "src/index.ts", Language::TypeScript);
    const auto types_ts = add(idx, "src/types.ts", Language::TypeScript);
    const auto svc_ts = add(idx, "src/services/user-service.ts", Language::TypeScript);

    std::vector<FileImports> per_file;
    per_file.push_back(make_fi(index_ts, Language::TypeScript, "src/index.ts",
                               {
                                   RawImport{"./types", "import", 1},
                                   RawImport{"./services/user-service", "import", 2},
                                   RawImport{"react", "import", 3}, // external
                               }));

    resolve_all(idx, "/fake/project", per_file);

    const auto deps = idx.dependencies_of(index_ts);
    ASSERT_EQ(deps.size(), 3U);
    bool saw_types = false;
    bool saw_svc = false;
    bool saw_react = false;
    for (const auto& d : deps) {
        if (d.import_string == "./types") {
            saw_types = true;
            EXPECT_EQ(d.target_file_id, types_ts);
        }
        if (d.import_string == "./services/user-service") {
            saw_svc = true;
            EXPECT_EQ(d.target_file_id, svc_ts);
        }
        if (d.import_string == "react") {
            saw_react = true;
            EXPECT_EQ(d.target_file_id, 0);
        }
    }
    EXPECT_TRUE(saw_types);
    EXPECT_TRUE(saw_svc);
    EXPECT_TRUE(saw_react);
}

TEST(DependencyResolverTest, EmptyInput_NoopAndNoError)
{
    CodeIndex idx;
    add(idx, "core/app.cpp", Language::Cpp);

    resolve_all(idx, "/fake/project", {});
    EXPECT_EQ(idx.dependency_count(), 0U);
}

// -----------------------------------------------------------------------------
// Java — `import com.foo.*;` (wildcard) falls back to the package index
// and emits one edge per file declaring `package com.foo`. Specific
// imports still resolve via the existing path-based matcher.
// -----------------------------------------------------------------------------
TEST(DependencyResolverTest, Java_WildcardImportResolvesToEveryFileInPackage)
{
    CodeIndex idx;
    const auto main_j = add(idx, "Main.java", Language::Java);
    const auto bar_j = add(idx, "com/example/foo/Bar.java", Language::Java);
    const auto baz_j = add(idx, "com/example/foo/Baz.java", Language::Java);

    std::vector<FileImports> per_file;
    per_file.push_back(make_fi(main_j, Language::Java, "Main.java",
                               {RawImport{"com.example.foo", "import", 3}} // the `.*` is stripped
                               ));
    per_file.push_back(
        make_fi(bar_j, Language::Java, "com/example/foo/Bar.java", {}, {"com.example.foo"}));
    per_file.push_back(
        make_fi(baz_j, Language::Java, "com/example/foo/Baz.java", {}, {"com.example.foo"}));

    resolve_all(idx, "/fake/java", per_file);

    bool main_to_bar = false;
    bool main_to_baz = false;
    for (const Dependency& d : idx.dependencies_of(main_j)) {
        if (d.target_file_id == bar_j)
            main_to_bar = true;
        if (d.target_file_id == baz_j)
            main_to_baz = true;
    }
    EXPECT_TRUE(main_to_bar);
    EXPECT_TRUE(main_to_baz);
}

// -----------------------------------------------------------------------------
// C# — `using X.Y;` resolves to every file declaring `namespace X.Y`.
// -----------------------------------------------------------------------------
TEST(DependencyResolverTest, CSharp_UsingResolvesToEveryFileInNamespace)
{
    CodeIndex idx;
    const auto prog = add(idx, "Program.cs", Language::CSharp);
    const auto user = add(idx, "Models/User.cs", Language::CSharp);
    const auto order = add(idx, "Models/Order.cs", Language::CSharp);
    const auto svc = add(idx, "Services/UserService.cs", Language::CSharp);

    std::vector<FileImports> per_file;
    per_file.push_back(make_fi(
        prog, Language::CSharp, "Program.cs",
        {RawImport{"SampleApp.Models", "use", 2}, RawImport{"SampleApp.Services", "use", 3}},
        {"SampleApp"}));
    per_file.push_back(make_fi(user, Language::CSharp, "Models/User.cs", {}, {"SampleApp.Models"}));
    per_file.push_back(
        make_fi(order, Language::CSharp, "Models/Order.cs", {}, {"SampleApp.Models"}));
    per_file.push_back(make_fi(svc, Language::CSharp, "Services/UserService.cs",
                               {RawImport{"SampleApp.Models", "use", 1}}, {"SampleApp.Services"}));

    resolve_all(idx, "/fake/csharp", per_file);

    // Program.cs → 2 files in Models + 1 file in Services = 3 internal edges
    // UserService.cs → 2 files in Models = 2 internal edges
    // Total internal: 5. Plus the two unresolved `SampleApp` references?
    // No: `using SampleApp` does match the Program.cs declaration, but
    // we skip self-edges, so it yields 0.
    bool prog_to_user = false;
    bool prog_to_order = false;
    bool prog_to_svc = false;
    bool svc_to_user = false;
    for (const Dependency& d : idx.dependencies_of(prog)) {
        if (d.target_file_id == user)
            prog_to_user = true;
        if (d.target_file_id == order)
            prog_to_order = true;
        if (d.target_file_id == svc)
            prog_to_svc = true;
    }
    for (const Dependency& d : idx.dependencies_of(svc)) {
        if (d.target_file_id == user)
            svc_to_user = true;
    }
    EXPECT_TRUE(prog_to_user);
    EXPECT_TRUE(prog_to_order);
    EXPECT_TRUE(prog_to_svc);
    EXPECT_TRUE(svc_to_user);
}

// -----------------------------------------------------------------------------
// PHP — `use App\Services\X;` resolves to every file declaring the namespace.
// -----------------------------------------------------------------------------
TEST(DependencyResolverTest, Php_UseResolvesViaNamespaceIndex)
{
    CodeIndex idx;
    const auto idx_php = add(idx, "index.php", Language::Php);
    const auto svc_php = add(idx, "src/UserService.php", Language::Php);

    std::vector<FileImports> per_file;
    per_file.push_back(
        make_fi(idx_php, Language::Php, "index.php", {RawImport{R"(App\Services)", "use", 3}}));
    per_file.push_back(
        make_fi(svc_php, Language::Php, "src/UserService.php", {}, {R"(App\Services)"}));

    resolve_all(idx, "/fake/php", per_file);

    bool linked = false;
    for (const Dependency& d : idx.dependencies_of(idx_php)) {
        if (d.target_file_id == svc_php)
            linked = true;
    }
    EXPECT_TRUE(linked);
}

// -----------------------------------------------------------------------------
// Go — imports that start with the module prefix from `go.mod` resolve to
// every `.go` file in the named package directory.
// -----------------------------------------------------------------------------
TEST(DependencyResolverTest, Go_ModuleImportResolvesToPackageFiles)
{
    namespace fs = std::filesystem;

    // Stage a temp project root with a real go.mod so the resolver
    // can read the module prefix from disk.
    const fs::path tmp =
        fs::temp_directory_path() /
        ("vectis_go_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(&tmp)));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    {
        std::ofstream out(tmp / "go.mod");
        out << "module example.com/sample\n\ngo 1.22\n";
    }

    CodeIndex idx;
    const auto main_go = add(idx, "main.go", Language::Go);
    const auto user_go = add(idx, "user/user.go", Language::Go);
    const auto util_go = add(idx, "user/util.go", Language::Go);

    std::vector<FileImports> per_file;
    per_file.push_back(make_fi(
        main_go, Language::Go, "main.go",
        {RawImport{"fmt", "import", 3}, RawImport{"example.com/sample/user", "import", 4}}));

    resolve_all(idx, tmp, per_file);

    bool main_to_user = false;
    bool main_to_util = false;
    for (const Dependency& d : idx.dependencies_of(main_go)) {
        if (d.target_file_id == user_go)
            main_to_user = true;
        if (d.target_file_id == util_go)
            main_to_util = true;
    }
    EXPECT_TRUE(main_to_user);
    EXPECT_TRUE(main_to_util);

    fs::remove_all(tmp, ec);
}

// -----------------------------------------------------------------------------
// PHP — `use Slim\Factory\X;` resolves via PSR-4 path matching, with the
// project's own namespace tree under any source root (`Slim/`, `src/Slim/`,
// `lib/Slim/`).
// -----------------------------------------------------------------------------
TEST(DependencyResolverTest, Php_UseResolvesByPsr4PathSuffix)
{
    CodeIndex idx;
    const auto app_php = add(idx, "src/Slim/App.php", Language::Php);
    const auto fact_php = add(idx, "src/Slim/Factory/RequestFactory.php", Language::Php);

    std::vector<FileImports> per_file;
    per_file.push_back(make_fi(app_php, Language::Php, "src/Slim/App.php",
                               {RawImport{R"(Slim\Factory\RequestFactory)", "use", 1}}, {"Slim"}));
    per_file.push_back(make_fi(fact_php, Language::Php, "src/Slim/Factory/RequestFactory.php", {},
                               {R"(Slim\Factory)"}));

    resolve_all(idx, "/fake/php", per_file);

    bool linked = false;
    for (const Dependency& d : idx.dependencies_of(app_php)) {
        if (d.target_file_id == fact_php) {
            linked = true;
        }
    }
    EXPECT_TRUE(linked);
}

// -----------------------------------------------------------------------------
// Ruby — bare `require 'sinatra/base'` from outside `lib/` resolves to
// `lib/sinatra/base.rb` via load-path-style suffix matching. The previous
// implementation joined to source dir and silently classified every
// gem-style require as external.
// -----------------------------------------------------------------------------
TEST(DependencyResolverTest, Ruby_RequireResolvesByLoadPathSuffix)
{
    CodeIndex idx;
    const auto base_rb = add(idx, "lib/sinatra/base.rb", Language::Ruby);
    const auto test_rb = add(idx, "test/integration/x_test.rb", Language::Ruby);

    std::vector<FileImports> per_file;
    per_file.push_back(make_fi(test_rb, Language::Ruby, "test/integration/x_test.rb",
                               {RawImport{"sinatra/base", "require", 1}}));

    resolve_all(idx, "/fake/ruby", per_file);

    bool linked = false;
    for (const Dependency& d : idx.dependencies_of(test_rb)) {
        if (d.target_file_id == base_rb) {
            linked = true;
        }
    }
    EXPECT_TRUE(linked);
}

TEST(DependencyResolverTest, Java_DottedCandidates_DirectMatchListedFirst)
{
    using vectis::code::match_java_dotted_candidates;
    std::vector<FileEntry> files;
    files.push_back(FileEntry{
        .id = 1, .path_relative = "src/main/java/com/x/Foo.java", .language = Language::Java});
    files.push_back(FileEntry{.id = 2,
                              .path_relative = "com/x/Foo.java", // direct
                              .language = Language::Java});
    files.push_back(
        FileEntry{.id = 3, .path_relative = "lib/com/x/Foo.java", .language = Language::Java});

    const auto candidates = match_java_dotted_candidates(files, "com.x.Foo");

    ASSERT_EQ(candidates.size(), 3U);
    EXPECT_EQ(candidates[0], 2); // direct match wins position 0
    EXPECT_EQ(candidates[1], 1); // suffix matches follow in insertion order
    EXPECT_EQ(candidates[2], 3);
}

TEST(DependencyResolverTest, Java_DottedCandidates_NoDirect_SuffixOnly_InsertionOrder)
{
    using vectis::code::match_java_dotted_candidates;
    std::vector<FileEntry> files;
    files.push_back(FileEntry{
        .id = 1, .path_relative = "src/main/java/com/x/Foo.java", .language = Language::Java});
    files.push_back(FileEntry{
        .id = 2, .path_relative = "src/test/java/com/x/Foo.java", .language = Language::Java});

    const auto candidates = match_java_dotted_candidates(files, "com.x.Foo");

    ASSERT_EQ(candidates.size(), 2U);
    EXPECT_EQ(candidates[0], 1); // insertion order preserved
    EXPECT_EQ(candidates[1], 2);
}

TEST(DependencyResolverTest, Java_DottedCandidates_NoMatch_EmptyVector)
{
    using vectis::code::match_java_dotted_candidates;
    std::vector<FileEntry> files;
    files.push_back(FileEntry{
        .id = 1, .path_relative = "src/main/java/com/x/Bar.java", .language = Language::Java});

    const auto candidates = match_java_dotted_candidates(files, "com.x.Foo");

    EXPECT_TRUE(candidates.empty());
}

} // namespace
