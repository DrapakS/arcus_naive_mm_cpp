cmake_minimum_required(VERSION 3.20)
project(naive_mm_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(NMM_TESTS "Build the unit tests (GoogleTest)" ON)

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
