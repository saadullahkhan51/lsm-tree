cmake_minimum_required(VERSION 3.24)
project(lsm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(LSM_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. address,undefined or thread")
option(LSM_BUILD_TESTS "Build the test suite" ON)
option(LSM_BUILD_FUZZERS "Build the libFuzzer targets in fuzz/ (needs clang)" OFF)
option(LSM_BUILD_BENCH "Build db_bench" ON)
option(LSM_WERROR "Treat warnings as errors" ON)

if(LSM_SANITIZE)
  add_compile_options(-fsanitize=${LSM_SANITIZE} -fno-sanitize-recover=all -fno-omit-frame-pointer -O1)
  add_link_options(-fsanitize=${LSM_SANITIZE})
endif()
if(LSM_BUILD_FUZZERS)
  # Coverage instrumentation for the library, so the fuzzer can see into it.
  add_compile_options(-fsanitize=fuzzer-no-link)
endif()

set(LSM_WARNINGS -Wall -Wextra)
if(LSM_WERROR)
  list(APPEND LSM_WARNINGS -Werror)
endif()

find_package(Threads REQUIRED)

# Globbed so that adding a source file needs no edit here; CONFIGURE_DEPENDS
# makes the build re-glob.
file(GLOB_RECURSE LSM_SOURCES CONFIGURE_DEPENDS src/*.cc)
add_library(lsm ${LSM_SOURCES})
target_include_directories(lsm PUBLIC include src)
target_compile_options(lsm PRIVATE ${LSM_WARNINGS})
target_link_libraries(lsm PUBLIC Threads::Threads)

# The hardware CRC32C path needs the instruction set enabled for its one file.
# Apple Silicon has it on by default; a runtime check guards the others.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set_source_files_properties(src/util/crc32c.cc PROPERTIES COMPILE_OPTIONS "-msse4.2")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64" AND NOT APPLE)
  set_source_files_properties(src/util/crc32c.cc PROPERTIES COMPILE_OPTIONS "-march=armv8-a+crc")
endif()

if(LSM_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  include(GoogleTest)

  file(GLOB LSM_TEST_SOURCES CONFIGURE_DEPENDS test/*_test.cc)
  add_executable(lsm_tests ${LSM_TEST_SOURCES})
  target_compile_options(lsm_tests PRIVATE ${LSM_WARNINGS})
  target_link_libraries(lsm_tests PRIVATE lsm GTest::gtest_main)
  gtest_discover_tests(lsm_tests DISCOVERY_TIMEOUT 60)

  # A process that writes until it is killed; process_crash_test runs it.
  add_executable(lsm_kill_child test/kill_child_main.cc)
  target_compile_options(lsm_kill_child PRIVATE ${LSM_WARNINGS})
  target_link_libraries(lsm_kill_child PRIVATE lsm)
  add_dependencies(lsm_tests lsm_kill_child)
  target_compile_definitions(lsm_tests PRIVATE LSM_KILL_CHILD_PATH="$<TARGET_FILE:lsm_kill_child>")
endif()

if(LSM_BUILD_FUZZERS)
  foreach(target block log_reader version_edit table write_batch)
    add_executable(${target}_fuzzer fuzz/${target}_fuzzer.cc)
    target_compile_options(${target}_fuzzer PRIVATE ${LSM_WARNINGS})
    target_link_libraries(${target}_fuzzer PRIVATE lsm)
    target_link_options(${target}_fuzzer PRIVATE -fsanitize=fuzzer)
  endforeach()
endif()

if(LSM_BUILD_BENCH)
  add_executable(db_bench
    bench/db_bench.cc
    bench/engine_lsm.cc
    bench/engine_leveldb.cc
    bench/engine_rocksdb.cc)
  target_compile_options(db_bench PRIVATE ${LSM_WARNINGS})
  target_link_libraries(db_bench PRIVATE lsm)

  # The comparison engines are optional: db_bench is built with whichever
  # are installed.
  find_path(LEVELDB_INCLUDE_DIR leveldb/db.h)
  find_library(LEVELDB_LIBRARY leveldb)
  if(LEVELDB_INCLUDE_DIR AND LEVELDB_LIBRARY)
    message(STATUS "db_bench: LevelDB found at ${LEVELDB_LIBRARY}")
    target_compile_definitions(db_bench PRIVATE LSM_BENCH_HAVE_LEVELDB)
    target_include_directories(db_bench SYSTEM PRIVATE ${LEVELDB_INCLUDE_DIR})
    target_link_libraries(db_bench PRIVATE ${LEVELDB_LIBRARY})
  endif()
  find_path(ROCKSDB_INCLUDE_DIR rocksdb/db.h)
  find_library(ROCKSDB_LIBRARY rocksdb)
  if(ROCKSDB_INCLUDE_DIR AND ROCKSDB_LIBRARY)
    message(STATUS "db_bench: RocksDB found at ${ROCKSDB_LIBRARY}")
    target_compile_definitions(db_bench PRIVATE LSM_BENCH_HAVE_ROCKSDB)
    target_include_directories(db_bench SYSTEM PRIVATE ${ROCKSDB_INCLUDE_DIR})
    target_link_libraries(db_bench PRIVATE ${ROCKSDB_LIBRARY})
  endif()

  # Microbenchmarks. Not in sanitizer builds: timings there mean nothing,
  # and Google Benchmark 1.9.4 does not compile with the clang 23 those
  # builds need on macOS.
  if(NOT LSM_SANITIZE)
    include(FetchContent)
    FetchContent_Declare(benchmark
      URL https://github.com/google/benchmark/archive/refs/tags/v1.9.4.tar.gz)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)
    add_executable(micro_bench bench/micro_bench.cc)
    target_compile_options(micro_bench PRIVATE ${LSM_WARNINGS})
    target_link_libraries(micro_bench PRIVATE lsm benchmark::benchmark)
  endif()
endif()
