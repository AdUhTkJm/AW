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
option(AW_USE_SYSTEM_ORTOOLS "Use OR-Tools from the system/CMAKE_PREFIX_PATH instead of the vendored copy" OFF)

if(MSVC)
   add_compile_options(/W4 /Zi /utf-8)
else()
   add_compile_options(-Wall -g)
endif()

# Most of our C++ files do not enable exceptions or RTTI.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
   set(AW_STRICT_FLAGS -fno-exceptions -fno-rtti)
else()
   set(AW_STRICT_FLAGS "")
endif()

# Configure OR-Tools.
set(AW_ORTOOLS_ROOT "" CACHE PATH "Extracted OR-Tools release (directory containing lib/cmake/ortools)")
set(AW_ORTOOLS_RESOLVED "")

if(AW_ORTOOLS_ROOT)
   set(AW_ORTOOLS_RESOLVED "${AW_ORTOOLS_ROOT}")
   list(PREPEND CMAKE_PREFIX_PATH "${AW_ORTOOLS_ROOT}")
elseif(NOT AW_USE_SYSTEM_ORTOOLS)
   if(WIN32)
      set(AW_ORTOOLS_PLATFORM windows-x86_64)
   elseif(APPLE)
      set(AW_ORTOOLS_PLATFORM macos-x86_64)
   else()
      set(AW_ORTOOLS_PLATFORM linux-x86_64)
   endif()
   set(AW_VENDORED_ORTOOLS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ortools/${AW_ORTOOLS_PLATFORM}")
   if(EXISTS "${AW_VENDORED_ORTOOLS}/lib/cmake/ortools/ortoolsConfig.cmake")
      set(AW_ORTOOLS_RESOLVED "${AW_VENDORED_ORTOOLS}")
      list(PREPEND CMAKE_PREFIX_PATH "${AW_VENDORED_ORTOOLS}")
      message(STATUS "Using vendored OR-Tools at ${AW_VENDORED_ORTOOLS}")
   endif()
endif()

find_package(ortools CONFIG REQUIRED)

# Find header-only libraries.
find_path(AW_JSON_INCLUDE_DIR nlohmann/json.hpp
   HINTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann")
if(NOT AW_JSON_INCLUDE_DIR)
   message(FATAL_ERROR "nlohmann/json.hpp not found.")
endif()
message(STATUS "Using nlohmann/json from ${AW_JSON_INCLUDE_DIR}")

find_path(AW_DENSE_MAP_DIR ankerl/unordered_dense.h
   HINTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ankerl")
if(NOT AW_DENSE_MAP_DIR)
   message(FATAL_ERROR "ankerl/unordered_dense.h not found.")
endif()
message(STATUS "Using ankerl/unordered_dense from ${AW_DENSE_MAP_DIR}")

add_library(aw_crafting STATIC
   src/plan/CraftingGraph.cpp
   src/plan/CpSatSolver.cpp
   src/plan/Fireability.cpp
   src/plan/Greedy.cpp
   src/plan/Options.cpp
   src/plan/OptionsJson.cpp
   src/plan/PackPrune.cpp
   src/plan/Plan.cpp
   src/plan/Prune.cpp
   src/plan/Protocol.cpp
   src/plan/SatellitePrune.cpp
   src/plan/SubstitutionPrune.cpp
   src/plan/TagPrune.cpp
)
target_include_directories(aw_crafting PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(aw_crafting PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(aw_crafting PRIVATE "${AW_DENSE_MAP_DIR}") # For substitution prune.
target_include_directories(aw_crafting PRIVATE "${AW_JSON_INCLUDE_DIR}") # For option loader.
target_compile_features(aw_crafting PUBLIC cxx_std_20)
target_link_libraries(aw_crafting PUBLIC ortools::ortools)

# Profile pruning on development.
if(LINUX)
   target_compile_definitions(aw_crafting PUBLIC AW_PROFILE_PRUNING=1)
endif()

if(AW_STRICT_FLAGS)
   set_source_files_properties(
      src/plan/CraftingGraph.cpp
      src/plan/Fireability.cpp
      src/plan/Greedy.cpp
      src/plan/Options.cpp
      src/plan/PackPrune.cpp
      src/plan/Plan.cpp
      src/plan/Prune.cpp
      src/plan/Protocol.cpp
      src/plan/SubstitutionPrune.cpp
      src/plan/TagPrune.cpp
      PROPERTIES COMPILE_OPTIONS "${AW_STRICT_FLAGS}")
endif()

# The item search index. It is deliberately independent of OR-Tools and of
# aw_crafting, so it builds and tests on its own; the JNI shim
# (src/search/SearchJni.cpp) needs the JVM headers and is therefore compiled
# straight into aw_jni below.
#
# The pinyin lookup table comes from third_party/pinyin.txt (see
# scripts/fetch-pinyin.sh). It is generated at configure/build time rather than
# committed: the header is ~1.2 MB of derivable tables, and the input is pinned.
# The output lands in the build tree, so the source tree stays clean and the
# private include keeps it out of the public include/ directory.
set(AW_PINYIN_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pinyin.txt")
set(AW_PINYIN_HEADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/pinyin")
set(AW_PINYIN_HEADER "${AW_PINYIN_HEADER_DIR}/Pinyin.h")
set(AW_PINYIN_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen-pinyin.py")

if(NOT EXISTS "${AW_PINYIN_SOURCE}")
   message(FATAL_ERROR
      "third_party/pinyin.txt not found. Run scripts/fetch-pinyin.sh to download "
      "the pinned pinyin-data release.")
endif()

find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
   message(FATAL_ERROR "Python 3 is required to generate Pinyin.h (scripts/gen-pinyin.py).")
endif()

add_custom_command(
   OUTPUT "${AW_PINYIN_HEADER}"
   COMMAND ${CMAKE_COMMAND} -E make_directory "${AW_PINYIN_HEADER_DIR}"
   COMMAND "${Python3_EXECUTABLE}" "${AW_PINYIN_SCRIPT}" "${AW_PINYIN_SOURCE}" "${AW_PINYIN_HEADER}"
   DEPENDS "${AW_PINYIN_SCRIPT}" "${AW_PINYIN_SOURCE}"
   COMMENT "Generating Pinyin.h from third_party/pinyin.txt"
   VERBATIM)

add_library(aw_search STATIC src/search/TextIndex.cpp src/search/SearchIndex.cpp "${AW_PINYIN_HEADER}")
target_include_directories(aw_search PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(aw_search PRIVATE "${AW_PINYIN_HEADER_DIR}")
target_compile_features(aw_search PUBLIC cxx_std_20)
if(AW_STRICT_FLAGS)
   set_source_files_properties(src/search/TextIndex.cpp src/search/SearchIndex.cpp
      PROPERTIES COMPILE_OPTIONS "${AW_STRICT_FLAGS}")
endif()

if(AW_BUILD_TOOLS)
   add_executable(awr_inspect tools/Inspect.cpp)
   target_link_libraries(awr_inspect PRIVATE aw_crafting)
   target_compile_options(awr_inspect PRIVATE ${AW_STRICT_FLAGS})

   # Cross-engine benchmark driver. Registers the graph once per (dataset, mode)
   # and sweeps every ablation stage and query against that registration, then
   # writes JSONL. See bench/README.md.
   #
   # The derived missing set accumulates balances in aw::int128, which is a
   # native type on GCC/Clang and a small wrapper on MSVC, so unlike when the
   # sources spelled __int128 directly the tool builds on both.
   add_executable(aw_bench tools/Bench.cpp)
   target_link_libraries(aw_bench PRIVATE aw_crafting)
   target_compile_options(aw_bench PRIVATE ${AW_STRICT_FLAGS})

   # Development-only CPU profiling (gperftools). It is linked in whenever the
   # library is installed, but only into awr_inspect, and it only does anything
   # when the tool is run with --profile. aw_crafting, aw_jni and the Windows
   # build never see it: the JNI path has no profiling code and Windows is
   # excluded here. Without gperftools the tool still builds; --profile then
   # reports that profiling is unavailable.
   if(NOT WIN32)
      find_path(AW_PROFILER_INCLUDE_DIR NAMES gperftools/profiler.h)
      find_library(AW_PROFILER_LIBRARY NAMES profiler)
      if(AW_PROFILER_INCLUDE_DIR AND AW_PROFILER_LIBRARY)
         target_compile_definitions(awr_inspect PRIVATE AW_HAVE_PROFILER=1)
         target_include_directories(awr_inspect PRIVATE "${AW_PROFILER_INCLUDE_DIR}")
         target_link_libraries(awr_inspect PRIVATE "${AW_PROFILER_LIBRARY}")
         message(STATUS "awr_inspect: CPU profiling enabled (${AW_PROFILER_LIBRARY})")
      else()
         message(STATUS "awr_inspect: gperftools not found; --profile will report an "
                        "error (install libgoogle-perftools-dev to enable it)")
      endif()
   endif()
endif()

if(AW_BUILD_TESTS)
   enable_testing()
   add_executable(aw_tests tests/Test.cpp)
   target_link_libraries(aw_tests PRIVATE aw_crafting)
   target_compile_options(aw_tests PRIVATE ${AW_STRICT_FLAGS})
   add_test(NAME crafting_graph COMMAND aw_tests)

   # Container conformance suite for aw::PodVector. Header-only, so it needs no
   # link to aw_crafting, but it is built with the same strict flags.
   add_executable(aw_podvector_tests tests/TestPodVector.cpp)
   target_include_directories(aw_podvector_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
   target_compile_features(aw_podvector_tests PRIVATE cxx_std_20)
   target_compile_options(aw_podvector_tests PRIVATE ${AW_STRICT_FLAGS})
   add_test(NAME pod_vector COMMAND aw_podvector_tests)

   # The search corpus loader: transfer layout, ASCII folding and rejection of
   # malformed offsets. Links only aw_search, so it builds without OR-Tools.
   add_executable(aw_search_tests tests/TestSearch.cpp)
   target_link_libraries(aw_search_tests PRIVATE aw_search)
   target_compile_options(aw_search_tests PRIVATE ${AW_STRICT_FLAGS})
   target_compile_definitions(aw_search_tests PRIVATE
      AW_SEARCH_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
   add_test(NAME search_index COMMAND aw_search_tests)

   # The pinyin table generator, checked against a handful of known readings
   # (including 重 and the two diaeresis spellings of 女 and 略).
   add_test(NAME pinyin_generator
      COMMAND "${Python3_EXECUTABLE}" "${AW_PINYIN_SCRIPT}" --self-test "${AW_PINYIN_SOURCE}")
endif()

if(AW_BUILD_JNI)
   # JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 if CMake cannot guess it.
   find_package(JNI)
   if(JNI_FOUND)
      add_library(aw_jni SHARED src/JNIBridge.cpp src/search/SearchJni.cpp)
      target_link_libraries(aw_jni PRIVATE aw_crafting aw_search)
      target_include_directories(aw_jni PRIVATE ${JNI_INCLUDE_DIRS})

      # A Minecraft mod ships one file. The Windows OR-Tools release is built
      # as a set of DLLs (ortools.dll plus Abseil, protobuf, re2, zlib, ...),
      # so stage them next to aw_jni.dll; the mod's native loader must expose
      # that directory (or the jar's extracted natives directory) on the
      # library search path.
      if(WIN32 AND AW_ORTOOLS_RESOLVED)
         file(GLOB AW_ORTOOLS_DLLS "${AW_ORTOOLS_RESOLVED}/bin/*.dll")
         if(AW_ORTOOLS_DLLS)
            add_custom_command(TARGET aw_jni POST_BUILD
               COMMAND ${CMAKE_COMMAND} -E copy_if_different
                       ${AW_ORTOOLS_DLLS} "$<TARGET_FILE_DIR:aw_jni>"
               COMMENT "Staging OR-Tools runtime DLLs next to aw_jni.dll")
         endif()
      endif()
   else()
      message(WARNING "JNI not found; skipping aw_jni (set JAVA_HOME and reconfigure)")
   endif()
endif()
