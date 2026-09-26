# FluxDM — engine/CMakeLists.txt
# Builds the C++ engine layer (Phase 2) as a static library: libfluxdm_engine.a
# Links against the C core objects and OpenSSL.
#
# Usage:
#   cd engine && cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --parallel
#   cmake --build build --target test_engine   (runs Phase 2 unit tests)

cmake_minimum_required(VERSION 3.20)
project(fluxdm_engine CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Dependencies ───────────────────────────────────────────────────────────
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# ── Compiler flags ─────────────────────────────────────────────────────────
set(COMMON_FLAGS
    -Wall -Wextra -Wpedantic
    -Wshadow -Wformat=2
    -fstack-protector-strong
    -D_GNU_SOURCE
)

set(RELEASE_FLAGS ${COMMON_FLAGS} -O2 -DNDEBUG -flto)
set(DEBUG_FLAGS   ${COMMON_FLAGS} -Og -g3 -DDEBUG
    -fsanitize=address,undefined -fno-omit-frame-pointer)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(${DEBUG_FLAGS})
    add_link_options(-fsanitize=address,undefined)
else()
    add_compile_options(${RELEASE_FLAGS})
    add_link_options(-flto)
endif()

# ── C core sources (compiled as C, linked into the engine) ────────────────
set(C_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../core")
set(IPC_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/../ipc")

set(C_CORE_SRCS
    ${C_CORE_DIR}/src/errors.c
    ${C_CORE_DIR}/src/bandwidth.c
    ${C_CORE_DIR}/src/checksum.c
    ${C_CORE_DIR}/src/resume.c
    ${C_CORE_DIR}/src/connection.c
    ${C_CORE_DIR}/src/segment.c
    ${C_CORE_DIR}/src/downloader.c
    ${C_CORE_DIR}/src/protocol/http.c
    ${C_CORE_DIR}/src/protocol/https.c
    ${C_CORE_DIR}/src/ipc/socket_server.c
)

# Build C core as a static library so the engine can link it
add_library(fluxdm_core STATIC ${C_CORE_SRCS})
target_include_directories(fluxdm_core PUBLIC
    ${C_CORE_DIR}/include
    ${IPC_DIR}
)
target_compile_options(fluxdm_core PRIVATE
    -std=c11 -Wall -Wextra -D_GNU_SOURCE
)
target_link_libraries(fluxdm_core PUBLIC
    OpenSSL::SSL OpenSSL::Crypto Threads::Threads
)

# ── C++ engine library ────────────────────────────────────────────────────
set(ENGINE_SRCS
    src/SegmentManager.cpp
    src/AdaptiveTuner.cpp
    src/BufferManager.cpp
    src/MetadataParser.cpp
    src/TorrentEngine.cpp
    src/PeerDiscovery.cpp
)

add_library(fluxdm_engine STATIC ${ENGINE_SRCS})
target_include_directories(fluxdm_engine PUBLIC
    include
    ${C_CORE_DIR}/include
    ${IPC_DIR}
)
target_link_libraries(fluxdm_engine PUBLIC
    fluxdm_core
    OpenSSL::SSL OpenSSL::Crypto
    Threads::Threads
)

# ── Phase 2 unit tests ────────────────────────────────────────────────────
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_engine.cpp")
    add_executable(test_engine
        ../tests/test_engine.cpp
    )
    target_link_libraries(test_engine PRIVATE
        fluxdm_engine fluxdm_core
        OpenSSL::SSL OpenSSL::Crypto Threads::Threads
    )
    target_compile_options(test_engine PRIVATE
        ${DEBUG_FLAGS}
    )
    add_custom_target(run_tests
        COMMAND test_engine
        DEPENDS test_engine
        COMMENT "Running Phase 2 engine unit tests"
    )
endif()

# ── Install ───────────────────────────────────────────────────────────────
install(TARGETS fluxdm_engine fluxdm_core
    ARCHIVE DESTINATION lib
)
install(DIRECTORY include/
    DESTINATION include/fluxdm/engine
    FILES_MATCHING PATTERN "*.hpp"
)
