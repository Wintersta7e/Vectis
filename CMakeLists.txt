cmake_minimum_required(VERSION 3.25)
project(Vectis
    VERSION 1.0.0
    DESCRIPTION "Portable Developer Intelligence Tool"
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Always produce compile_commands.json for clang-tidy / IDE use.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Build options -----------------------------------------------------------
#
# VECTIS_STATIC_LINK: produce a (mostly) statically linked binary suitable
# for distribution. Requires all dependency libraries to be available as
# static archives (.a / .lib) — typically only the case when building
# against vcpkg. Off by default so system-package dev builds (apt) work
# without extra setup.
option(VECTIS_STATIC_LINK "Link Vectis statically for distribution builds" OFF)
option(VECTIS_BUILD_TESTS "Build unit tests" ON)

# --- Strict warnings ---------------------------------------------------------
# Applied project-wide via add_compile_options; affects all targets declared
# *after* this point. Third-party grammar sources that can't meet these
# standards get per-target overrides below.
if(MSVC)
    add_compile_options(/W4 /WX /permissive-)
    # Silence MSVC's CRT "secure" deprecation on std C functions (std::getenv,
    # std::strncpy, ...). Our code uses these intentionally; the "safer"
    # _dupenv_s/strncpy_s replacements are Windows-only and break portability.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Werror -Wpedantic)
    # gcc's -Wmissing-field-initializers fires when callers use C++20
    # designated initialisers and leave fields off. The warning predates
    # the feature and produces false positives on every Symbol-like
    # struct in our tests. clang doesn't warn here. We keep -Wextra but
    # silence this one piece — it has not caught a real bug in this
    # codebase and the workaround (`= {}` on every member) is noise.
    add_compile_options(-Wno-missing-field-initializers)
    # gcc 13 + fmt v9 (transitively pulled in via spdlog) emits a
    # known false-positive pair of warnings (`-Werror=array-bounds`
    # and `-Werror=stringop-overflow`) from inside fmt's float-
    # formatting bigint code under -O3 inlining. None of our code
    # touches fmt's float formatter directly; entirely a third-
    # party-header issue that breaks the Release CI build.
    # `-Wstringop-overflow` is gcc-only — clang/clang-tidy reject
    # the suppression as an unknown warning option.
    add_compile_options(-Wno-array-bounds)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_compile_options(-Wno-stringop-overflow)
    endif()
endif()

# --- Static linking (opt-in) -------------------------------------------------
# "Static" here means our own libs + C/C++ runtime on non-MSVC. System
# libraries (X11, Wayland, etc.) on Linux remain dynamic — that is expected.
if(VECTIS_STATIC_LINK)
    set(BUILD_SHARED_LIBS OFF)
    if(MSVC)
        # .lib on MSVC — leave CMAKE_FIND_LIBRARY_SUFFIXES at its default
        # (.lib) so find_library() resolves vcpkg's static archives.
    else()
        set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
        set(CMAKE_EXE_LINKER_FLAGS
            "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")
    endif()
endif()

# --- Dependencies ------------------------------------------------------------
# Dual-mode find_package: omitting CONFIG makes CMake try Module mode first,
# then Config mode. This lets the same CMakeLists work with either vcpkg
# (config) or system apt packages (module or config), picking whichever
# the current environment provides.
include(FetchContent)

find_package(nlohmann_json REQUIRED)
find_package(tomlplusplus REQUIRED)
find_package(fmt          REQUIRED)
find_package(spdlog       REQUIRED)

# For static/distribution builds, pull spdlog and fmt in header-only so the
# binary carries no libspdlog.so / libfmt.so dependency (which would also drag
# libstdc++.so back in transitively, defeating -static-libstdc++). Both the
# apt and vcpkg configs expose these header-only targets.
if(VECTIS_STATIC_LINK)
    set(VECTIS_FMT_TARGET fmt::fmt-header-only)
    set(VECTIS_SPDLOG_TARGET spdlog::spdlog_header_only)
else()
    set(VECTIS_FMT_TARGET fmt::fmt)
    set(VECTIS_SPDLOG_TARGET spdlog::spdlog)
endif()

# Header-only wrapper around tomlplusplus. Debian/Ubuntu's
# tomlplusplusConfig.cmake binds the target to libtomlplusplus.so and
# defines TOML_HEADER_ONLY=0, which would force every portable-bundle
# target machine to install libtomlplusplusN. Re-export the headers
# with TOML_HEADER_ONLY=1 so the implementation inlines into vectis and
# the binary has no toml++ runtime dependency. vcpkg's port is already
# INTERFACE-only, so this collapses to a no-op there.
add_library(vectis_tomlplusplus INTERFACE)
get_target_property(_vectis_tomlpp_includes
    tomlplusplus::tomlplusplus INTERFACE_INCLUDE_DIRECTORIES)
if(_vectis_tomlpp_includes)
    target_include_directories(vectis_tomlplusplus SYSTEM INTERFACE
        ${_vectis_tomlpp_includes})
endif()
target_compile_definitions(vectis_tomlplusplus INTERFACE TOML_HEADER_ONLY=1)

# SQLite3: vcpkg ships `unofficial-sqlite3`, system provides `SQLite3`.
# Try the system path first; fall back to vcpkg's unofficial name.
find_package(SQLite3 QUIET)
if(SQLite3_FOUND)
    set(VECTIS_SQLITE3_TARGET SQLite::SQLite3)
else()
    find_package(unofficial-sqlite3 CONFIG REQUIRED)
    set(VECTIS_SQLITE3_TARGET unofficial::sqlite3::sqlite3)
endif()

# tl-expected: not packaged in common Linux distros. Try the vcpkg target
# first; otherwise fetch the header-only source tree.
find_package(tl-expected CONFIG QUIET)
if(NOT tl-expected_FOUND)
    FetchContent_Declare(
        tl_expected
        GIT_REPOSITORY https://github.com/TartanLlama/expected.git
        GIT_TAG        v1.1.0
    )
    # Tell upstream not to build its own tests/examples.
    set(EXPECTED_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(tl_expected)
endif()

# --- Tree-sitter core --------------------------------------------------------
# Download only — upstream's CMakeLists.txt wants to invoke the tree-sitter
# CLI tool to regenerate parsers from grammars, which we don't need and which
# breaks our build with "TREE_SITTER_CLI-NOTFOUND". Compile the core
# runtime ourselves from lib.c.
FetchContent_Declare(
    tree_sitter
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
    # v0.25.9 (resolved 2026-04-09) — pinned SHA for reproducible clones.
    GIT_TAG        a467ea8502d95562171f97953a6dc5b2a8622609
)
FetchContent_GetProperties(tree_sitter)
if(NOT tree_sitter_POPULATED)
    FetchContent_Populate(tree_sitter)
endif()

add_library(tree_sitter_lib STATIC
    ${tree_sitter_SOURCE_DIR}/lib/src/lib.c
)
target_include_directories(tree_sitter_lib
    PUBLIC  ${tree_sitter_SOURCE_DIR}/lib/include
    PRIVATE ${tree_sitter_SOURCE_DIR}/lib/src
)
set_target_properties(tree_sitter_lib PROPERTIES
    C_STANDARD     11
    C_CLANG_TIDY   ""
    CXX_CLANG_TIDY ""
)
if(MSVC)
    target_compile_options(tree_sitter_lib PRIVATE /w)
else()
    target_compile_options(tree_sitter_lib PRIVATE -w)
endif()

# Reusable grammar-target helper. Given a stem name, the FetchContent
# project variable, and an optional subdirectory inside the repo, builds
# a static library ts_grammar_<stem> from parser.c (always) and
# scanner.c/scanner.cc (whichever exists). All grammar targets get
# warnings silenced and clang-tidy disabled — the generated parser.c
# sources routinely fail every strict check Vectis enables.
function(vectis_add_ts_grammar stem repo_var subdir)
    if(subdir STREQUAL "")
        set(src_dir ${${repo_var}_SOURCE_DIR}/src)
    else()
        set(src_dir ${${repo_var}_SOURCE_DIR}/${subdir}/src)
    endif()

    set(sources ${src_dir}/parser.c)
    foreach(ext IN ITEMS c cc)
        if(EXISTS ${src_dir}/scanner.${ext})
            list(APPEND sources ${src_dir}/scanner.${ext})
        endif()
    endforeach()

    add_library(ts_grammar_${stem} STATIC ${sources})
    target_include_directories(ts_grammar_${stem}
        PRIVATE ${src_dir} ${tree_sitter_SOURCE_DIR}/lib/include
    )
    set_target_properties(ts_grammar_${stem} PROPERTIES
        C_CLANG_TIDY   ""
        CXX_CLANG_TIDY ""
    )
    if(MSVC)
        target_compile_options(ts_grammar_${stem} PRIVATE /w)
    else()
        target_compile_options(ts_grammar_${stem} PRIVATE -w)
    endif()
endfunction()

# --- Tree-sitter grammars ----------------------------------------------------
# Download-only, for the same reason as tree-sitter core above. Every
# declaration is paired with a pinned SHA so fresh clones are reproducible.

FetchContent_Declare(
    ts_python
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-python.git
    # v0.23.6 (resolved 2026-04-09)
    GIT_TAG        bffb65a8cfe4e46290331dfef0dbf0ef3679de11
)
FetchContent_GetProperties(ts_python)
if(NOT ts_python_POPULATED)
    FetchContent_Populate(ts_python)
endif()
vectis_add_ts_grammar(python ts_python "")

FetchContent_Declare(
    ts_javascript
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-javascript.git
    # v0.23.1 (resolved 2026-04-09)
    GIT_TAG        3a837b6f3658ca3618f2022f8707e29739c91364
)
FetchContent_GetProperties(ts_javascript)
if(NOT ts_javascript_POPULATED)
    FetchContent_Populate(ts_javascript)
endif()
vectis_add_ts_grammar(javascript ts_javascript "")

FetchContent_Declare(
    ts_typescript
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-typescript.git
    # v0.23.2 (resolved 2026-04-09)
    GIT_TAG        f975a621f4e7f532fe322e13c4f79495e0a7b2e7
)
FetchContent_GetProperties(ts_typescript)
if(NOT ts_typescript_POPULATED)
    FetchContent_Populate(ts_typescript)
endif()
# The TypeScript repo nests the parser sources under typescript/src; the
# tsx parser (typescript-repo's tsx/src) is intentionally not wired —
# .tsx files detect as TypeScript and parse with this grammar acceptably
# for symbol extraction, which is all Step 2 needs.
vectis_add_ts_grammar(typescript ts_typescript "typescript")

FetchContent_Declare(
    ts_c
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-c.git
    # v0.24.1 (resolved 2026-04-09)
    GIT_TAG        7fa1be1b694b6e763686793d97da01f36a0e5c12
)
FetchContent_GetProperties(ts_c)
if(NOT ts_c_POPULATED)
    FetchContent_Populate(ts_c)
endif()
vectis_add_ts_grammar(c ts_c "")

FetchContent_Declare(
    ts_cpp
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-cpp.git
    # v0.23.4 (resolved 2026-04-09)
    GIT_TAG        f41e1a044c8a84ea9fa8577fdd2eab92ec96de02
)
FetchContent_GetProperties(ts_cpp)
if(NOT ts_cpp_POPULATED)
    FetchContent_Populate(ts_cpp)
endif()
vectis_add_ts_grammar(cpp ts_cpp "")

FetchContent_Declare(
    ts_rust
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-rust.git
    # v0.24.2 (resolved 2026-04-09)
    GIT_TAG        77a3747266f4d621d0757825e6b11edcbf991ca5
)
FetchContent_GetProperties(ts_rust)
if(NOT ts_rust_POPULATED)
    FetchContent_Populate(ts_rust)
endif()
vectis_add_ts_grammar(rust ts_rust "")

FetchContent_Declare(
    ts_java
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-java.git
    # v0.23.5 (resolved 2026-04-09)
    GIT_TAG        94703d5a6bed02b98e438d7cad1136c01a60ba2c
)
FetchContent_GetProperties(ts_java)
if(NOT ts_java_POPULATED)
    FetchContent_Populate(ts_java)
endif()
vectis_add_ts_grammar(java ts_java "")

FetchContent_Declare(
    ts_csharp
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-c-sharp.git
    # v0.23.1 (resolved 2026-04-09)
    GIT_TAG        362a8a41b265056592a0c3771664a21d23a71392
)
FetchContent_GetProperties(ts_csharp)
if(NOT ts_csharp_POPULATED)
    FetchContent_Populate(ts_csharp)
endif()
vectis_add_ts_grammar(csharp ts_csharp "")

FetchContent_Declare(
    ts_go
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-go.git
    # v0.25.0 (resolved 2026-04-09)
    GIT_TAG        6048bfc6e5238eaf062c2221bd934489c39fbb61
)
FetchContent_GetProperties(ts_go)
if(NOT ts_go_POPULATED)
    FetchContent_Populate(ts_go)
endif()
vectis_add_ts_grammar(go ts_go "")

FetchContent_Declare(
    ts_ruby
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-ruby.git
    # v0.23.1 (resolved 2026-04-09)
    GIT_TAG        71bd32fb7607035768799732addba884a37a6210
)
FetchContent_GetProperties(ts_ruby)
if(NOT ts_ruby_POPULATED)
    FetchContent_Populate(ts_ruby)
endif()
vectis_add_ts_grammar(ruby ts_ruby "")

FetchContent_Declare(
    ts_php
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-php.git
    # v0.24.2 (resolved 2026-04-09)
    GIT_TAG        094172694b05589f8b394017d6821a07055c1a61
)
FetchContent_GetProperties(ts_php)
if(NOT ts_php_POPULATED)
    FetchContent_Populate(ts_php)
endif()
# tree-sitter-php nests the main parser under `php/src/`. There's also a
# `php_only/` variant for pure PHP (no embedded HTML) which we skip —
# the main grammar handles both regular .php and pure scripts.
vectis_add_ts_grammar(php ts_php "php")

FetchContent_Declare(
    ts_sql
    GIT_REPOSITORY https://github.com/m-novikov/tree-sitter-sql.git
    # master @ 587f30d (resolved 2026-04-09). This fork has no tags but
    # ships a pre-generated parser.c + scanner.cc (PostgreSQL dialect,
    # which handles Oracle PL/SQL top-level CREATE statements well
    # enough for symbol extraction). DerekStride's fork requires running
    # `tree-sitter generate` and so can't be used via FetchContent alone.
    GIT_TAG        587f30d184b058450be2a2330878210c5f33b3f9
)
FetchContent_GetProperties(ts_sql)
if(NOT ts_sql_POPULATED)
    FetchContent_Populate(ts_sql)
endif()
vectis_add_ts_grammar(sql ts_sql "")

# --- Version header ----------------------------------------------------------
# Single source of truth for the version string: generated from project(VERSION)
# so the CLI, MCP server info, and digest output never drift from CMake.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/core/version.h
    @ONLY
)

# --- Vectis core library -----------------------------------------------------
# Everything Vectis-written lives in a static library so the executable and
# the test runner can link against the same objects without double-compiling.
add_library(vectis_core STATIC
    src/cli/cli_main.cpp
    src/cli/guide.cpp
    src/cli/mcp_server.cpp
    src/core/config_manager.cpp
    src/core/log.cpp
    src/core/service_registry.cpp
    src/core/task_queue.cpp
    src/code/architecture_detector.cpp
    src/code/cmake_inspect.cpp
    src/code/code_index.cpp
    src/code/code_index_store.cpp
    src/code/complexity_analyzer.cpp
    src/code/dependency_graph.cpp
    src/code/dependency_resolver.cpp
    src/code/digest_exporter.cpp
    src/code/dotnet_project.cpp
    src/code/dotnet_project_handler.cpp
    src/code/exclude_dirs.cpp
    src/code/explain.cpp
    src/code/fidelity.cpp
    src/code/framework_hints.cpp
    src/code/gitignore.cpp
    src/code/hotspot_detector.cpp
    src/code/language.cpp
    src/code/manifest_deps.cpp
    src/code/manifest_scanner.cpp
    src/code/maven_pom.cpp
    src/code/maven_pom_handler.cpp
    src/code/pagerank.cpp
    src/code/parser.cpp
    src/code/parser_queries.cpp
    src/code/properties_handler.cpp
    src/code/properties_reader.cpp
    src/code/scanner.cpp
    src/code/spring_xml.cpp
    src/code/spring_xml_handler.cpp
    src/code/xml_reader.cpp
    src/services/index_engine/index_engine.cpp
    src/services/storage_engine/storage_engine.cpp
    src/platform/file_io.cpp
    src/platform/file_watcher.cpp
    src/platform/process.cpp
)

target_include_directories(vectis_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_link_libraries(vectis_core PUBLIC
    tree_sitter_lib
    ts_grammar_python
    ts_grammar_javascript
    ts_grammar_typescript
    ts_grammar_c
    ts_grammar_cpp
    ts_grammar_rust
    ts_grammar_java
    ts_grammar_csharp
    ts_grammar_go
    ts_grammar_ruby
    ts_grammar_php
    ts_grammar_sql
    ${VECTIS_SQLITE3_TARGET}
    nlohmann_json::nlohmann_json
    vectis_tomlplusplus
    ${VECTIS_FMT_TARGET}
    ${VECTIS_SPDLOG_TARGET}
    tl::expected
)

# --- Vectis executable -------------------------------------------------------
# Headless CLI only — no SDL / OpenGL / GUI subsystem.
add_executable(vectis src/main.cpp)
target_link_libraries(vectis PRIVATE vectis_core)

# --- Tests -------------------------------------------------------------------
if(VECTIS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
