include(CTest)

set(PQ_UNIT_SOURCES
    test_smoke.c
    test_clock.c
    test_config.c
    test_str_util.c
    test_random_util.c
    test_queue_service.c
)

# Each test source defines its own main(); link them as separate binaries
# so we get per-test reporting under CTest.
foreach(src ${PQ_UNIT_SOURCES})
    get_filename_component(test_name ${src} NAME_WE)
    add_executable(${test_name} ${src})
    target_include_directories(${test_name} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party
    )
    target_link_libraries(${test_name}
        PRIVATE pocketqueue_core cmocka
    )
    target_compile_definitions(${test_name} PRIVATE PQ_BUILDING_TESTS=1)
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 30)
endforeach()