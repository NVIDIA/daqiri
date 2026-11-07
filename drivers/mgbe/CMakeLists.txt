cmake_minimum_required(VERSION 3.20)
project(daqiri_mgbe_driver LANGUAGES CXX)

add_library(daqiri_mgbe_driver STATIC src/mgbe_pmd.cpp)
target_compile_features(daqiri_mgbe_driver PUBLIC cxx_std_17)
target_include_directories(daqiri_mgbe_driver PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
set_target_properties(daqiri_mgbe_driver PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(daqiri_mgbe_driver PRIVATE -Wall -Wextra -Wpedantic)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  include(CTest)
  if(BUILD_TESTING)
    add_executable(mgbe_queue_test tests/queue_test.cpp)
    target_link_libraries(mgbe_queue_test PRIVATE daqiri_mgbe_driver)
    target_compile_options(mgbe_queue_test PRIVATE -UNDEBUG)
    add_test(NAME mgbe_queue COMMAND mgbe_queue_test)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME mgbe_module_recovery COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/module_recovery_test.py)
  endif()
  add_executable(mgbe_lifecycle_test tests/lifecycle_test.cpp)
  target_link_libraries(mgbe_lifecycle_test PRIVATE daqiri_mgbe_driver)
endif()
