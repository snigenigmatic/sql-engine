add_library(optimizer
    optimizer.cpp
    estimator.cpp
)

target_link_libraries(optimizer PUBLIC parser common)
