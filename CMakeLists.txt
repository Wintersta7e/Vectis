cmake_minimum_required(VERSION 3.25)
project(Vectis
    VERSION 0.1.0
    DESCRIPTION "Portable Developer Intelligence Tool"
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Strict warnings
if(MSVC)
    add_compile_options(/W4 /WX /permissive-)
else()
    add_compile_options(-Wall -Wextra -Werror -Wpedantic)
endif()

# Static linking for portability
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
set(BUILD_SHARED_LIBS OFF)
if(NOT MSVC)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")
endif()

# --- Dependencies via vcpkg ---
find_package(SDL2 CONFIG REQUIRED)
find_package(OpenGL REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)

# Dear ImGui (added as submodule or fetched)
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG docking
)
FetchContent_MakeAvailable(imgui)

# ImGui library target
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
target_link_libraries(imgui_lib PUBLIC SDL2::SDL2 OpenGL::GL)

# --- Vectis core ---
add_executable(vectis
    src/main.cpp
)

target_include_directories(vectis PRIVATE src)

target_link_libraries(vectis PRIVATE
    imgui_lib
    SDL2::SDL2
    SDL2::SDL2main
    OpenGL::GL
    unofficial::sqlite3::sqlite3
    nlohmann_json::nlohmann_json
    tomlplusplus::tomlplusplus
    CURL::libcurl
)

# As modes and services grow, they'll be added as subdirectories:
# add_subdirectory(src/core)
# add_subdirectory(src/modes/code)
# add_subdirectory(src/services/storage_engine)
# etc.

# --- Tests ---
option(VECTIS_BUILD_TESTS "Build tests" ON)
if(VECTIS_BUILD_TESTS)
    enable_testing()
    # add_subdirectory(tests)
endif()
