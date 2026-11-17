add_library(net
    protocol.cpp
    socket.cpp
    remote_connection.cpp
)

target_link_libraries(net PUBLIC session execution common)
