find_package(Threads REQUIRED)
add_executable(graphflow_av_bench av_benchmark.cpp)
target_link_libraries(graphflow_av_bench PRIVATE graphflow::engine Threads::Threads)
if(WIN32)
    target_link_libraries(graphflow_av_bench PRIVATE psapi)
endif()
# Deliberately not registered with CTest: load tests are opt-in only.
