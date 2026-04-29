cmake_minimum_required(VERSION 3.20)

project(agentcore VERSION 0.1.13 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
include(CTest)

find_package(Threads REQUIRED)

option(AGENTCORE_BUILD_EXAMPLES "Build agentcore example executables." ON)
option(AGENTCORE_BUILD_BENCHMARKS "Build agentcore benchmark executables." ON)
option(AGENTCORE_BUILD_PYTHON_BINDINGS "Build the CPython bindings for the agentcore graph API." ON)
option(AGENTCORE_ENABLE_SQLITE_CHECKPOINTS "Enable SQLite-backed checkpoint storage." ON)
option(AGENTCORE_ENABLE_CURL_ADAPTERS "Enable libcurl-backed HTTP transports for adapters." ON)

set(AGENTCORE_HAVE_SQLITE3 OFF)
if(AGENTCORE_ENABLE_SQLITE_CHECKPOINTS)
    find_package(SQLite3 QUIET)
    if(SQLite3_FOUND)
        set(AGENTCORE_HAVE_SQLITE3 ON)
    else()
        message(STATUS "SQLite3 not found; SQLite checkpoint backend disabled.")
    endif()
endif()

set(AGENTCORE_HAVE_CURL OFF)
if(AGENTCORE_ENABLE_CURL_ADAPTERS)
    find_package(CURL QUIET)
    if(CURL_FOUND)
        set(AGENTCORE_HAVE_CURL ON)
    else()
        message(STATUS "CURL not found; libcurl-backed adapters disabled.")
    endif()
endif()

set(AGENTCORE_EXECUTION_SOURCES
    agentcore/src/execution/checkpoint.cpp
    agentcore/src/execution/engine.cpp
    agentcore/src/execution/proof.cpp
    agentcore/src/execution/reactive/memoization.cpp
    agentcore/src/execution/subgraph/inline/runner.cpp
    agentcore/src/execution/subgraph/reuse/engine_pool.cpp
    agentcore/src/execution/subgraph/session_runtime.cpp
    agentcore/src/execution/subgraph/subgraph_runtime.cpp
    agentcore/src/execution/streaming/public_stream.cpp
    agentcore/src/execution/trace.cpp
)

set(AGENTCORE_GRAPH_SOURCES
    agentcore/src/graph/composition/subgraph.cpp
    agentcore/src/graph/graph_ir.cpp
)

set(AGENTCORE_RUNTIME_SOURCES
    agentcore/src/runtime/async_executor.cpp
    agentcore/src/runtime/model_registry.cpp
    agentcore/src/runtime/scheduler.cpp
    agentcore/src/runtime/tool_registry.cpp
)

set(AGENTCORE_STATE_SOURCES
    agentcore/src/state/context/context_graph.cpp
    agentcore/src/state/intelligence/model.cpp
    agentcore/src/state/intelligence/ops.cpp
    agentcore/src/state/journal/task_journal.cpp
    agentcore/src/state/knowledge_graph.cpp
    agentcore/src/state/state_store.cpp
)

set(AGENTCORE_ADAPTER_SOURCES
    agentcore/adapters/common/http_transport.cpp
    agentcore/adapters/models/gemini_generate_content_model_adapter.cpp
    agentcore/adapters/models/grok_chat_model_adapter.cpp
    agentcore/adapters/models/llm_http_adapter.cpp
    agentcore/adapters/models/local_model_adapter.cpp
    agentcore/adapters/models/openai_chat_model_adapter.cpp
    agentcore/adapters/tools/http_tool.cpp
    agentcore/adapters/tools/http_json_tool.cpp
    agentcore/adapters/tools/sqlite_tool.cpp
)

set(AGENTCORE_PYTHON_SOURCES
    agentcore/src/bindings/python/core/bridge.cpp
    agentcore/src/bindings/python/module/module.cpp
)

set(AGENTCORE_ALL_SOURCES
    ${AGENTCORE_EXECUTION_SOURCES}
    ${AGENTCORE_GRAPH_SOURCES}
    ${AGENTCORE_RUNTIME_SOURCES}
    ${AGENTCORE_STATE_SOURCES}
    ${AGENTCORE_ADAPTER_SOURCES}
)

function(agentcore_configure_target target_name)
    target_include_directories(
        ${target_name}
        PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/agentcore/include>"
            "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    )
    target_compile_features(${target_name} PUBLIC cxx_std_20)
    set_target_properties(${target_name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

function(agentcore_enable_validation_asserts target_name)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU|AppleClang")
        target_compile_options(${target_name} PRIVATE -UNDEBUG)
    elseif(MSVC)
        target_compile_definitions(${target_name} PRIVATE NDEBUG=0)
    endif()
endfunction()

function(agentcore_source_group target_name)
    get_target_property(target_sources ${target_name} SOURCES)
    if(target_sources)
        source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/agentcore" FILES ${target_sources})
    endif()
endfunction()

add_library(agentcore_graph STATIC ${AGENTCORE_GRAPH_SOURCES})
set_target_properties(agentcore_graph PROPERTIES EXPORT_NAME graph)
agentcore_configure_target(agentcore_graph)
agentcore_source_group(agentcore_graph)

add_library(agentcore_state STATIC ${AGENTCORE_STATE_SOURCES})
set_target_properties(agentcore_state PROPERTIES EXPORT_NAME state)
target_link_libraries(agentcore_state PUBLIC agentcore_graph atomic)
agentcore_configure_target(agentcore_state)
agentcore_source_group(agentcore_state)

add_library(agentcore_runtime STATIC ${AGENTCORE_RUNTIME_SOURCES})
set_target_properties(agentcore_runtime PROPERTIES EXPORT_NAME runtime)
target_link_libraries(agentcore_runtime PUBLIC agentcore_state Threads::Threads)
agentcore_configure_target(agentcore_runtime)
agentcore_source_group(agentcore_runtime)

add_library(agentcore_execution STATIC ${AGENTCORE_EXECUTION_SOURCES})
set_target_properties(agentcore_execution PROPERTIES EXPORT_NAME execution)
target_link_libraries(agentcore_execution PUBLIC agentcore_graph agentcore_runtime agentcore_state)
if(AGENTCORE_HAVE_SQLITE3)
    target_link_libraries(agentcore_execution PUBLIC SQLite::SQLite3)
    target_compile_definitions(agentcore_execution PUBLIC AGENTCORE_HAVE_SQLITE3=1)
endif()
agentcore_configure_target(agentcore_execution)
agentcore_source_group(agentcore_execution)

add_library(agentcore_adapters STATIC ${AGENTCORE_ADAPTER_SOURCES})
set_target_properties(agentcore_adapters PROPERTIES EXPORT_NAME adapters)
target_link_libraries(agentcore_adapters PUBLIC agentcore_runtime PRIVATE agentcore_state)
if(AGENTCORE_HAVE_CURL AND TARGET CURL::libcurl)
    target_link_libraries(agentcore_adapters PUBLIC CURL::libcurl)
    target_compile_definitions(agentcore_adapters PUBLIC AGENTCORE_HAVE_CURL=1)
endif()
agentcore_configure_target(agentcore_adapters)
agentcore_source_group(agentcore_adapters)

add_library(agentcore INTERFACE)
add_library(agentcore::agentcore ALIAS agentcore)
set_target_properties(agentcore PROPERTIES EXPORT_NAME agentcore)

target_include_directories(
    agentcore
    INTERFACE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/agentcore/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(
    agentcore
    INTERFACE
        agentcore_graph
        agentcore_state
        agentcore_runtime
        agentcore_execution
        agentcore_adapters
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/agentcore" FILES ${AGENTCORE_ALL_SOURCES})

if(AGENTCORE_BUILD_EXAMPLES)
    add_executable(planner_executor_graph agentcore/examples/planner_executor_graph.cpp)
    target_link_libraries(planner_executor_graph PRIVATE agentcore::agentcore)

    add_executable(retrieval_graph agentcore/examples/retrieval_graph.cpp)
    target_link_libraries(retrieval_graph PRIVATE agentcore::agentcore)

    add_executable(knowledge_graph_workflow agentcore/examples/knowledge_graph_workflow.cpp)
    target_link_libraries(knowledge_graph_workflow PRIVATE agentcore::agentcore)

    add_executable(runtime_proof_suite agentcore/examples/runtime_proof_suite.cpp)
    target_link_libraries(runtime_proof_suite PRIVATE agentcore::agentcore)
    agentcore_enable_validation_asserts(runtime_proof_suite)
endif()

if(AGENTCORE_BUILD_BENCHMARKS)
    add_executable(agentcore_runtime_benchmark agentcore/benchmarks/runtime_benchmark.cpp)
    target_link_libraries(agentcore_runtime_benchmark PRIVATE agentcore::agentcore)
    agentcore_enable_validation_asserts(agentcore_runtime_benchmark)
    add_executable(
        agentcore_persistent_subgraph_session_benchmark
        agentcore/benchmarks/persistent_subgraph_session_benchmark.cpp
    )
    target_link_libraries(
        agentcore_persistent_subgraph_session_benchmark
        PRIVATE
            agentcore::agentcore
    )
    agentcore_enable_validation_asserts(agentcore_persistent_subgraph_session_benchmark)
endif()

if(AGENTCORE_BUILD_PYTHON_BINDINGS)
    find_package(Python3 COMPONENTS Interpreter Development.Module QUIET)
    if(Python3_FOUND)
        set(AGENTCORE_PYTHON_BUILD_ROOT "${CMAKE_CURRENT_BINARY_DIR}/python")
        set(AGENTCORE_PYTHON_PACKAGE_ROOT "${AGENTCORE_PYTHON_BUILD_ROOT}/agentcore")
        file(
            GLOB_RECURSE AGENTCORE_PYTHON_PACKAGE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/python/*.py"
        )
        set(AGENTCORE_PYTHON_PACKAGE_STAMP "${AGENTCORE_PYTHON_BUILD_ROOT}/.python_package.stamp")

        add_custom_command(
            OUTPUT "${AGENTCORE_PYTHON_PACKAGE_STAMP}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${AGENTCORE_PYTHON_BUILD_ROOT}"
            COMMAND ${CMAKE_COMMAND} -E remove_directory "${AGENTCORE_PYTHON_PACKAGE_ROOT}"
            COMMAND ${CMAKE_COMMAND} -E remove_directory "${AGENTCORE_PYTHON_BUILD_ROOT}/agentcore_langgraph_native"
            COMMAND ${CMAKE_COMMAND} -E remove_directory "${AGENTCORE_PYTHON_BUILD_ROOT}/langgraph"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/python/agentcore"
                "${AGENTCORE_PYTHON_PACKAGE_ROOT}"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/python/agentcore_langgraph_native"
                "${AGENTCORE_PYTHON_BUILD_ROOT}/agentcore_langgraph_native"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/python/langgraph"
                "${AGENTCORE_PYTHON_BUILD_ROOT}/langgraph"
            COMMAND ${CMAKE_COMMAND} -E touch "${AGENTCORE_PYTHON_PACKAGE_STAMP}"
            DEPENDS ${AGENTCORE_PYTHON_PACKAGE_FILES}
        )

        add_custom_target(agentcore_python_package DEPENDS "${AGENTCORE_PYTHON_PACKAGE_STAMP}")

        add_library(_agentcore_native MODULE ${AGENTCORE_PYTHON_SOURCES})
        target_include_directories(
            _agentcore_native
            PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/agentcore/src/bindings/python/core"
        )
        target_link_libraries(_agentcore_native PRIVATE agentcore::agentcore Python3::Module)
        target_compile_features(_agentcore_native PRIVATE cxx_std_20)
        set_target_properties(
            _agentcore_native
            PROPERTIES
                POSITION_INDEPENDENT_CODE ON
                PREFIX ""
                OUTPUT_NAME "_agentcore_native"
                LIBRARY_OUTPUT_DIRECTORY "${AGENTCORE_PYTHON_PACKAGE_ROOT}"
        )
        add_dependencies(_agentcore_native agentcore_python_package)

        if(AGENTCORE_BUILD_BENCHMARKS)
            add_custom_target(
                agentcore_python_state_graph_benchmark
                COMMAND
                    ${CMAKE_COMMAND} -E env
                    "PYTHONPATH=${AGENTCORE_PYTHON_BUILD_ROOT}"
                    ${Python3_EXECUTABLE}
                    "${CMAKE_CURRENT_SOURCE_DIR}/python/benchmarks/state_graph_api_benchmark.py"
                DEPENDS _agentcore_native
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            )
        endif()
    else()
        message(STATUS "Skipping agentcore python bindings: Python3 interpreter/development module not found.")
    endif()
endif()

if(BUILD_TESTING)
    add_executable(agentcore_graph_tests agentcore/tests/graph_tests.cpp)
    target_link_libraries(agentcore_graph_tests PRIVATE agentcore_graph)
    agentcore_enable_validation_asserts(agentcore_graph_tests)
    add_test(NAME agentcore_graph_tests COMMAND agentcore_graph_tests)

    add_executable(agentcore_state_tests agentcore/tests/state_tests.cpp)
    target_link_libraries(agentcore_state_tests PRIVATE agentcore_state)
    agentcore_enable_validation_asserts(agentcore_state_tests)
    add_test(NAME agentcore_state_tests COMMAND agentcore_state_tests)

    add_executable(agentcore_runtime_module_tests agentcore/tests/runtime_module_tests.cpp)
    target_link_libraries(agentcore_runtime_module_tests PRIVATE agentcore_runtime agentcore_state)
    agentcore_enable_validation_asserts(agentcore_runtime_module_tests)
    add_test(NAME agentcore_runtime_module_tests COMMAND agentcore_runtime_module_tests)

    add_executable(agentcore_adapter_tests agentcore/tests/adapter_tests.cpp)
    target_link_libraries(agentcore_adapter_tests PRIVATE agentcore_adapters agentcore_state)
    agentcore_enable_validation_asserts(agentcore_adapter_tests)
    add_test(NAME agentcore_adapter_tests COMMAND agentcore_adapter_tests)

    add_executable(agentcore_execution_module_tests agentcore/tests/execution_module_tests.cpp)
    target_link_libraries(agentcore_execution_module_tests PRIVATE agentcore_execution agentcore_adapters)
    agentcore_enable_validation_asserts(agentcore_execution_module_tests)
    add_test(NAME agentcore_execution_module_tests COMMAND agentcore_execution_module_tests)

    add_executable(agentcore_control_flow_tests agentcore/tests/control_flow_tests.cpp)
    target_link_libraries(agentcore_control_flow_tests PRIVATE agentcore_execution)
    agentcore_enable_validation_asserts(agentcore_control_flow_tests)
    add_test(NAME agentcore_control_flow_tests COMMAND agentcore_control_flow_tests)

    add_executable(agentcore_runtime_tests agentcore/tests/runtime_tests.cpp)
    target_link_libraries(agentcore_runtime_tests PRIVATE agentcore::agentcore)
    agentcore_enable_validation_asserts(agentcore_runtime_tests)
    add_test(NAME agentcore_runtime_tests COMMAND agentcore_runtime_tests)

    add_executable(agentcore_subgraph_session_tests agentcore/tests/subgraph_session_tests.cpp)
    target_link_libraries(agentcore_subgraph_session_tests PRIVATE agentcore::agentcore)
    agentcore_enable_validation_asserts(agentcore_subgraph_session_tests)
    add_test(NAME agentcore_subgraph_session_tests COMMAND agentcore_subgraph_session_tests)

    if(TARGET runtime_proof_suite)
        add_test(NAME runtime_proof_suite COMMAND runtime_proof_suite)
    endif()

    if(TARGET _agentcore_native)
        add_test(
            NAME agentcore_python_state_graph_smoke
            COMMAND
                ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/python/tests/state_graph_api_smoke.py"
        )
        set_tests_properties(
            agentcore_python_state_graph_smoke
            PROPERTIES
                ENVIRONMENT "PYTHONPATH=${AGENTCORE_PYTHON_BUILD_ROOT}"
        )

        add_test(
            NAME agentcore_python_agent_workflows_smoke
            COMMAND
                ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/python/tests/agent_workflows_smoke.py"
        )
        set_tests_properties(
            agentcore_python_agent_workflows_smoke
            PROPERTIES
                ENVIRONMENT "PYTHONPATH=${AGENTCORE_PYTHON_BUILD_ROOT}"
        )

        add_test(
            NAME agentcore_python_patterns_smoke
            COMMAND
                ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/python/tests/patterns_smoke.py"
        )
        set_tests_properties(
            agentcore_python_patterns_smoke
            PROPERTIES
                ENVIRONMENT "PYTHONPATH=${AGENTCORE_PYTHON_BUILD_ROOT}"
        )

        add_test(
            NAME agentcore_python_adapters_runtime_smoke
            COMMAND
                ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/python/tests/adapters_runtime_smoke.py"
        )
        set_tests_properties(
            agentcore_python_adapters_runtime_smoke
            PROPERTIES
                ENVIRONMENT "PYTHONPATH=${AGENTCORE_PYTHON_BUILD_ROOT}"
        )

        add_test(
            NAME agentcore_python_mcp_runtime_smoke
            COMMAND
                ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/python/tests/mcp_runtime_smoke.py"
        )
        set_tests_properties(
            agentcore_python_mcp_runtime_smoke
            PROPERTIES
                ENVIRONMENT "PYTHONPATH=${AGENTCORE_PYTHON_BUILD_ROOT}"
        )

        add_test(
            NAME agentcore_python_mcp_launcher_smoke
            COMMAND
                ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/python/tests/mcp_launcher_smoke.py"
        )
        set_tests_properties(
            agentcore_python_mcp_launcher_smoke
            PROPERTIES
                ENVIRONMENT "PYTHONPATH=${AGENTCORE_PYTHON_BUILD_ROOT}"
        )

        add_test(
            NAME agentcore_python_opentelemetry_smoke
            COMMAND
                ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/python/tests/opentelemetry_smoke.py"
        )
        set_tests_properties(
            agentcore_python_opentelemetry_smoke
            PROPERTIES
                ENVIRONMENT "PYTHONPATH=${AGENTCORE_PYTHON_BUILD_ROOT}"
        )

        add_test(
            NAME agentcore_python_context_state_smoke
            COMMAND
                ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/python/tests/context_state_smoke.py"
        )
        set_tests_properties(
            agentcore_python_context_state_smoke
            PROPERTIES
                ENVIRONMENT "PYTHONPATH=${AGENTCORE_PYTHON_BUILD_ROOT}"
        )

        add_test(
            NAME agentcore_python_real_world_pipeline
            COMMAND
                ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/python/tests/real_world_pipeline_test.py"
        )
        set_tests_properties(
            agentcore_python_real_world_pipeline
            PROPERTIES
                ENVIRONMENT "PYTHONPATH=${AGENTCORE_PYTHON_BUILD_ROOT}"
        )

        add_test(
            NAME agentcore_python_graph_store_pipeline
            COMMAND
                ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/python/tests/graph_store_pipeline_test.py"
        )
        set_tests_properties(
            agentcore_python_graph_store_pipeline
            PROPERTIES
                ENVIRONMENT "PYTHONPATH=${AGENTCORE_PYTHON_BUILD_ROOT}"
        )
    endif()
endif()

set(AGENTCORE_INSTALL_NATIVE_DEV_ARTIFACTS ON)
if(DEFINED SKBUILD_PLATLIB_DIR AND NOT SKBUILD_PLATLIB_DIR STREQUAL "")
    set(AGENTCORE_INSTALL_NATIVE_DEV_ARTIFACTS OFF)
endif()

if(AGENTCORE_INSTALL_NATIVE_DEV_ARTIFACTS)
    install(
        TARGETS
            agentcore
            agentcore_graph
            agentcore_state
            agentcore_runtime
            agentcore_execution
            agentcore_adapters
        EXPORT agentcoreTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(
        DIRECTORY agentcore/include/agentcore
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/agentcoreConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/agentcoreConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/agentcore"
    )

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/agentcoreConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(
        EXPORT agentcoreTargets
        FILE agentcoreTargets.cmake
        NAMESPACE agentcore::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/agentcore"
    )

    install(
        FILES
            "${CMAKE_CURRENT_BINARY_DIR}/agentcoreConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/agentcoreConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/agentcore"
    )
endif()

if(TARGET _agentcore_native)
    if(DEFINED SKBUILD_PLATLIB_DIR AND NOT SKBUILD_PLATLIB_DIR STREQUAL "")
        install(
            TARGETS _agentcore_native
            LIBRARY DESTINATION "${SKBUILD_PLATLIB_DIR}/agentcore"
        )
    elseif(DEFINED Python3_SITELIB AND NOT Python3_SITELIB STREQUAL "")
        install(
            TARGETS _agentcore_native
            LIBRARY DESTINATION "${Python3_SITELIB}/agentcore"
        )
        install(
            DIRECTORY python/agentcore/
            DESTINATION "${Python3_SITELIB}/agentcore"
        )
        install(
            DIRECTORY python/agentcore_langgraph_native/
            DESTINATION "${Python3_SITELIB}/agentcore_langgraph_native"
        )
    endif()
endif()
