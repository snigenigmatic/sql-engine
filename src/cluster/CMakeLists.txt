# Consistent hashing and placement: pure code, always built
add_library(ring
    ring/hash.cpp
    ring/hash_ring.cpp
    ring/placement.cpp
)

target_link_libraries(ring PUBLIC execution common)

# Raft building blocks: need NuRaft
if(SQLENGINE_DISTRIBUTED)
    add_library(cluster
        raft/memory_log_store.cpp
        raft/memory_state_manager.cpp
        raft/raft_logger.cpp
    )

    target_link_libraries(cluster PUBLIC nuraft ring common)
endif()
