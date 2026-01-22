add_library(storage
    table.cpp
    page.cpp
    buffer_pool.cpp
    disk_manager.cpp
)

target_link_libraries(storage PUBLIC common)
