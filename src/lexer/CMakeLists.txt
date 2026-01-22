add_library(lexer
    lexer.cpp
    token.cpp
)

target_link_libraries(lexer PUBLIC common)
