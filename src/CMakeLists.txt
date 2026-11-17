# Add subdirectories for each component
add_subdirectory(common)
add_subdirectory(lexer)
add_subdirectory(parser)
add_subdirectory(optimizer)
add_subdirectory(execution)
add_subdirectory(storage)
add_subdirectory(catalog)
add_subdirectory(session)
add_subdirectory(cli)
add_subdirectory(net)
add_subdirectory(server)
add_subdirectory(cluster)

# Main executable (REPL)
add_executable(sqlengine main.cpp)

# Link all component libraries
target_link_libraries(sqlengine
    PRIVATE
        common
        lexer
        parser
        optimizer
        execution
        storage
        catalog
        session
        cli
        net
)

# Install target
install(TARGETS sqlengine DESTINATION bin)