cmake_minimum_required(VERSION 3.20)

project(seaproxy VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

find_package(Seastar REQUIRED)

add_executable(seaproxy
    src/cluster.cc
    src/main.cc
    src/proxy.cc
    src/resp.cc
)
target_include_directories(seaproxy PRIVATE src)
target_link_libraries(seaproxy PRIVATE Seastar::seastar)
target_compile_options(seaproxy PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
    add_executable(resp_tests
        tests/resp_test.cc
        src/cluster.cc
        src/resp.cc
    )
    target_include_directories(resp_tests PRIVATE src)
    target_compile_options(resp_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME resp_tests COMMAND resp_tests)
endif()
