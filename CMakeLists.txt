cmake_minimum_required(VERSION 3.16)
project(latency_pipeline LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# A benchmark built without optimizations measures nothing useful: default to Release.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_executable(latency_pipeline src/main.cpp)
add_executable(unit_tests tests/unit_tests.cpp)
add_executable(queue_throughput bench/queue_throughput.cpp)

foreach(target latency_pipeline unit_tests queue_throughput)
  target_include_directories(${target} PRIVATE include)
  target_link_libraries(${target} PRIVATE Threads::Threads)
  if(MSVC)
    # C4324 = "structure was padded due to alignment specifier": that padding is exactly what alignas(64) asks for.
    target_compile_options(${target} PRIVATE /W4 /wd4324)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endforeach()

enable_testing()
add_test(NAME unit_tests COMMAND unit_tests)
