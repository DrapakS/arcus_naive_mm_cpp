cmake_minimum_required(VERSION 3.20)

# The compiler, when neither CXX nor -DCMAKE_CXX_COMPILER names one: the newest GCC 13 or later
# found, by its version. Ubuntu's g++-13 package installs g++-13 only (no c++, no g++); a plain g++
# may be too old (Ubuntu 22.04: 11) or the one the system's Boost was built with, newer than g++-13
# (Boost 1.90 built by GCC 15 does not link with GCC 13's libstdc++: __cxa_call_terminate).
if(NOT CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
  set(NMM_BEST_VERSION 0)
  foreach(name g++ c++ g++-16 g++-15 g++-14 g++-13)
    unset(NMM_CANDIDATE CACHE)
    find_program(NMM_CANDIDATE ${name})
    if(NMM_CANDIDATE)
      execute_process(COMMAND ${NMM_CANDIDATE} -dumpfullversion OUTPUT_VARIABLE version
                      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE failed)
      if(NOT failed AND version VERSION_GREATER_EQUAL 13 AND version VERSION_GREATER NMM_BEST_VERSION)
        set(NMM_BEST_VERSION ${version})
        set(CMAKE_CXX_COMPILER ${NMM_CANDIDATE})
      endif()
    endif()
  endforeach()
  unset(NMM_CANDIDATE CACHE)
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

# A compile takes up to ~2 GB here (net.cpp: Beast over OpenSSL; agent.cpp ~700 MB, the tests' fake
# servers ~1 GB): with Ninja, no more compiles at once than the memory free at configure time holds,
# so a small host builds slower rather than have the compiler killed ("Killed signal terminated
# program cc1plus"). NMM_COMPILE_JOBS sets the number by hand. Under 4 GB free GCC also collects its
# garbage more often: net.cpp then peaks at ~1.2 GB, for some 10% more time.
cmake_host_system_information(RESULT NMM_FREE_MB QUERY AVAILABLE_PHYSICAL_MEMORY)
math(EXPR NMM_JOBS_BY_MEMORY "${NMM_FREE_MB} / 2000")
if(NMM_JOBS_BY_MEMORY LESS 1)
  set(NMM_JOBS_BY_MEMORY 1)
endif()
set(NMM_LEAN_GCC OFF)
if(NMM_FREE_MB LESS 4000 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(NMM_LEAN_GCC ON)
endif()
if(NMM_COMPILE_JOBS)
  set(NMM_JOBS ${NMM_COMPILE_JOBS})
else()
  set(NMM_JOBS ${NMM_JOBS_BY_MEMORY})
endif()
set_property(GLOBAL APPEND PROPERTY JOB_POOLS nmm_compile=${NMM_JOBS})
set(CMAKE_JOB_POOL_COMPILE nmm_compile)
message(STATUS "Compiles at once: ${NMM_JOBS} (${NMM_FREE_MB} MB free; -DNMM_COMPILE_JOBS=N to set)")
if(NMM_FREE_MB LESS 1500)
  message(WARNING "${NMM_FREE_MB} MB free: one compile takes up to ~1.2 GB (~2 GB with more memory free). Add swap "
                  "(README, \"Сборка\"), and build the bot alone: cmake --build build --target naive_mm")
endif()

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)  # Boost's own BoostConfig.cmake, not CMake's FindBoost
endif()
find_package(Boost 1.81 REQUIRED COMPONENTS json)

# The system's Boost is built by the system's GCC: an older compiler compiles everything and then
# fails at the link (Boost 1.90 built by GCC 15, linked by g++-13: __cxa_call_terminate). Seen here.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES Boost::json)
check_cxx_source_compiles("#include <boost/json.hpp>
int main() { return boost::json::parse(\"1\").as_int64() == 1 ? 0 : 1; }" NMM_BOOST_LINKS)
unset(CMAKE_REQUIRED_LIBRARIES)
if(NOT NMM_BOOST_LINKS)
  message(FATAL_ERROR "Boost ${Boost_VERSION} does not link with ${CMAKE_CXX_COMPILER} (GCC ${CMAKE_CXX_COMPILER_VERSION}): "
                      "it was built by a newer GCC, the system's own. Install it (sudo apt install g++), "
                      "then rm -rf build and configure again.")
endif()

file(GLOB_RECURSE NMM_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
list(REMOVE_ITEM NMM_SOURCES ${CMAKE_SOURCE_DIR}/src/main.cpp)

add_library(nmm STATIC ${NMM_SOURCES})
target_include_directories(nmm PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(nmm SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/third_party)
target_compile_definitions(nmm PUBLIC BOOST_ASIO_NO_DEPRECATED BOOST_BEAST_USE_STD_STRING_VIEW)
target_compile_options(nmm PUBLIC -Wall -Wextra -Wno-unused-parameter $<$<CXX_COMPILER_ID:GNU>:-Wno-psabi>)  # psabi: an ABI note on ARM, no warning
if(NMM_LEAN_GCC)
  target_compile_options(nmm PUBLIC --param=ggc-min-expand=10 --param=ggc-min-heapsize=32768)
endif()
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
