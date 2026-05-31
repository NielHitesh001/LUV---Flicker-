cmake_minimum_required(VERSION 3.10)
project(LUV_Engine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Strict optimization flags for zero-allocation C++ engine
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -Wall -Wextra -march=native")

# We link pthread for background telemetry thread and thread affinity setup
add_executable(luv_engine main.cpp)
target_link_libraries(luv_engine pthread)
