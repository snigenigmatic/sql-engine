# Common library
add_library(common
    value.cpp
    schema.cpp
    tuple.cpp
    logger.cpp
)

# Make headers accessible
target_include_directories(common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Set library properties
set_target_properties(common PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)