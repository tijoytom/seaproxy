cmake_minimum_required(VERSION 3.20)

project(seaproxy VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

include(FetchContent)
set(TOMLPLUSPLUS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TOMLPLUSPLUS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(tomlplusplus)

find_package(Seastar REQUIRED)

add_executable(seaproxy
    src/cluster.cc
    src/config.cc
    src/main.cc
    src/proxy.cc
    src/resp.cc
)
target_include_directories(seaproxy PRIVATE src)
target_link_libraries(seaproxy PRIVATE
    Seastar::seastar
    tomlplusplus::tomlplusplus
)
target_compile_options(seaproxy PRIVATE -Wall -Wextra -Wpedantic)

include(GNUInstallDirs)
install(TARGETS seaproxy RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(
    FILES
        README.md
        LICENSE
        seaproxy.toml.example
        src/config.toml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/seaproxy
)

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

    add_executable(config_tests
        tests/config_test.cc
        src/config.cc
    )
    target_include_directories(config_tests PRIVATE src)
    target_link_libraries(config_tests PRIVATE
        tomlplusplus::tomlplusplus
    )
    target_compile_options(config_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(
        NAME config_tests
        COMMAND config_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/src/config.toml
    )
endif()

set(CPACK_PACKAGE_NAME "seaproxy")
set(CPACK_PACKAGE_VENDOR "SeaProxy contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Shard-per-core multiplexing proxy for Redis"
)
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME
    "seaproxy-${PROJECT_VERSION}-ubuntu24.04-amd64"
)
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES TRUE)
set(CPACK_GENERATOR "DEB;TGZ")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "SeaProxy contributors")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "ca-certificates, libboost-program-options1.83.0, libboost-thread1.83.0, libc-ares2, libfmt9, libgnutls30t64, libhwloc15, liburing2, libyaml-cpp0.8"
)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
include(CPack)
