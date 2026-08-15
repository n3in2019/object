cmake_minimum_required(VERSION 3.15)
project(object VERSION 1.0.0 LANGUAGES CXX)

option(OBJECT_BUILD_TESTING "Build the object test suite" ON)

add_library(object INTERFACE)
add_library(object::object ALIAS object)
target_include_directories(
  object INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>")
target_compile_features(object INTERFACE cxx_std_17)

enable_testing()

if(OBJECT_BUILD_TESTING)
  add_subdirectory(tests)
endif()
