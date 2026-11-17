add_library(cluster
    raft/memory_log_store.cpp
    raft/memory_state_manager.cpp
    raft/raft_logger.cpp
)

target_link_libraries(cluster PUBLIC nuraft common)
