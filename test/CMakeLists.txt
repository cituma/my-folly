find_package(GTest)
include_directories(${GTEST_INCLUDE_DIRS})

add_executable(futex_test
    ${CMAKE_CURRENT_SOURCE_DIR}/futex_test.cpp)
target_link_libraries(futex_test
    ${PROJECT_NAME}
    ${GTEST_LIBRARIES})

add_executable(turn_sequencer_test
    ${CMAKE_CURRENT_SOURCE_DIR}/turn_sequencer_test.cpp)
target_link_libraries(turn_sequencer_test
    ${PROJECT_NAME}
    ${GTEST_LIBRARIES})
