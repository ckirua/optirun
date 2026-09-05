cmake_minimum_required(VERSION 4.0)
project(hybrid_python VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "Milestone 1 supports Linux/POSIX only")
endif()

set(HYBRID_PYTHON_GIL_EXECUTABLE "" CACHE FILEPATH "Regular CPython 3.14 executable")
set(HYBRID_PYTHON_FREE_THREADED_EXECUTABLE "" CACHE FILEPATH "Free-threaded CPython 3.14 executable")
foreach(executable IN ITEMS HYBRID_PYTHON_GIL_EXECUTABLE HYBRID_PYTHON_FREE_THREADED_EXECUTABLE)
  if(NOT ${executable} OR NOT EXISTS "${${executable}}" OR IS_DIRECTORY "${${executable}}")
    message(FATAL_ERROR "${executable} must name an existing Python executable")
  endif()
endforeach()

find_package(Threads REQUIRED)
enable_testing()

add_library(hybrid_python STATIC src/runtime_manager.cpp src/wire_codec.cpp src/pipe_transport.cpp)
target_include_directories(hybrid_python PUBLIC include)
target_compile_features(hybrid_python PUBLIC cxx_std_20)
target_link_libraries(hybrid_python PUBLIC Threads::Threads)
target_compile_options(hybrid_python PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)

add_executable(native_dispatch examples/native_dispatch.cpp)
target_link_libraries(native_dispatch PRIVATE hybrid_python)

target_compile_definitions(hybrid_python PRIVATE HYBRID_WORKER_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/workers/hybrid_python_worker.py")
add_executable(runtime_manager_test tests/runtime_manager_test.cpp)
target_link_libraries(runtime_manager_test PRIVATE hybrid_python)
add_test(NAME runtime_manager_test COMMAND runtime_manager_test
  "${HYBRID_PYTHON_GIL_EXECUTABLE}"
  "${HYBRID_PYTHON_FREE_THREADED_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/handlers.py")
