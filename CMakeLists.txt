cmake_minimum_required(VERSION 3.30)
project(queue_priority VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(GRAFT_BUILD_TESTS OFF)
set(GRAFT_BUILD_EXAMPLES OFF)
set(QUEUE_PRIORITY_GRAFT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../KR_GRAFTED" CACHE PATH "KR_GRAFTED v0.2.0 source")

include("${QUEUE_PRIORITY_GRAFT_DIR}/cmake/graft.boot.cmake")
graft_import(graft "${QUEUE_PRIORITY_GRAFT_DIR}")

graft_plugin(queue_priority
    NAME    QueuePriority
    VERSION 1
    SOURCES src/plugin.cpp)
target_compile_definitions(queue_priority PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
