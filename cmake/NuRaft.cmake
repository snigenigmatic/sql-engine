# NuRaft (Raft consensus) and standalone asio, its networking layer.
#
# Both are fetched at configure time. NuRaft's own CMakeLists always builds
# its examples and tests and forces -std=c++11, so it is not added as a
# subproject: its library sources are compiled here as the `nuraft` target.
# To build offline, point FETCHCONTENT_SOURCE_DIR_ASIO and
# FETCHCONTENT_SOURCE_DIR_NURAFT at local checkouts.

include(FetchContent)

# asio 1.28 still has io_service (removed in 1.33), which NuRaft 2.1 uses
FetchContent_Declare(asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-28-0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR do-not-add)
FetchContent_Declare(nuraft
    GIT_REPOSITORY https://github.com/eBay/NuRaft.git
    GIT_TAG v2.1.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR do-not-add)
FetchContent_MakeAvailable(asio nuraft)

find_package(Threads REQUIRED)
# NuRaft's no-SSL build uses a mock socket that newer asio rejects, so it is
# built against OpenSSL; TLS stays off unless a server's options enable it
find_package(OpenSSL REQUIRED)

file(GLOB NURAFT_SOURCES CONFIGURE_DEPENDS ${nuraft_SOURCE_DIR}/src/*.cxx)
add_library(nuraft STATIC ${NURAFT_SOURCES})
target_include_directories(nuraft SYSTEM
    PUBLIC
        ${nuraft_SOURCE_DIR}/include
        ${nuraft_SOURCE_DIR}/include/libnuraft
        ${asio_SOURCE_DIR}/asio/include
    PRIVATE
        ${nuraft_SOURCE_DIR}/src)
target_compile_definitions(nuraft PUBLIC ASIO_STANDALONE)
# Third-party code: keep its warnings out of our build output. Its headers
# use uint64_t without <cstdint>, which GCC 13 no longer pulls in for them;
# our code includes them through src/cluster/raft/nuraft.h, which adds it
target_compile_options(nuraft PRIVATE -w -include cstdint)
target_link_libraries(nuraft PUBLIC Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
