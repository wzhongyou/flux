cmake_minimum_required(VERSION 3.25)
project(flux
    VERSION 2.0.0
    DESCRIPTION "Vector database recall engine"
    LANGUAGES CXX
)

# ---- C++17 standard ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Sanitizers for debug builds ----
if(CMAKE_BUILD_TYPE MATCHES "Debug" OR SANITIZE)
    add_compile_options(-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address -fsanitize=undefined)
endif()

# ---- Find dependencies (vcpkg or system) ----
find_package(hwy CONFIG REQUIRED)              # Google Highway (SIMD)
find_package(nlohmann_json CONFIG REQUIRED)   # JSON
find_package(httplib CONFIG REQUIRED)          # HTTP server
find_package(spdlog CONFIG REQUIRED)           # Logging
find_package(CLI11 CONFIG REQUIRED)            # CLI parsing
find_package(yaml-cpp CONFIG REQUIRED)         # YAML config

# ---- Source library ----
add_subdirectory(src)

# ---- Tests ----
option(BUILD_TESTS "Build test suite" ON)
if(BUILD_TESTS)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)
    add_subdirectory(tests)
endif()
