# ---- Distance computation tests ----
add_executable(test_distance test_distance.cpp)
target_link_libraries(test_distance PRIVATE
    flux_core
    GTest::gtest
    GTest::gtest_main
)
add_test(NAME distance COMMAND test_distance)

# ---- Config tests ----
add_executable(test_config test_config.cpp)
target_link_libraries(test_config PRIVATE
    flux_core
    GTest::gtest
    GTest::gtest_main
)
add_test(NAME config COMMAND test_config)

# ---- Engine tests (database, search, filter, BM25, WAL, hybrid) ----
add_executable(test_engine test_engine.cpp)
target_link_libraries(test_engine PRIVATE
    flux_core
    GTest::gtest
    GTest::gtest_main
)
add_test(NAME engine COMMAND test_engine)

# ---- Index tests (HNSW + IVF + recall verification) ----
add_executable(test_index test_index.cpp)
target_link_libraries(test_index PRIVATE
    flux_core
    GTest::gtest
    GTest::gtest_main
)
add_test(NAME index COMMAND test_index)
