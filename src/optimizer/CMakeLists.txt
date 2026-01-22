add_library(optimizer
    optimizer.cpp
)

target_link_libraries(optimizer PUBLIC parser common)
