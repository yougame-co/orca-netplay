# libjuice (https://github.com/paullouisageneau/libjuice, MPL-2.0): ICE with STUN and TURN for
# Orca's direct links between players (Core/Orca/Session/DirectLink.cpp). Built here as a static
# library from its sources, without its server, tests, shared library or install rules.
# A networking fix for Windows, targeting upstream b89c792e3612faf2f12cf35bcc56857313a06be3.
set(LIBJUICE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libjuice)
option(ORCA_JUICE_UNPATCHED "Test-only: disable a networking fix for Windows" OFF)
if(ORCA_JUICE_UNPATCHED AND NOT ENABLE_TESTS)
  message(FATAL_ERROR "ORCA_JUICE_UNPATCHED requires ENABLE_TESTS")
endif()

function(juice_replace_once source_var anchor replacement)
  string(FIND "${${source_var}}" "${anchor}" first)
  if(first EQUAL -1)
    message(FATAL_ERROR "libjuice ${JUICE_PATCH_FILE}: networking fix anchor missing")
  endif()
  string(LENGTH "${anchor}" anchor_length)
  math(EXPR after "${first} + ${anchor_length}")
  string(SUBSTRING "${${source_var}}" ${after} -1 remainder)
  string(FIND "${remainder}" "${anchor}" second)
  if(NOT second EQUAL -1)
    message(FATAL_ERROR "libjuice ${JUICE_PATCH_FILE}: networking fix anchor is not unique")
  endif()
  string(REPLACE "${anchor}" "${replacement}" patched "${${source_var}}")
  set(${source_var} "${patched}" PARENT_SCOPE)
endfunction()

foreach(JUICE_PATCH_FILE conn_poll conn_thread conn_mux)
  set(upstream "${LIBJUICE_DIR}/src/${JUICE_PATCH_FILE}.c")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${upstream}")
  file(READ "${upstream}" source)
  string(REPLACE "\r\n" "\n" source "${source}")
  if(NOT ORCA_JUICE_UNPATCHED AND JUICE_PATCH_FILE STREQUAL "conn_poll")
    juice_replace_once(source [=[			if ((ret = conn_poll_recv_udp(conn_impl->udp_sock, buffer, BUFFER_SIZE,
							&src)) <= 0) {
				break;
			}]=] [=[			ret = conn_poll_recv_udp(conn_impl->udp_sock, buffer, BUFFER_SIZE, &src);
			if (ret == -SEMSGSIZE)
				continue;
			if (ret <= 0)
				break;]=])
    juice_replace_once(source [=[		if (ret > 0) {
			// There are more datagrams but we need to give other sockets a turn
			JLOG_VERBOSE("Fairness limit reached, will continue on next poll");
		} else if (ret == -SEAGAIN || ret == -SEWOULDBLOCK) {
			JLOG_VERBOSE("No more datagrams to receive");]=]
      [=[		if (ret > 0 || ret == -SEMSGSIZE) {
			// There are more datagrams but we need to give other sockets a turn
			JLOG_VERBOSE("Fairness limit reached, will continue on next poll");
		} else if (ret == -SEAGAIN || ret == -SEWOULDBLOCK) {
			JLOG_VERBOSE("No more datagrams to receive");]=])
    juice_replace_once(source [=[	if (len < 0) {
		if (sockerrno != SEAGAIN && sockerrno != SEWOULDBLOCK)]=]
      [=[	if (len < 0) {
		if (sockerrno == SEMSGSIZE) {
			JLOG_VERBOSE("Dropping oversized datagram");
			return -SEMSGSIZE;
		}
		if (sockerrno != SEAGAIN && sockerrno != SEWOULDBLOCK)]=])
  elseif(NOT ORCA_JUICE_UNPATCHED)
    juice_replace_once(source [=[		JLOG_ERROR("recvfrom failed, errno=%d", sockerrno);
		return -1;]=] [=[		if (sockerrno == SEMSGSIZE) {
			JLOG_VERBOSE("Dropping oversized datagram");
			return -SEMSGSIZE;
		}
		JLOG_ERROR("recvfrom failed, errno=%d", sockerrno);
		return -1;]=])
    if(JUICE_PATCH_FILE STREQUAL "conn_thread")
      juice_replace_once(source
        [=[		while ((ret = conn_thread_recv(conn_impl->sock, buffer, BUFFER_SIZE, &src)) > 0) {]=]
        [=[		while ((ret = conn_thread_recv(conn_impl->sock, buffer, BUFFER_SIZE, &src)) > 0 ||
		       ret == -SEMSGSIZE) {
			if (ret == -SEMSGSIZE)
				continue;]=])
    else()
      juice_replace_once(source
        [=[			if ((ret = conn_mux_recv(registry, buffer, BUFFER_SIZE, &src)) <= 0) {
				break;
			}]=] [=[			ret = conn_mux_recv(registry, buffer, BUFFER_SIZE, &src);
			if (ret == -SEMSGSIZE)
				continue;
			if (ret <= 0)
				break;]=])
      juice_replace_once(source [=[		if (ret < 0) {
			conn_mux_fail(registry);]=] [=[		if (ret < 0 && ret != -SEMSGSIZE) {
			conn_mux_fail(registry);]=])
    endif()
  endif()
  file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${JUICE_PATCH_FILE}.c"
    CONTENT "${source}" @ONLY)
endforeach()

add_library(juice STATIC
  ${LIBJUICE_DIR}/src/addr.c
  ${LIBJUICE_DIR}/src/agent.c
  ${LIBJUICE_DIR}/src/base64.c
  ${LIBJUICE_DIR}/src/conn.c
  ${CMAKE_CURRENT_BINARY_DIR}/conn_mux.c
  ${CMAKE_CURRENT_BINARY_DIR}/conn_poll.c
  ${CMAKE_CURRENT_BINARY_DIR}/conn_thread.c
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
