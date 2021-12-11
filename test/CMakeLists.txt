add_executable(futex_demo
    ${CMAKE_CURRENT_SOURCE_DIR}/futex_demo.cpp)
target_link_libraries(futex_demo
    ${PROJECT_NAME})
