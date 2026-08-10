cmake_minimum_required(VERSION 3.16)

project(WhyRun VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "WhyRun v0.1 supports Linux only")
endif()

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(FATAL_ERROR "WhyRun v0.1 currently supports the x86_64 syscall ABI only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SQLite3 REQUIRED)

add_library(whyrun_core
    src/event.cpp
    src/execution_state.cpp
    src/ptrace_collector.cpp
    src/sqlite_store.cpp
    src/show.cpp
    src/diff.cpp
)

target_include_directories(whyrun_core PUBLIC include)
target_link_libraries(whyrun_core PUBLIC SQLite::SQLite3)
target_compile_options(whyrun_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(whyrun src/main.cpp)
target_link_libraries(whyrun PRIVATE whyrun_core)

install(TARGETS whyrun RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    add_executable(test_file tests/test_file.cpp)
    add_executable(test_child tests/test_child.cpp)
    add_executable(test_network tests/test_network.cpp)
    add_executable(test_unix tests/test_unix.cpp)

    add_test(
        NAME integration
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.sh
                $<TARGET_FILE:whyrun>
                $<TARGET_FILE:test_file>
                $<TARGET_FILE:test_child>
                $<TARGET_FILE:test_network>
                $<TARGET_FILE:test_unix>
                ${CMAKE_CURRENT_BINARY_DIR}/integration-work
    )
endif()
