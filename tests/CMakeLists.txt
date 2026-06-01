# GTest comes from vcpkg (CONFIG) or from system libgtest-dev (MODULE).
# Dropping CONFIG lets CMake try both, matching the root CMakeLists.
find_package(GTest REQUIRED)

include(GoogleTest)

add_executable(vectis_tests
    cli/cli_cache_test.cpp
    cli/mcp_server_test.cpp
    core/config_manager_test.cpp
    core/hash_test.cpp
    core/task_queue_test.cpp
    code/architecture_detector_test.cpp
    code/cmake_inspect_test.cpp
    code/code_index_store_test.cpp
    code/code_index_test.cpp
    code/complexity_analyzer_test.cpp
    code/dependency_graph_test.cpp
    code/dependency_resolver_test.cpp
    code/digest_exporter_test.cpp
    code/fidelity_test.cpp
    code/dotnet_project_test.cpp
    code/explain_test.cpp
    code/fixtures_test.cpp
    code/framework_hints_test.cpp
    code/gitignore_test.cpp
    code/hotspot_detector_test.cpp
    code/language_test.cpp
    code/manifest_deps_test.cpp
    code/manifest_scanner_test.cpp
    code/maven_pom_test.cpp
    code/maven_pom_handler_test.cpp
    code/pagerank_test.cpp
    code/parser_test.cpp
    code/properties_handler_test.cpp
    code/properties_reader_test.cpp
    code/scanner_test.cpp
    code/spring_xml_test.cpp
    code/spring_xml_handler_test.cpp
    code/symbol_test.cpp
    code/xml_reader_test.cpp
    platform/file_io_test.cpp
    platform/file_watcher_test.cpp
    services/index_engine_test.cpp
    services/storage_engine_test.cpp
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
