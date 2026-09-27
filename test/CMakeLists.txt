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

# REPL shell
add_sqlengine_test(cli_test cli/cli_test.cpp)

# Golden SQL scripts (test/sql/*.test)
add_sqlengine_test(sql_logic_test sql/sql_logic_test.cpp)
target_compile_definitions(sql_logic_test PRIVATE SQL_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/sql")
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
