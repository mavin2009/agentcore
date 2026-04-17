cmake_minimum_required(VERSION 3.20)

project(agentcore_smoke_consumer LANGUAGES CXX)

find_package(agentcore CONFIG REQUIRED)

if(NOT TARGET agentcore::graph)
    message(FATAL_ERROR "expected exported target agentcore::graph")
endif()

if(NOT TARGET agentcore::state)
    message(FATAL_ERROR "expected exported target agentcore::state")
endif()

if(NOT TARGET agentcore::runtime)
    message(FATAL_ERROR "expected exported target agentcore::runtime")
endif()

if(NOT TARGET agentcore::execution)
    message(FATAL_ERROR "expected exported target agentcore::execution")
endif()

if(NOT TARGET agentcore::adapters)
    message(FATAL_ERROR "expected exported target agentcore::adapters")
endif()

if(NOT TARGET agentcore::agentcore)
    message(FATAL_ERROR "expected exported target agentcore::agentcore")
endif()

add_executable(agentcore_smoke_consumer main.cpp)
target_link_libraries(agentcore_smoke_consumer PRIVATE agentcore::agentcore)
