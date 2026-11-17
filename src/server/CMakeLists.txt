add_library(server
    server.cpp
)

target_link_libraries(server PUBLIC net cli session catalog)

# The network server
add_executable(sqlnode main.cpp)
target_link_libraries(sqlnode PRIVATE server)
install(TARGETS sqlnode DESTINATION bin)
