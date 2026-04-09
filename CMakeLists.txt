cmake_minimum_required(VERSION 3.25)
project(Vectis
    VERSION 0.1.0
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
# *after* this point. Third-party targets (imgui_lib) that can't meet these
# standards get per-target overrides below.
if(MSVC)
    add_compile_options(/W4 /WX /permissive-)
else()
    add_compile_options(-Wall -Wextra -Werror -Wpedantic)
endif()

# --- Static linking (opt-in) -------------------------------------------------
# "Static" here means our own libs + C/C++ runtime on non-MSVC. System
# libraries (X11, Wayland, etc.) on Linux remain dynamic — that is expected.
if(VECTIS_STATIC_LINK)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    set(BUILD_SHARED_LIBS OFF)
    if(NOT MSVC)
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

find_package(SDL2         REQUIRED)
find_package(OpenGL       REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(tomlplusplus REQUIRED)
find_package(CURL         REQUIRED)
find_package(fmt          REQUIRED)
find_package(spdlog       REQUIRED)

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

# --- Dear ImGui (docking branch, pinned) -------------------------------------
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    # v1.92.7-docking (resolved 2026-04-09) — pinned SHA so fresh clones
    # always build against the same ImGui snapshot.
    GIT_TAG        7d2273672596f2a2fc4d5f20c52ec1d3cb6cd2c3
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
# SDL2 target name differs between vcpkg (SDL2::SDL2) and system FindSDL2
# (SDL2::SDL2 on newer CMake, otherwise just ${SDL2_LIBRARIES}). Modern
# Ubuntu ships the CMake config, so SDL2::SDL2 works in both cases.
target_link_libraries(imgui_lib PUBLIC SDL2::SDL2 OpenGL::GL)

# ImGui sources don't meet Vectis's strict lint standard. Silence warnings
# and exclude from clang-tidy on the third-party target only — our own
# sources still get the full treatment.
if(MSVC)
    target_compile_options(imgui_lib PRIVATE /w)
else()
    target_compile_options(imgui_lib PRIVATE -w)
endif()
set_target_properties(imgui_lib PROPERTIES CXX_CLANG_TIDY "")

# --- Vectis core library -----------------------------------------------------
# Everything Vectis-written lives in a static library so the executable and
# the test runner can link against the same objects without double-compiling.
add_library(vectis_core STATIC
    src/core/app.cpp
    src/core/config_manager.cpp
    src/core/context_bus.cpp
    src/core/log.cpp
    src/core/service_registry.cpp
    src/platform/file_io.cpp
    src/platform/process.cpp
    src/ui/theme.cpp
    src/ui/widgets.cpp
)

target_include_directories(vectis_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(vectis_core PUBLIC
    imgui_lib
    SDL2::SDL2
    OpenGL::GL
    ${VECTIS_SQLITE3_TARGET}
    nlohmann_json::nlohmann_json
    tomlplusplus::tomlplusplus
    CURL::libcurl
    fmt::fmt
    spdlog::spdlog
    tl::expected
)

# --- Vectis executable -------------------------------------------------------
add_executable(vectis
    src/main.cpp
)

# SDL2::SDL2main is only provided by vcpkg on Windows. On Linux/macOS it
# doesn't exist, and system SDL2 handles `main` via its own header macro.
if(TARGET SDL2::SDL2main)
    target_link_libraries(vectis PRIVATE vectis_core SDL2::SDL2main)
else()
    target_link_libraries(vectis PRIVATE vectis_core)
endif()

# --- Tests -------------------------------------------------------------------
if(VECTIS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
