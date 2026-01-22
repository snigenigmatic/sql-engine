add_library(catalog
    catalog.cpp
)

target_link_libraries(catalog PUBLIC storage common)
