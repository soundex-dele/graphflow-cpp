# examples/av_pipeline/CMakeLists.txt
# 音视频管线端到端示例:双流采集->编码->打包->发送->接收->解码->同步渲染。
# 拓扑唯一真相源 = av_pipeline.json(与 minimal/selector_tree/bt 同款,文件驱动)。
add_executable(graphflow_av_pipeline main.cpp)
target_link_libraries(graphflow_av_pipeline PRIVATE graphflow_engine)
# 运行时工作目录设为项目根,以便找到 examples/av_pipeline/av_pipeline.json
set_target_properties(graphflow_av_pipeline PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
# smoke test: 工作目录 = 源码根,main 默认读 examples/av_pipeline/av_pipeline.json
add_test(NAME graphflow_av_pipeline_smoke COMMAND graphflow_av_pipeline)
set_tests_properties(graphflow_av_pipeline_smoke PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
