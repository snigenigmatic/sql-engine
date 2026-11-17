# Test macro for easier test addition
macro(add_sqlengine_test test_name)
    add_executable(${test_name} ${ARGN})
    target_link_libraries(${test_name}
        PRIVATE
            gtest
            gtest_main
            common
            lexer
            parser
            execution
            storage
            catalog
            session
            cli
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
endmacro()

# Common tests
add_sqlengine_test(value_test common/value_test.cpp)
add_sqlengine_test(schema_test common/schema_test.cpp)
add_sqlengine_test(tuple_test common/tuple_test.cpp)

# Lexer tests
add_sqlengine_test(lexer_test lexer/lexer_test.cpp)

# Parser tests
add_sqlengine_test(parser_test parser/parser_test.cpp)

# Integration tests
add_sqlengine_test(query_test integration/query_test.cpp)

# Planner cost estimates
add_sqlengine_test(estimator_test optimizer/estimator_test.cpp)

# Client/server
add_sqlengine_test(protocol_test net/protocol_test.cpp)
target_link_libraries(protocol_test PRIVATE net)
add_sqlengine_test(server_test server/server_test.cpp)
target_link_libraries(server_test PRIVATE server net)

# REPL shell
add_sqlengine_test(cli_test cli/cli_test.cpp)

# Consistent hashing and placement
add_sqlengine_test(ring_test cluster/ring_test.cpp)
target_link_libraries(ring_test PRIVATE ring)

# Cluster components (need NuRaft)
if(SQLENGINE_DISTRIBUTED)
    add_sqlengine_test(memory_log_store_test cluster/memory_log_store_test.cpp)
    target_link_libraries(memory_log_store_test PRIVATE cluster)
    add_sqlengine_test(nuraft_test cluster/nuraft_test.cpp)
    target_link_libraries(nuraft_test PRIVATE cluster)
    add_sqlengine_test(file_log_store_test cluster/file_log_store_test.cpp)
    target_link_libraries(file_log_store_test PRIVATE cluster)
    add_sqlengine_test(replicated_shard_test cluster/replicated_shard_test.cpp)
    target_link_libraries(replicated_shard_test PRIVATE cluster)
    add_sqlengine_test(cluster_test cluster/cluster_test.cpp)
    target_link_libraries(cluster_test PRIVATE cluster)
    add_sqlengine_test(planner_test cluster/planner_test.cpp)
    target_link_libraries(planner_test PRIVATE cluster)
endif()

# Golden SQL scripts (test/sql/*.test)
add_sqlengine_test(sql_logic_test sql/sql_logic_test.cpp)
target_compile_definitions(sql_logic_test PRIVATE SQL_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/sql")
target_link_libraries(sql_logic_test PRIVATE server net)
if(SQLENGINE_DISTRIBUTED)
    target_link_libraries(sql_logic_test PRIVATE cluster)
    target_compile_definitions(sql_logic_test PRIVATE SQLENGINE_DISTRIBUTED)
endif()
# Storage tests
add_sqlengine_test(buffer_pool_test storage/buffer_pool_test.cpp)
add_sqlengine_test(table_heap_test storage/table_heap_test.cpp)

# Catalog tests
add_sqlengine_test(catalog_persistence_test catalog/catalog_persistence_test.cpp)
add_sqlengine_test(btree_test storage/btree_test.cpp)

# Recovery tests
add_sqlengine_test(wal_test recovery/wal_test.cpp)
add_sqlengine_test(crash_recovery_test recovery/crash_recovery_test.cpp)

# Session tests
add_sqlengine_test(session_test session/session_test.cpp)
add_sqlengine_test(evaluator_test execution/evaluator_test.cpp)
