# examples/agent/CMakeLists.txt
# 智能体示例:Perceive->Think->Act 闭环,mock LLM 按规则决策(纯模拟)。
# 所有事件类型统一为 AgentStateEvent(无 phase),靠 edge.condition() 语义选路
# + sub 边 condition 角色(seed/feedback)分流。
add_executable(graphflow_agent main.cpp)
target_link_libraries(graphflow_agent PRIVATE graphflow_engine)
add_test(NAME graphflow_agent_smoke COMMAND graphflow_agent)
set_tests_properties(graphflow_agent_smoke PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
