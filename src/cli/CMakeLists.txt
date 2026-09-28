add_library(cli
    shell.cpp
)

target_link_libraries(cli PUBLIC session catalog execution parser common)
