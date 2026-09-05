# Each test executable links the bedrockkv library and gtest_main
# (gtest_main provides main(); we only write TEST bodies).
add_executable(status_test status_test.cpp)
target_link_libraries(status_test PRIVATE bedrockkv GTest::gtest_main)

# gtest_discover_tests registers each TEST(...) as an individual ctest
# entry, so failures are reported per-case, and it runs them at test time
# (not build time), which plays well with cross-compilation and CI sharding.
include(GoogleTest)
gtest_discover_tests(status_test)
