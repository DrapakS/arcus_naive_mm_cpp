cmake_minimum_required(VERSION 3.20)

# The compiler, when neither CXX nor -DCMAKE_CXX_COMPILER names one: a GCC with <format> (13+) by its
# versioned name first, since Ubuntu's g++-13 package installs g++-13 only (no c++, no g++), and a
# plain c++ may be an older GCC (Ubuntu 22.04: 11).
if(NOT CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
  find_program(NMM_CXX NAMES g++-15 g++-14 g++-13 c++ g++ clang++)
  if(NMM_CXX)
    set(CMAKE_CXX_COMPILER ${NMM_CXX})
  endif()
endif()

project(naive_mm_cpp LANGUAGES CXX)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
  message(FATAL_ERROR "GCC ${CMAKE_CXX_COMPILER_VERSION} (${CMAKE_CXX_COMPILER}) has no <format>: GCC 13 or later is needed "
                      "(Ubuntu 24.04: sudo apt install g++-13; then rm -rf build and configure again)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(NMM_TESTS "Build the unit tests (GoogleTest)" ON)

# A compile takes up to ~700 MB here (the bot's Asio/Beast coroutines; the tests' fake servers ~1 GB):
# with Ninja, no more compiles at once than the memory free at configure time holds, so a small host
# builds slower rather than have the compiler killed ("Killed signal terminated program cc1plus").
# NMM_COMPILE_JOBS sets the number by hand.
cmake_host_system_information(RESULT NMM_FREE_MB QUERY AVAILABLE_PHYSICAL_MEMORY)
math(EXPR NMM_JOBS_BY_MEMORY "${NMM_FREE_MB} / 1000")
if(NMM_JOBS_BY_MEMORY LESS 1)
  set(NMM_JOBS_BY_MEMORY 1)
endif()
if(NMM_COMPILE_JOBS)
  set(NMM_JOBS ${NMM_COMPILE_JOBS})
else()
  set(NMM_JOBS ${NMM_JOBS_BY_MEMORY})
endif()
set_property(GLOBAL APPEND PROPERTY JOB_POOLS nmm_compile=${NMM_JOBS})
set(CMAKE_JOB_POOL_COMPILE nmm_compile)
message(STATUS "Compiles at once: ${NMM_JOBS} (${NMM_FREE_MB} MB free; -DNMM_COMPILE_JOBS=N to set)")
if(NMM_FREE_MB LESS 900)
  message(WARNING "${NMM_FREE_MB} MB free: one compile takes up to ~700 MB (~1 GB for the tests). Add swap, "
                  "or build the bot alone: cmake --build build --target naive_mm")
endif()

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Boost 1.81 REQUIRED COMPONENTS json)

file(GLOB_RECURSE NMM_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
list(REMOVE_ITEM NMM_SOURCES ${CMAKE_SOURCE_DIR}/src/main.cpp)

add_library(nmm STATIC ${NMM_SOURCES})
target_include_directories(nmm PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(nmm SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/third_party)
target_compile_definitions(nmm PUBLIC BOOST_ASIO_NO_DEPRECATED BOOST_BEAST_USE_STD_STRING_VIEW)
target_compile_options(nmm PUBLIC -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(nmm PUBLIC Boost::json OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

# The git commit and a fingerprint of the sources, refreshed on every build.
add_custom_target(nmm_version
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUTPUT=${CMAKE_BINARY_DIR}/version.cpp
          -P ${CMAKE_SOURCE_DIR}/cmake/version.cmake
  BYPRODUCTS ${CMAKE_BINARY_DIR}/version.cpp
  COMMENT "Version of the sources")

add_executable(naive_mm ${CMAKE_SOURCE_DIR}/src/main.cpp ${CMAKE_BINARY_DIR}/version.cpp)
add_dependencies(naive_mm nmm_version)
target_link_libraries(naive_mm PRIVATE nmm)

if(NMM_TESTS)
  find_package(GTest)
  if(GTest_FOUND)
    enable_testing()
    file(GLOB NMM_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*.cpp)
    add_executable(nmm_tests ${NMM_TEST_SOURCES} ${CMAKE_BINARY_DIR}/version.cpp)
    add_dependencies(nmm_tests nmm_version)
    target_include_directories(nmm_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    target_link_libraries(nmm_tests PRIVATE nmm GTest::gtest GTest::gtest_main)
    target_compile_definitions(nmm_tests PRIVATE NMM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    include(GoogleTest)
    gtest_discover_tests(nmm_tests DISCOVERY_TIMEOUT 60)
  else()
    message(STATUS "GoogleTest not found: the tests are not built")
  endif()
endif()
