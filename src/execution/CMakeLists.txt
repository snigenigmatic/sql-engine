add_library(execution
    executor.cpp
    seq_scan.cpp
    filter.cpp
    projection.cpp
    index_scan.cpp
)

target_link_libraries(execution PUBLIC optimizer storage common)
