cmake_minimum_required(VERSION 3.20)
project(pocketqueue
    VERSION 1.0.0
    DESCRIPTION "A lightweight, durable message queue server"
    LANGUAGES C)

# ---------------------------------------------------------------------------
# Standard, warnings, sanitizers
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(PQ_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(PQ_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

# POSIX feature test macro for things like gmtime_r, strerror_r.
add_compile_definitions(_POSIX_C_SOURCE=200809L _DEFAULT_SOURCE=1)

add_compile_options(
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    -Werror=implicit-function-declaration
    -Wmissing-prototypes -Wstrict-prototypes
    -Wformat=2 -Wnull-dereference -Wdouble-promotion
    -Wno-unused-parameter
)

if(PQ_ENABLE_ASAN)
    message(STATUS "AddressSanitizer ENABLED")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
endif()
if(PQ_ENABLE_UBSAN)
    message(STATUS "UndefinedBehaviorSanitizer ENABLED")
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=undefined)
endif()

# ---------------------------------------------------------------------------
# Vendored dependencies
# ---------------------------------------------------------------------------
# cJSON is shipped as a single .c/.h pair in third_party/. CMake 4 dropped
# compatibility with cJSON 1.7.x's CMakeLists, and FetchContent's CMake
# build adds noise we don't need for ~3,500 lines of JSON code.

# SQLite is shipped as the official C amalgamation. FetchContent pulls the
# zip from sqlite.org; the amalgamation has no CMakeLists, so we compile
# the .c file directly as part of our core library and pull in the .h
# header path via target_include_directories below.
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
set(FETCHCONTENT_POLICY_VERSION_MINIMUM 3.5)

FetchContent_Declare(sqlite
    URL     https://sqlite.org/2026/sqlite-amalgamation-3530300.zip
    URL_HASH SHA3_256=d45c688a8cb23f68611a894a756a12d7eb6ab6e9e2468ca70adbeab3808b5ab9
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# cmocka is shipped as a static library built from the official source.
FetchContent_Declare(cmocka
    GIT_REPOSITORY https://gitlab.com/cmocka/cmocka.git
    GIT_TAG        cmocka-1.1.7
    GIT_SHALLOW    TRUE)
set(CMOCKA_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMOCKA_BUILD_STATIC_LIBS ON  CACHE BOOL "" FORCE)
set(CMOCKA_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CMOCKA_INSTALL OFF CACHE BOOL "" FORCE)
set(WITH_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(cmocka sqlite)

# Vendored libraries should not inherit our strict warnings — gate them.
if(TARGET cmocka)
    target_compile_options(cmocka PRIVATE
        -Wno-conversion -Wno-double-promotion -Wno-sign-conversion)
endif()

# SQLite's amalgamation ships no CMakeLists, but MakeAvailable still
# populates ${sqlite_SOURCE_DIR} so we can use it directly below.

# ---------------------------------------------------------------------------
# Public include path
# ---------------------------------------------------------------------------
add_library(pq_platform_iface INTERFACE)
target_include_directories(pq_platform_iface INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(pq_platform_iface INTERFACE c_std_17)
add_library(pocketqueue::headers ALIAS pq_platform_iface)

# ---------------------------------------------------------------------------
# Core library (everything except the two main()s)
# ---------------------------------------------------------------------------
set(PQ_CORE_SOURCES
    src/config.c
    src/logger.c
    src/clock.c
    src/http_server.c
    src/http_routes.c
    src/json_util.c
    src/str_util.c
    src/random_util.c
    src/notifier.c
    src/sqlite_repository.c
    src/migrations.c
    src/queue_service.c
    src/queue_service_inmem.c
    src/queue_service_sqlite.c
    third_party/cJSON.c
    ${sqlite_SOURCE_DIR}/sqlite3.c
)

add_library(pocketqueue_core STATIC ${PQ_CORE_SOURCES})
target_include_directories(pocketqueue_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${sqlite_SOURCE_DIR}
)
# SQLite amalgamation emits a few warnings under our strict set; silence
# them at the target level so they don't pollute every build.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(
        "${sqlite_SOURCE_DIR}/sqlite3.c"
        PROPERTIES
            COMPILE_OPTIONS "-Wno-shadow;-Wno-format-nonliteral;-Wno-stringop-truncation;-Wno-conversion"
    )
endif()

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
add_executable(pocketqueue-server src/server_main.c)
target_link_libraries(pocketqueue-server PRIVATE pocketqueue_core)
set_target_properties(pocketqueue-server PROPERTIES OUTPUT_NAME "pocketqueue-server")

# pqctl is built in Stage 3 (HTTP API); placeholder target keeps the build
# honest until then.
add_executable(pqctl src/pqctl_main.c)
target_link_libraries(pqctl PRIVATE pocketqueue_core)
set_target_properties(pqctl PROPERTIES OUTPUT_NAME "pqctl")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
option(PQ_BUILD_TESTS "Build unit and e2e tests" ON)
if(PQ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
    add_subdirectory(tests/e2e)
endif()

# ---------------------------------------------------------------------------
# Install (basic)
# ---------------------------------------------------------------------------
install(TARGETS pocketqueue-server pqctl
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/ DESTINATION include)

option(PQ_BUILD_SPIDER "Build pq-spider example" ON)
if(PQ_BUILD_SPIDER)
    add_subdirectory(examples/pq-spider)
endif()