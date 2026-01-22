add_library(parser
    parser.cpp
    ast.cpp
)

target_link_libraries(parser PUBLIC lexer common)
