find_package(GTest QUIET CONFIG)
if(NOT GTest_FOUND)
  include(FetchContent)
  FetchContent_Declare(googletest URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
                       URL_HASH SHA256=1f357c27ca988c3f7c6b4bf68a9395005ac6761f034046e9dde0896e3aba00e4)
  FetchContent_MakeAvailable(googletest)
endif()

add_executable(test_object_factory test_object_factory.cpp second_tu.cpp)
target_link_libraries(test_object_factory PRIVATE GTest::gtest_main object::object)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_object_factory PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(GoogleTest)
gtest_discover_tests(test_object_factory)
