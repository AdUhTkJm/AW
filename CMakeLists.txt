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
   add_compile_options(/W4 /Zi)
else()
   add_compile_options(-Wall -g)
endif()

# The hand-written code is exception- and RTTI-free. OR-Tools, Abseil and
# protobuf need both, so the flags are applied per source file rather than
# globally; src/CpSatSolver.cpp (the CP-SAT model) and src/SatellitePrune.cpp
# (the small GLOP LP behind satellite elimination) are the only files that
# include OR-Tools.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
   set(AW_STRICT_FLAGS -fno-exceptions -fno-rtti)
else()
   set(AW_STRICT_FLAGS "")
endif()

# OR-Tools is a hard dependency: the LP fallback it replaced is gone.
#
# Either install OR-Tools somewhere CMake can see (set CMAKE_PREFIX_PATH or
# AW_ORTOOLS_ROOT), or extract a release archive into third_party/ortools and
# let scripts/fetch-ortools.sh lay it out. The archive ships a CMake package,
# which is the only supported way to consume the prebuilt libraries: it is what
# injects OR_PROTO_DLL=, without which the generated protobuf headers do not
# compile.
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

add_library(aw_crafting STATIC src/CraftingGraph.cpp src/CpSatSolver.cpp src/Options.cpp src/PackPrune.cpp src/Plan.cpp src/Prune.cpp src/SatellitePrune.cpp)
target_include_directories(aw_crafting PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(aw_crafting PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(aw_crafting PUBLIC cxx_std_20)
target_link_libraries(aw_crafting PUBLIC ortools::ortools)

if(AW_STRICT_FLAGS)
   set_source_files_properties(src/CraftingGraph.cpp src/Options.cpp src/PackPrune.cpp src/Plan.cpp src/Prune.cpp
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
   # GCC/Clang only: the derived missing set accumulates balances in __int128,
   # which MSVC does not have. The benchmark runs on Linux/WSL, and the Windows
   # path (scripts/build-windows.sh) only needs the DLL, so the tool is skipped
   # there rather than carrying a second arithmetic path for it.
   if(NOT MSVC)
      add_executable(aw_bench tools/Bench.cpp)
      target_link_libraries(aw_bench PRIVATE aw_crafting)
      target_compile_options(aw_bench PRIVATE ${AW_STRICT_FLAGS})
   endif()

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
endif()

if(AW_BUILD_JNI)
   # JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 if CMake cannot guess it.
   find_package(JNI)
   if(JNI_FOUND)
      add_library(aw_jni SHARED src/JNIBridge.cpp)
      target_link_libraries(aw_jni PRIVATE aw_crafting)
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
