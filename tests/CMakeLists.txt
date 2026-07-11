find_package(Catch2 3 CONFIG REQUIRED)

add_executable(unit_tests
    test_protocol.cpp
    test_movement.cpp
)

target_include_directories(unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/common/include
    ${CMAKE_SOURCE_DIR}/server/game/include
)

target_link_libraries(unit_tests PRIVATE Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(unit_tests)
