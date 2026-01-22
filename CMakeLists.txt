cmake_minimum_required(VERSION 3.16)
project(timer_scheduler LANGUAGES CXX)

# ---- C++ standard ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- library (header-only) ----
# 你当前 timer_scheduler.h 是 header-only，实现全在头文件里
add_library(timer_scheduler INTERFACE)
target_include_directories(timer_scheduler INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# ---- threads ----
find_package(Threads REQUIRED)
target_link_libraries(timer_scheduler INTERFACE Threads::Threads)

# ---- warnings ----
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(timer_scheduler INTERFACE -Wall -Wextra -Wpedantic)
endif()

# ---- tests ----
add_executable(timer_tests test.cpp)
target_link_libraries(timer_tests PRIVATE timer_scheduler)

# 如果你想在 CI 或本地直接跑：ctest
enable_testing()
add_test(NAME timer_tests COMMAND timer_tests)
