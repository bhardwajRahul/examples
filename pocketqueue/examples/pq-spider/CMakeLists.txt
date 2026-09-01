cmake_minimum_required(VERSION 3.20)
project(pq_spider C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# --- Warnings, sanitizer support (mirrors top-level flags) --------------
add_compile_options(
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    -Werror=implicit-function-declaration
    -Wmissing-prototypes -Wstrict-prototypes
    -Wformat=2 -Wnull-dereference -Wdouble-promotion
    -Wno-unused-parameter
)
option(PQ_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(PQ_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
if(PQ_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
endif()
if(PQ_ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=undefined)
endif()

# --- Dependencies ----------------------------------------------------
# pq-spider deliberately avoids libcurl — it uses raw sockets so the
# example builds against the same vendored dependencies as the server.

# --- Sources ---------------------------------------------------------
set(PQ_SPIDER_SOURCES
    src/main.c
    src/url_norm.c
    src/dedup.c
    src/http_fetch.c
    src/extract.c
    src/worker.c
    src/pqctl_client.c
)

add_executable(pq-spider ${PQ_SPIDER_SOURCES})
# CMAKE_CURRENT_SOURCE_DIR is examples/pq-spider/ here; the top-level
# repo root is two levels up.
get_filename_component(PQ_ROOT ${CMAKE_CURRENT_SOURCE_DIR} DIRECTORY)
get_filename_component(PQ_ROOT ${PQ_ROOT} DIRECTORY)
target_include_directories(pq-spider PRIVATE
    src
    ${PQ_ROOT}/third_party   # for cjson/cJSON.h
    ${PQ_ROOT}/include        # for pocketqueue/service.h
)
target_link_libraries(pq-spider PRIVATE pocketqueue_core pthread)

install(TARGETS pq-spider RUNTIME DESTINATION bin)

# --- End-to-end test ---------------------------------------------------
# Runs the spider against a tiny local web server; verifies the pages queue
# drains and the bad link dead-letters into pages.dead.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME e2e_spider
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/spider_test.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(e2e_spider PROPERTIES TIMEOUT 60)
