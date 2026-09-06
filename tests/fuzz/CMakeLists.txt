# Fuzz targets (libFuzzer). Requires clang; enable with
#   cmake -S . -B build-fuzz -DBEDROCKKV_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
# Everything linked into the targets is compiled with coverage
# instrumentation so the fuzzer can see into the library, not just the
# harnesses. CI runs each target as a short smoke fuzz (see ci.yml).
add_executable(resp_fuzz resp_fuzz.cpp)
target_link_libraries(resp_fuzz PRIVATE bedrockkv)
target_compile_options(resp_fuzz PRIVATE -fsanitize=fuzzer)
target_link_options(resp_fuzz PRIVATE -fsanitize=fuzzer)

add_executable(wal_fuzz wal_fuzz.cpp)
target_link_libraries(wal_fuzz PRIVATE bedrockkv)
target_compile_options(wal_fuzz PRIVATE -fsanitize=fuzzer)
target_link_options(wal_fuzz PRIVATE -fsanitize=fuzzer)

add_executable(server_fuzz server_fuzz.cpp)
target_link_libraries(server_fuzz PRIVATE bedrockkv)
target_compile_options(server_fuzz PRIVATE -fsanitize=fuzzer)
target_link_options(server_fuzz PRIVATE -fsanitize=fuzzer)
