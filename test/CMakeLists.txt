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

# Integration tests (add later)
# add_sqlengine_test(query_test integration/query_test.cpp)