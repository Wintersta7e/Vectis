# GTest comes from vcpkg (CONFIG) or from system libgtest-dev (MODULE).
# Dropping CONFIG lets CMake try both, matching the root CMakeLists.
find_package(GTest REQUIRED)

include(GoogleTest)

add_executable(vectis_tests
    core/config_manager_test.cpp
    core/context_bus_test.cpp
    core/task_queue_test.cpp
    modes/code/code_index_test.cpp
    modes/code/dependency_resolver_test.cpp
    modes/code/digest_exporter_test.cpp
    modes/code/fixtures_test.cpp
    modes/code/language_test.cpp
    modes/code/parser_test.cpp
    modes/code/scanner_test.cpp
    platform/file_io_test.cpp
)

# Fixture test needs to find the sample projects in the source tree
# regardless of where the binary runs from. Pass the absolute source
# path as a compile definition.
target_compile_definitions(vectis_tests PRIVATE
    VECTIS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(vectis_tests PRIVATE
    vectis_core
    GTest::gtest
    GTest::gtest_main
)

# Tests inherit the same strict-lint surface via add_compile_options at
# the top of the root CMakeLists.

gtest_discover_tests(vectis_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
        LABELS "unit"
)
