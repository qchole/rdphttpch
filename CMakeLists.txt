cmake_minimum_required(VERSION 3.20)
project(rdpch C)

set(CMAKE_C_STANDARD 11)

if(MSVC)
    add_compile_options(/W4)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
    # static CRT everywhere (no VC runtime dependency for the plugin DLL)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

set(RDPCH_COMMON_SRC
    src/common/protocol.c
    src/common/bufchain.c
    src/common/log.c
    src/common/util.c
    src/common/transport.c
    src/common/sockchan.c
)

add_library(rdpch_common STATIC ${RDPCH_COMMON_SRC})
target_include_directories(rdpch_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/common)
target_link_libraries(rdpch_common PUBLIC ws2_32)

# ---- server (runs inside the RDP session) ----
add_executable(rdpchsrv
    src/srv/main.c
    src/srv/proxy.c
    src/srv/http_rewrite.c
    src/srv/channel_wts.c
    src/srv/channel_mock.c
    src/srv/wininet_cfg.c
    src/srv/conf.c
)
target_include_directories(rdpchsrv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/srv)
target_link_libraries(rdpchsrv PRIVATE rdpch_common wtsapi32 wininet)

# ---- client plugin DLL (loaded by mstsc.exe) ----
add_library(rdpchcli SHARED
    src/cli/entry.c
    src/cli/core.c
    src/cli/transport_vc.c
    src/cli/rdpchcli.def
)
target_include_directories(rdpchcli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/cli)
target_link_libraries(rdpchcli PRIVATE rdpch_common)

# ---- mock client host (runs client core over TCP, for testing) ----
add_executable(rdpchclihost
    src/cli/host_main.c
    src/cli/core.c
    src/cli/transport_tcp.c
)
target_include_directories(rdpchclihost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/cli)
target_link_libraries(rdpchclihost PRIVATE rdpch_common)

# ---- tests ----
enable_testing()
add_executable(test_protocol test/test_protocol.c)
target_link_libraries(test_protocol PRIVATE rdpch_common)
add_test(NAME protocol COMMAND test_protocol)

add_executable(test_http_rewrite test/test_http_rewrite.c src/srv/http_rewrite.c)
target_include_directories(test_http_rewrite PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/srv)
target_link_libraries(test_http_rewrite PRIVATE rdpch_common)
add_test(NAME http_rewrite COMMAND test_http_rewrite)

add_executable(test_conf test/test_conf.c src/srv/conf.c)
target_include_directories(test_conf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/srv)
target_link_libraries(test_conf PRIVATE rdpch_common)
add_test(NAME conf COMMAND test_conf)

add_executable(test_backpressure test/test_backpressure.c src/common/sockchan.c src/srv/channel_mock.c)
target_include_directories(test_backpressure PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/srv)
target_link_libraries(test_backpressure PRIVATE rdpch_common)
add_test(NAME backpressure COMMAND test_backpressure)

# e2e: receiver-side backpressure (large-transfer disconnect family);
# drives the real srv+cli engines over the mock carrier in one process
add_executable(test_flowcontrol test/test_flowcontrol.c
    src/srv/proxy.c src/srv/http_rewrite.c src/srv/channel_mock.c
    src/cli/core.c src/cli/transport_tcp.c
)
target_include_directories(test_flowcontrol PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/srv ${CMAKE_CURRENT_SOURCE_DIR}/src/cli)
target_link_libraries(test_flowcontrol PRIVATE rdpch_common)
add_test(NAME flowcontrol COMMAND test_flowcontrol)

add_executable(test_vc test/test_vc.c src/cli/transport_vc.c)
target_include_directories(test_vc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/cli)
target_link_libraries(test_vc PRIVATE rdpch_common)
add_test(NAME vc COMMAND test_vc)
