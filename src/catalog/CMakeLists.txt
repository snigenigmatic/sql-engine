add_library(catalog
    catalog.cpp
    database.cpp
    legacy_import.cpp
)

target_link_libraries(catalog PUBLIC storage common)
