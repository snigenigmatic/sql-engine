add_library(storage
    table.cpp
    page.cpp
    buffer_pool.cpp
    pager.cpp
    disk_manager.cpp
    btree.cpp
)

target_link_libraries(storage PUBLIC common)
