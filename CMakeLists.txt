cmake_minimum_required(VERSION 3.20)

project(strata
    VERSION 0.1.0
    DESCRIPTION "An embedded LSM-tree key-value store"
    LANGUAGES CXX)

option(STRATA_BUILD_TESTS "Build the unit tests" ${PROJECT_IS_TOP_LEVEL})
option(STRATA_BUILD_TOOLS "Build the strata CLI and strata_bench" ${PROJECT_IS_TOP_LEVEL})
option(STRATA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(STRATA_SANITIZE "Build with AddressSanitizer and UBSan (GCC/Clang)" OFF)
option(STRATA_TSAN "Build with ThreadSanitizer (GCC/Clang)" OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

add_library(strata
    src/util/arena.cpp
    src/util/bloom.cpp
    src/util/coding.cpp
    src/util/comparator.cpp
    src/util/crc32c.cpp
    src/util/hash.cpp
    src/util/histogram.cpp
    src/util/lz.cpp
    src/util/status.cpp
    src/util/strutil.cpp
    src/env/env.cpp
    src/env/env_default.cpp
    src/env/mem_env.cpp
    src/table/block.cpp
    src/table/block_builder.cpp
    src/table/format.cpp
    src/table/iterator.cpp
    src/table/merging_iterator.cpp
    src/table/properties.cpp
    src/table/table.cpp
    src/table/table_builder.cpp
    src/table/two_level_iterator.cpp
    src/db/builder.cpp
    src/db/db_impl.cpp
    src/db/db_iter.cpp
    src/db/dbformat.cpp
    src/db/dumpfile.cpp
    src/db/filename.cpp
    src/db/log_reader.cpp
    src/db/log_writer.cpp
    src/db/memtable.cpp
    src/db/table_cache.cpp
    src/db/version_edit.cpp
    src/db/version_set.cpp
    src/db/write_batch.cpp
)
add_library(strata::strata ALIAS strata)

target_include_directories(strata
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(strata PUBLIC cxx_std_20)
set_target_properties(strata PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(strata PUBLIC Threads::Threads)

add_library(strata_build_flags INTERFACE)
if(MSVC)
    target_compile_options(strata_build_flags INTERFACE /W4 /permissive- /utf-8 /EHsc)
    # fopen, strerror and friends are fine here.
    target_compile_definitions(strata_build_flags INTERFACE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    if(STRATA_WARNINGS_AS_ERRORS)
        target_compile_options(strata_build_flags INTERFACE /WX)
    endif()
else()
    target_compile_options(strata_build_flags INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wnon-virtual-dtor -Wold-style-cast)
    if(STRATA_WARNINGS_AS_ERRORS)
        target_compile_options(strata_build_flags INTERFACE -Werror)
    endif()
    if(STRATA_SANITIZE AND STRATA_TSAN)
        message(FATAL_ERROR "STRATA_SANITIZE and STRATA_TSAN cannot be combined")
    endif()
    if(STRATA_SANITIZE)
        target_compile_options(strata_build_flags INTERFACE
            -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
        target_link_options(strata_build_flags INTERFACE -fsanitize=address,undefined)
    endif()
    if(STRATA_TSAN)
        target_compile_options(strata_build_flags INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(strata_build_flags INTERFACE -fsanitize=thread)
    endif()
endif()
target_link_libraries(strata PRIVATE $<BUILD_INTERFACE:strata_build_flags>)

if(STRATA_BUILD_TOOLS)
    # The CLI's command logic is a library so the tests can drive it.
    add_library(strata_cli_lib STATIC tools/cli.cpp)
    target_include_directories(strata_cli_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tools
                                              PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(strata_cli_lib PUBLIC strata PRIVATE strata_build_flags)

    add_executable(strata_cli tools/strata_main.cpp)
    set_target_properties(strata_cli PROPERTIES OUTPUT_NAME strata)
    target_link_libraries(strata_cli PRIVATE strata_cli_lib strata_build_flags)

    add_executable(strata_bench tools/strata_bench.cpp)
    target_include_directories(strata_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(strata_bench PRIVATE strata strata_build_flags)

    # Mutation fuzzer for every on-disk decoder; see tools/strata_fuzz.cpp.
    add_executable(strata_fuzz tools/strata_fuzz.cpp)
    target_include_directories(strata_fuzz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(strata_fuzz PRIVATE strata strata_build_flags)

    # Uses only the public headers, like a program that installed strata.
    add_executable(basic_usage examples/basic_usage.cpp)
    target_link_libraries(basic_usage PRIVATE strata strata_build_flags)
endif()

if(STRATA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS strata EXPORT strataTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(STRATA_BUILD_TOOLS)
    install(TARGETS strata_cli strata_bench RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY include/strata DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT strataTargets NAMESPACE strata:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/strata)
