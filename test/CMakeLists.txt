# llbt test runner — reuses the upstream unit-test harness (test/util).
file(GLOB LLBT_TEST_SOURCES CONFIGURE_DEPENDS test_*.cpp)
file(GLOB LLBT_TEST_UTIL_SOURCES CONFIGURE_DEPENDS util/*.cpp)

add_executable(llbt-tests
    main.cpp
    test_all.cpp
    ${LLBT_TEST_SOURCES}
    ${LLBT_TEST_UTIL_SOURCES})

target_link_libraries(llbt-tests PRIVATE llbt-core)
target_include_directories(llbt-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME llbt-tests COMMAND llbt-tests)
set_tests_properties(llbt-tests PROPERTIES TIMEOUT 900)
