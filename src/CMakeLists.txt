# ---- Flux core library ----
add_library(flux_core STATIC
    # Core types + distance
    types.h
    distance.h
    distance.cpp

    # Configuration
    config.h
    config.cpp

    # Schema
    schema.h
    schema.cpp

    # Filter engine
    filter.h
    filter.cpp

    # Full-text search
    bm25.h
    bm25.cpp

    # Persistence
    wal.h
    wal.cpp

    # Core engine
    database.h
    database.cpp

    # ANN Indexes
    hnsw.h
    hnsw.cpp
    ivf.h
    ivf.cpp

    # IO (import/export/snapshot)
    io.h
    io.cpp

    # HTTP Server
    server.h
    server.cpp
)

target_include_directories(flux_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(flux_core PUBLIC
    hwy::hwy
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    httplib::httplib
)

# ---- Flux CLI executable ----
add_executable(flux main.cpp)
target_link_libraries(flux PRIVATE
    flux_core
    CLI11::CLI11
)
