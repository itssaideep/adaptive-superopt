cmake_minimum_required(VERSION 3.20)

project(adaptive_superopt
  VERSION 0.1.0
  DESCRIPTION "Budget-aware adaptive superoptimization research prototype"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(adaptive_superopt
  src/backend.cpp
  src/expression.cpp
  src/generator.cpp
  src/selector.cpp
  src/superoptimizer.cpp
  src/verifier.cpp
)

target_include_directories(adaptive_superopt
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(MSVC)
  target_compile_options(adaptive_superopt PRIVATE /W4 /permissive-)
else()
  target_compile_options(adaptive_superopt PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(adaptive_superopt_demo
  tools/demo.cpp
)
target_link_libraries(adaptive_superopt_demo PRIVATE adaptive_superopt)

add_executable(adaptive_superopt_budget_experiment
  tools/budget_experiment.cpp
)
target_link_libraries(adaptive_superopt_budget_experiment PRIVATE adaptive_superopt)

enable_testing()

add_executable(adaptive_superopt_tests
  tests/selector_tests.cpp
)
target_link_libraries(adaptive_superopt_tests PRIVATE adaptive_superopt)

add_test(NAME adaptive_superopt_tests COMMAND adaptive_superopt_tests)
