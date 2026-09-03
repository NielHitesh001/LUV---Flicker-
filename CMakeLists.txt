cmake_minimum_required(VERSION 3.20)
project(luv_flicker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LUV_ENABLE_DYNAMIC_MODEL_LOADING
       "Allow loading trusted Treelite/TL2cgen shared-object predictors" OFF)
option(LUV_REQUIRE_MLOCK
  "Fail Arena initialisation when infrastructure memory cannot be locked" OFF)

add_library(luv_core INTERFACE)
target_include_directories(luv_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(luv_core INTERFACE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-O3;-Wall;-Wextra;-Wpedantic>)
if(LUV_ENABLE_DYNAMIC_MODEL_LOADING)
  target_compile_definitions(luv_core INTERFACE LUV_ENABLE_DYNAMIC_MODEL_LOADING=1)
endif()
if(LUV_REQUIRE_MLOCK)
  target_compile_definitions(luv_core INTERFACE LUV_REQUIRE_MLOCK=1)
endif()

find_package(Threads REQUIRED)

add_executable(luv_engine main_engine.cpp)
target_link_libraries(luv_engine PRIVATE luv_core Threads::Threads)
if(CMAKE_DL_LIBS)
  target_link_libraries(luv_engine PRIVATE ${CMAKE_DL_LIBS})
endif()

foreach(test_name IN ITEMS arena feed lob execution ai_telemetry stress)
  add_executable(test_${test_name} test_${test_name}.cpp)
  target_link_libraries(test_${test_name} PRIVATE luv_core Threads::Threads)
  # Several test fixtures use assert() for their setup checks.  Keep asserts
  # enabled even when the project itself is configured as Release.
  target_compile_options(test_${test_name} PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-UNDEBUG>)
  if(CMAKE_DL_LIBS)
    target_link_libraries(test_${test_name} PRIVATE ${CMAKE_DL_LIBS})
  endif()
endforeach()

enable_testing()
foreach(test_name IN ITEMS arena feed lob execution ai_telemetry stress)
  add_test(NAME ${test_name} COMMAND test_${test_name})
endforeach()
