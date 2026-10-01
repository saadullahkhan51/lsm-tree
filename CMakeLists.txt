cmake_minimum_required(VERSION 3.24)
project(lsm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(LSM_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. address,undefined or thread")
if(LSM_SANITIZE)
  add_compile_options(-fsanitize=${LSM_SANITIZE} -fno-sanitize-recover=all -fno-omit-frame-pointer -O1)
  add_link_options(-fsanitize=${LSM_SANITIZE})
endif()

find_package(Threads REQUIRED)

# Globbed so that adding a source file needs no edit here; CONFIGURE_DEPENDS
# makes the build re-glob.
file(GLOB_RECURSE LSM_SOURCES CONFIGURE_DEPENDS src/*.cc)
add_library(lsm ${LSM_SOURCES})
target_include_directories(lsm PUBLIC include src)
target_compile_options(lsm PRIVATE -Wall -Wextra -Werror)
target_link_libraries(lsm PUBLIC Threads::Threads)

# The hardware CRC32C path needs the instruction set enabled for its one file.
# Apple Silicon has it on by default; a runtime check guards the others.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set_source_files_properties(src/util/crc32c.cc PROPERTIES COMPILE_OPTIONS "-msse4.2")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64" AND NOT APPLE)
  set_source_files_properties(src/util/crc32c.cc PROPERTIES COMPILE_OPTIONS "-march=armv8-a+crc")
endif()

include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
include(GoogleTest)

file(GLOB LSM_TEST_SOURCES CONFIGURE_DEPENDS test/*_test.cc)
add_executable(lsm_tests ${LSM_TEST_SOURCES})
target_compile_options(lsm_tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(lsm_tests PRIVATE lsm GTest::gtest_main)
gtest_discover_tests(lsm_tests DISCOVERY_TIMEOUT 60)

# A process that writes until it is killed; process_crash_test runs it.
add_executable(lsm_kill_child test/kill_child_main.cc)
target_compile_options(lsm_kill_child PRIVATE -Wall -Wextra -Werror)
target_link_libraries(lsm_kill_child PRIVATE lsm)
add_dependencies(lsm_tests lsm_kill_child)
target_compile_definitions(lsm_tests PRIVATE LSM_KILL_CHILD_PATH="$<TARGET_FILE:lsm_kill_child>")
