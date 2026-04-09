#include "modes/code/dependency_resolver.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "modes/code/code_index.h"
#include "modes/code/dependency.h"
#include "modes/code/language.h"
#include "modes/code/parser.h"
#include "modes/code/symbol.h"

namespace {

using vectis::modes::code::CodeIndex;
using vectis::modes::code::Dependency;
using vectis::modes::code::FileEntry;
using vectis::modes::code::FileImports;
using vectis::modes::code::Language;
using vectis::modes::code::RawImport;
using vectis::modes::code::resolve_all;

/// Helper: add a file to the index with the given relative path and
/// return its assigned id.
std::int64_t add(CodeIndex& idx, const std::string& relative, Language lang)
{
    FileEntry f;
    f.path_relative = relative;
    f.language      = lang;
    return idx.add_file(std::move(f));
}

/// Helper: build a FileImports record for the given source file.
FileImports make_fi(std::int64_t file_id,
                    Language lang,
                    const std::string& relative_path,
                    std::vector<RawImport> imports)
{
    FileImports fi;
    fi.file_id       = file_id;
    fi.language      = lang;
    fi.relative_path = relative_path;
    fi.imports       = std::move(imports);
    return fi;
}

TEST(DependencyResolverTest, Cpp_ResolvesQuotedIncludeByEndsWith)
{
    CodeIndex idx;
    const auto app_cpp = add(idx, "core/app.cpp", Language::Cpp);
    const auto log_h   = add(idx, "core/log.h",   Language::Cpp);

    std::vector<FileImports> per_file;
    per_file.push_back(make_fi(
        app_cpp, Language::Cpp, "core/app.cpp",
        {RawImport{"core/log.h", "include", 5}}));

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
    per_file.push_back(make_fi(
        widget_cpp, Language::Cpp, "src/widget.cpp",
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
    per_file.push_back(make_fi(
        app_cpp, Language::Cpp, "core/app.cpp",
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
    const auto main_py     = add(idx, "main.py",         Language::Python);
    const auto user_py     = add(idx, "models/user.py",  Language::Python);
    const auto helpers_py  = add(idx, "utils/helpers.py", Language::Python);

    std::vector<FileImports> per_file;
    per_file.push_back(make_fi(
        main_py, Language::Python, "main.py",
        {
            RawImport{"models.user",   "import", 1},
            RawImport{"utils.helpers", "import", 2},
            RawImport{"os",            "import", 3},  // external
        }));

    resolve_all(idx, "/fake/project", per_file);

    const auto deps = idx.dependencies_of(main_py);
    ASSERT_EQ(deps.size(), 3U);

    bool saw_user    = false;
    bool saw_helpers = false;
    bool saw_os      = false;
    for (const auto& d : deps) {
        if (d.import_string == "models.user")    { saw_user    = true; EXPECT_EQ(d.target_file_id, user_py); }
        if (d.import_string == "utils.helpers")  { saw_helpers = true; EXPECT_EQ(d.target_file_id, helpers_py); }
        if (d.import_string == "os")             { saw_os      = true; EXPECT_EQ(d.target_file_id, 0); }
    }
    EXPECT_TRUE(saw_user);
    EXPECT_TRUE(saw_helpers);
    EXPECT_TRUE(saw_os);
}

TEST(DependencyResolverTest, TypeScript_RelativeImportResolvesWithExtension)
{
    CodeIndex idx;
    const auto index_ts = add(idx, "src/index.ts",                        Language::TypeScript);
    const auto types_ts = add(idx, "src/types.ts",                        Language::TypeScript);
    const auto svc_ts   = add(idx, "src/services/user-service.ts",        Language::TypeScript);

    std::vector<FileImports> per_file;
    per_file.push_back(make_fi(
        index_ts, Language::TypeScript, "src/index.ts",
        {
            RawImport{"./types",                  "import", 1},
            RawImport{"./services/user-service",  "import", 2},
            RawImport{"react",                    "import", 3},  // external
        }));

    resolve_all(idx, "/fake/project", per_file);

    const auto deps = idx.dependencies_of(index_ts);
    ASSERT_EQ(deps.size(), 3U);
    bool saw_types = false;
    bool saw_svc = false;
    bool saw_react = false;
    for (const auto& d : deps) {
        if (d.import_string == "./types")                 { saw_types = true; EXPECT_EQ(d.target_file_id, types_ts); }
        if (d.import_string == "./services/user-service") { saw_svc   = true; EXPECT_EQ(d.target_file_id, svc_ts); }
        if (d.import_string == "react")                   { saw_react = true; EXPECT_EQ(d.target_file_id, 0); }
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

} // namespace
