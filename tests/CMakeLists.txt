# Each test executable links the bedrockkv library and gtest_main
# (gtest_main provides main(); we only write TEST bodies).
add_executable(status_test status_test.cpp)
target_link_libraries(status_test PRIVATE bedrockkv GTest::gtest_main)

add_executable(skiplist_test skiplist_test.cpp)
target_link_libraries(skiplist_test PRIVATE bedrockkv GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(status_test)
gtest_discover_tests(skiplist_test)
