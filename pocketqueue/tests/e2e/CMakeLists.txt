# End-to-end tests use Python 3 with the standard library only.
# Run from the build tree: ctest --test-dir build --output-on-failure.

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_test(NAME e2e_smoke
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/smoke.py
            $<TARGET_FILE:pocketqueue-server>
            ${CMAKE_SOURCE_DIR}/migrations
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(e2e_smoke PROPERTIES TIMEOUT 60)

add_test(NAME e2e_api
    COMMAND ${CMAKE_COMMAND} -E env
            BUILD_DIR=${CMAKE_BINARY_DIR}
            ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_api.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(e2e_api PROPERTIES TIMEOUT 120)

add_test(NAME e2e_stress
    COMMAND ${CMAKE_COMMAND} -E env
            BUILD_DIR=${CMAKE_BINARY_DIR}
            ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/stress.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(e2e_stress PROPERTIES TIMEOUT 90)
