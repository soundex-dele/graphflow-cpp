# examples/multi/CMakeLists.txt
add_executable(graphflow_multi main.cpp)
target_link_libraries(graphflow_multi PRIVATE graphflow_engine)
add_test(NAME graphflow_multi_smoke COMMAND graphflow_multi)
set_tests_properties(graphflow_multi_smoke PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
