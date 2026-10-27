cmake_minimum_required(VERSION 3.16)
project(graphflow_cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    add_compile_options(/FS /utf-8)
endif()
if(WIN32)
    add_compile_definitions(WIN32_LEAN_AND_MEAN _UNICODE UNICODE NOMINMAX)
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/utoolkit/CMakeLists.txt")
    message(FATAL_ERROR
        "utoolkit submodule is missing. Run: git submodule update --init --recursive")
endif()

add_subdirectory(third_party/nlohmann)
add_subdirectory(utoolkit)

add_library(graphflow_core STATIC
    utils/logger.h
    core/blackboard.h core/blackboard.cpp
    core/node_runtime_context.h
    core/graph_edge.h
    core/event.h
    core/event_bus.h core/event_bus.cpp
    core/lifecycle.h
    core/common.h
    core/graph_node.h core/graph_node.cpp
    core/graph_path.h core/graph_path.cpp
)
target_include_directories(graphflow_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(graphflow_core PUBLIC utoolkit nlohmann_json)

add_library(graphflow_engine STATIC
    engine/node_config.h
    engine/node_factory.h engine/node_factory.cpp
    engine/graph_builder.h engine/graph_builder.cpp
    engine/json_graph_builder.h engine/json_graph_builder.cpp
    engine/build_result.h
    engine/graph_engine.h engine/graph_engine.cpp
)
target_link_libraries(graphflow_engine PUBLIC graphflow_core nlohmann_json)

add_library(graphflow::core ALIAS graphflow_core)
add_library(graphflow::engine ALIAS graphflow_engine)

option(GRAPHFLOW_BUILD_EXAMPLES "Build GraphFlow examples" ON)
if(GRAPHFLOW_BUILD_EXAMPLES)
    add_subdirectory(examples/minimal)
    add_subdirectory(examples/multi)
    add_subdirectory(examples/agent)
    add_subdirectory(examples/av_pipeline)
endif()

option(GRAPHFLOW_BUILD_TESTS "Build GraphFlow tests" OFF)
if(GRAPHFLOW_BUILD_TESTS)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)
    add_subdirectory(tests)
endif()

option(GRAPHFLOW_BUILD_BENCHMARKS "Build standalone AV performance benchmarks" OFF)
if(GRAPHFLOW_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
