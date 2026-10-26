include(GoogleTest)

add_library(graphflow_test_common INTERFACE)
target_include_directories(graphflow_test_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(graphflow_test_common INTERFACE graphflow_engine)

add_executable(graphflow_core_tests
    core/lifecycle_test.cpp
    core/event_bus_test.cpp
    core/blackboard_test.cpp
    core/graph_node_pubsub_test.cpp
)
target_link_libraries(graphflow_core_tests PRIVATE
    graphflow_core GTest::gtest GTest::gtest_main graphflow_test_common)
gtest_discover_tests(graphflow_core_tests)

add_executable(graphflow_engine_tests
    engine/graph_builder_test.cpp
    engine/json_graph_builder_test.cpp
    engine/graph_engine_test.cpp
    engine/external_graph_test.cpp
    engine/node_factory_test.cpp
)
target_link_libraries(graphflow_engine_tests PRIVATE
    graphflow_engine GTest::gtest GTest::gtest_main graphflow_test_common)
gtest_discover_tests(graphflow_engine_tests)
