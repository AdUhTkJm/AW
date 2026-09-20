cmake_minimum_required(VERSION 3.20)
project(applied_wheelchair_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
   set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(AW_BUILD_JNI "Build the JNI bridge shared library" ON)
option(AW_BUILD_TOOLS "Build the awr_inspect command line tool" ON)
option(AW_BUILD_TESTS "Build the unit tests" ON)

add_library(aw_crafting STATIC src/CraftingGraph.cpp)
target_include_directories(aw_crafting PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(aw_crafting PUBLIC cxx_std_20)
# Deliberately applied globally.
add_compile_options(-Wall -fno-exceptions -fno-rtti -g)

if(AW_BUILD_TOOLS)
   add_executable(awr_inspect tools/Inspect.cpp)
   target_link_libraries(awr_inspect PRIVATE aw_crafting)
endif()

if(AW_BUILD_TESTS)
   enable_testing()
   add_executable(aw_tests tests/Test.cpp)
   target_link_libraries(aw_tests PRIVATE aw_crafting)
   add_test(NAME crafting_graph COMMAND aw_tests)
endif()

if(AW_BUILD_JNI)
   # JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 if CMake cannot guess it.
   find_package(JNI)
   if(JNI_FOUND)
      add_library(aw_jni SHARED src/JNIBridge.cpp)
      target_link_libraries(aw_jni PRIVATE aw_crafting)
      target_include_directories(aw_jni PRIVATE ${JNI_INCLUDE_DIRS})
   else()
      message(WARNING "JNI not found; skipping aw_jni (set JAVA_HOME and reconfigure)")
   endif()
endif()
