SET(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR})

# Configure test paths
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/test_paths.h.in" "${CMAKE_BINARY_DIR}/test/test_paths.h")

INCLUDE_DIRECTORIES(
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_BINARY_DIR}/test
)

ADD_EXECUTABLE(load_test load_test.cpp)
ADD_EXECUTABLE(benchmark_bin benchmark.cpp)
if(WIN32)
    target_link_libraries(benchmark_bin Psapi)
endif()
set_target_properties(benchmark_bin PROPERTIES OUTPUT_NAME benchmark_bin)
ADD_CUSTOM_TARGET(benchmark
    COMMAND $<TARGET_FILE:benchmark_bin>
    DEPENDS benchmark_bin
    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
    USES_TERMINAL)
ADD_SUBDIRECTORY(unittest)

TARGET_LINK_LIBRARIES(load_test cppjieba)
TARGET_LINK_LIBRARIES(benchmark_bin cppjieba)
find_package(Threads REQUIRED)
ADD_EXECUTABLE(cpu_differential cpu_differential.cpp)
TARGET_LINK_LIBRARIES(cpu_differential cppjieba Threads::Threads)
ADD_TEST(NAME cpu_differential COMMAND cpu_differential ${PROJECT_SOURCE_DIR})
ADD_EXECUTABLE(cpu_benchmark cpu_benchmark.cpp)
TARGET_LINK_LIBRARIES(cpu_benchmark cppjieba Threads::Threads)
ADD_EXECUTABLE(cpu_diagnostics cpu_benchmark.cpp)
TARGET_COMPILE_DEFINITIONS(cpu_diagnostics PRIVATE CPPJIEBA_CPU_DIAGNOSTICS)
TARGET_LINK_LIBRARIES(cpu_diagnostics cppjieba Threads::Threads)
if(WIN32)
  TARGET_LINK_LIBRARIES(cpu_benchmark Psapi)
  TARGET_LINK_LIBRARIES(cpu_diagnostics Psapi)
endif()
