# examples/minimal/CMakeLists.txt
add_executable(graphflow_minimal main.cpp)
target_link_libraries(graphflow_minimal PRIVATE graphflow_engine)
# 运行时工作目录设为项目根，以便找到 examples/minimal/graph.json
set_target_properties(graphflow_minimal PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# smoke test: 工作目录 = 源码根，main 默认读 examples/minimal/graph.json
add_test(NAME graphflow_minimal_smoke COMMAND graphflow_minimal)
set_tests_properties(graphflow_minimal_smoke PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
