cmake_minimum_required(VERSION 3.16)

project(bedrockkv
  VERSION 0.1.0
  DESCRIPTION "A LSM-Tree based persistent KV storage engine"
  LANGUAGES CXX
)

# ---- Global C++ settings -----------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # use -std=c++20, not gnu++20

# Default to Debug so sanitizers/asan work out of the box for development.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# Fail the build on any warning; new code must be clean from day one.
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

# ---- Dependencies (FetchContent, per design doc ch.4) -------------------
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
)
# Avoid gtest overriding our compiler/linker settings on some platforms.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ---- Library: bedrockkv --------------------------------------------------
# PUBLIC header dir: consumers (tests, bench, future RESP server) include
# headers as "bedrockkv/status.h", mirroring the installed layout.
add_library(bedrockkv
  src/status.cpp
  src/crc32.cpp
  src/log.cpp
  src/memtable.cpp
  src/block.cpp
  src/sstable.cpp
  src/fs_util.cpp
  src/db.cpp
)
target_include_directories(bedrockkv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---- Benchmarks ----------------------------------------------------------
# Not part of ctest: long-running by design. Run manually, e.g.
#   ./build/ycsb_bench --workload all
add_executable(ycsb_bench bench/ycsb_bench.cpp)
target_link_libraries(ycsb_bench PRIVATE bedrockkv)

# ---- Tests ---------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
