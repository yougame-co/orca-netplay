# libjuice (https://github.com/paullouisageneau/libjuice, MPL-2.0): ICE with STUN and TURN for
# Orca's direct links between players (Core/Orca/Session/DirectLink.cpp). Built here as a static
# library from its sources, without its server, tests, shared library or install rules.
set(LIBJUICE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libjuice)

add_library(juice STATIC
  ${LIBJUICE_DIR}/src/addr.c
  ${LIBJUICE_DIR}/src/agent.c
  ${LIBJUICE_DIR}/src/base64.c
  ${LIBJUICE_DIR}/src/conn.c
  ${LIBJUICE_DIR}/src/conn_mux.c
  ${LIBJUICE_DIR}/src/conn_poll.c
  ${LIBJUICE_DIR}/src/conn_thread.c
  ${LIBJUICE_DIR}/src/const_time.c
  ${LIBJUICE_DIR}/src/crc32.c
  ${LIBJUICE_DIR}/src/hash.c
  ${LIBJUICE_DIR}/src/hmac.c
  ${LIBJUICE_DIR}/src/ice.c
  ${LIBJUICE_DIR}/src/juice.c
  ${LIBJUICE_DIR}/src/log.c
  ${LIBJUICE_DIR}/src/random.c
  ${LIBJUICE_DIR}/src/server.c
  ${LIBJUICE_DIR}/src/stun.c
  ${LIBJUICE_DIR}/src/tcp.c
  ${LIBJUICE_DIR}/src/timestamp.c
  ${LIBJUICE_DIR}/src/turn.c
  ${LIBJUICE_DIR}/src/udp.c
)
set_target_properties(juice PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
target_include_directories(juice
  PUBLIC ${LIBJUICE_DIR}/include
  PRIVATE ${LIBJUICE_DIR}/include/juice ${LIBJUICE_DIR}/src
)
# RELEASE=1 leaves source file names and lines out of its log lines (they go to Dolphin's log).
target_compile_definitions(juice
  PUBLIC JUICE_STATIC
  PRIVATE JUICE_EXPORTS NO_SERVER USE_NETTLE=0 RELEASE=1
)
find_package(Threads REQUIRED)
target_link_libraries(juice PRIVATE Threads::Threads)
if(WIN32)
  target_compile_definitions(juice PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(juice PRIVATE ws2_32 bcrypt)
endif()
if(MSVC)
  target_compile_options(juice PRIVATE /utf-8)
endif()
dolphin_disable_warnings(juice)
add_library(LibJuice::LibJuice ALIAS juice)
