cmake_minimum_required(VERSION 3.20)
project(luv_flicker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(luv_core INTERFACE)
target_include_directories(luv_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(luv_core INTERFACE $<$<CXX_COMPILER_ID:GNU,Clang>:-O3;-Wall;-Wextra;-Wpedantic>)

find_package(Threads REQUIRED)

add_executable(luv_engine main_engine.cpp)
target_link_libraries(luv_engine PRIVATE luv_core Threads::Threads)
if(CMAKE_DL_LIBS)
  target_link_libraries(luv_engine PRIVATE ${CMAKE_DL_LIBS})
endif()

foreach(test_name IN ITEMS arena feed lob execution ai_telemetry stress)
  add_executable(test_${test_name} test_${test_name}.cpp)
  target_link_libraries(test_${test_name} PRIVATE luv_core Threads::Threads)
  if(CMAKE_DL_LIBS)
    target_link_libraries(test_${test_name} PRIVATE ${CMAKE_DL_LIBS})
  endif()
endforeach()

enable_testing()
foreach(test_name IN ITEMS arena feed lob execution ai_telemetry stress)
  add_test(NAME ${test_name} COMMAND test_${test_name})
endforeach()
