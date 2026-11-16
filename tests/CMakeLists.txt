add_executable(strata_tests
    main.cpp
    support/test_util.cpp
    test_util.cpp
    test_codecs.cpp
    test_env.cpp
    test_table.cpp
    test_memtable.cpp
    test_log.cpp
    test_version.cpp
    test_db.cpp
    test_db_iterator.cpp
    test_db_compaction.cpp
    test_db_recovery.cpp
    test_db_model.cpp
    test_db_concurrency.cpp
)
target_include_directories(strata_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/third_party/doctest)
target_link_libraries(strata_tests PRIVATE strata strata_build_flags)

if(STRATA_BUILD_TOOLS)
    target_sources(strata_tests PRIVATE test_cli.cpp)
    target_link_libraries(strata_tests PRIVATE strata_cli_lib)
    target_compile_definitions(strata_tests PRIVATE STRATA_HAVE_CLI=1)
endif()

# One ctest entry per test suite, so failures are easy to place.
set(strata_suites
    util codecs env table memtable log version db db-iterator db-compaction db-recovery db-model
    db-concurrency)
if(STRATA_BUILD_TOOLS)
    list(APPEND strata_suites cli)
endif()
foreach(suite IN LISTS strata_suites)
    add_test(NAME ${suite} COMMAND strata_tests --test-suite=${suite})
endforeach()

# Short fixed-seed fuzz passes, so ctest catches regressions in the decoders.
# The nightly fuzz workflow runs many more cases with changing seeds.
if(STRATA_BUILD_TOOLS)
    if(STRATA_SANITIZE)
        set(strata_fuzz_cases 400)
    else()
        set(strata_fuzz_cases 2000)
    endif()
    add_test(NAME fuzz-seed-1 COMMAND strata_fuzz --cases ${strata_fuzz_cases} --seed 1)
    add_test(NAME fuzz-seed-2 COMMAND strata_fuzz --cases ${strata_fuzz_cases} --seed 2)
    add_test(NAME example COMMAND basic_usage ${CMAKE_CURRENT_BINARY_DIR}/example-db)
endif()
