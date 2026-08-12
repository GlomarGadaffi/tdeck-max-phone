# Host-side unit tests -- deliberately a STANDALONE CMake project, not an
# ESP-IDF component. These run on the dev machine with no board and no
# ESP-IDF toolchain, so wire-format regressions are catchable in seconds.
#
# components/sip_core is portable by construction (it #ifdefs between
# lwip/netinet/WinSock2 for sockaddr_in), which is what makes this possible.
#
# Usage:
#   cmake -B build -S test
#   cmake --build build
#   ctest --test-dir build --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(tdeck_max_phone_host_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SIP_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../components/sip_core)

add_executable(sip_wire_test
    sip_wire_test.cpp
    ${SIP_CORE_DIR}/SipMessage.cpp
    ${SIP_CORE_DIR}/SipMessageFactory.cpp
    ${SIP_CORE_DIR}/SipSdpMessage.cpp
)
target_include_directories(sip_wire_test PRIVATE ${SIP_CORE_DIR})

if(WIN32)
    target_link_libraries(sip_wire_test PRIVATE ws2_32)
endif()

enable_testing()
add_test(NAME sip_wire_format COMMAND sip_wire_test)
