# Each test executable links the bedrockkv library and gtest_main
# (gtest_main provides main(); we only write TEST bodies).
add_executable(status_test status_test.cpp)
target_link_libraries(status_test PRIVATE bedrockkv GTest::gtest_main)

add_executable(skiplist_test skiplist_test.cpp)
target_link_libraries(skiplist_test PRIVATE bedrockkv GTest::gtest_main)

add_executable(crc32_test crc32_test.cpp)
target_link_libraries(crc32_test PRIVATE bedrockkv GTest::gtest_main)

add_executable(log_test log_test.cpp)
target_link_libraries(log_test PRIVATE bedrockkv GTest::gtest_main)

add_executable(db_test db_test.cpp)
target_link_libraries(db_test PRIVATE bedrockkv GTest::gtest_main)

add_executable(memtable_test memtable_test.cpp)
target_link_libraries(memtable_test PRIVATE bedrockkv GTest::gtest_main)

add_executable(iterator_test iterator_test.cpp)
target_link_libraries(iterator_test PRIVATE bedrockkv GTest::gtest_main)

add_executable(sstable_test sstable_test.cpp)
target_link_libraries(sstable_test PRIVATE bedrockkv GTest::gtest_main)

add_executable(vlog_test vlog_test.cpp)
target_link_libraries(vlog_test PRIVATE bedrockkv GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(status_test)
gtest_discover_tests(skiplist_test)
gtest_discover_tests(crc32_test)
gtest_discover_tests(log_test)
gtest_discover_tests(db_test)
gtest_discover_tests(memtable_test)
gtest_discover_tests(iterator_test)
gtest_discover_tests(sstable_test)
gtest_discover_tests(vlog_test)
