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

# ---- Sanitizers (optional) -------------------------------------------------
# -DSANITIZE=address | thread | address,undefined. Must be GLOBAL: a
# sanitizer runtime is linked per binary, so the test executables need the
# flags exactly as much as the library does. -fno-omit-frame-pointer keeps
# sanitizer stack traces readable.
set(SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. address,undefined")
if(SANITIZE)
  add_compile_options(-fsanitize=${SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${SANITIZE})
endif()

# ---- Fuzzing (libFuzzer, optional) ----------------------------------------
# Requires clang (coverage instrumentation). The LIBRARY is instrumented
# with -fsanitize=fuzzer-no-link so the fuzzer's feedback loop can see into
# it; the fuzz executables add the fuzzer runtime in tests/fuzz/. Scope
# matters: this must be a per-target flag — a global add_compile_options
# would instrument gtest and every test binary too, leaving them with
# coverage calls but no runtime to resolve them (undefined refs at link).
# See tests/fuzz/ for the harness contracts.
option(BEDROCKKV_FUZZ "Build libFuzzer targets (requires clang)" OFF)

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
  src/ring.cpp
  src/vlog.cpp
  src/db.cpp
  src/resp.cpp
  src/server.cpp
)
target_include_directories(bedrockkv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(BEDROCKKV_FUZZ)
  target_compile_options(bedrockkv PRIVATE -fsanitize=fuzzer-no-link)
  # Consumers of the instrumented static library (tests, the server
  # binary) must link the sanitizer-coverage runtime too — INTERFACE so
  # every target linking bedrockkv inherits it without repeating flags.
  target_link_options(bedrockkv INTERFACE -fsanitize=fuzzer-no-link)
endif()

# ---- Server binary ----------------------------------------------------------
# The demo endpoint: redis-cli / redis-benchmark connect here.
add_executable(bedrockkv-server tools/server_main.cpp)
target_link_libraries(bedrockkv-server PRIVATE bedrockkv)

# ---- Benchmarks ----------------------------------------------------------
# Not part of ctest: long-running by design. Run manually, e.g.
#   ./build/ycsb_bench --workload all
add_executable(ycsb_bench bench/ycsb_bench.cpp)
target_link_libraries(ycsb_bench PRIVATE bedrockkv)

# ---- Tests ---------------------------------------------------------------
enable_testing()
add_subdirectory(tests)

if(BEDROCKKV_FUZZ)
  add_subdirectory(tests/fuzz)
endif()
