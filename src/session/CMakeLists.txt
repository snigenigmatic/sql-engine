add_library(session
    session.cpp
)

target_link_libraries(session PUBLIC execution catalog parser lexer common)
