set(BUILD_CURL_EXE OFF)
set(BUILD_EXAMPLES OFF)
set(BUILD_LIBCURL_DOCS OFF)
set(BUILD_MISC_DOCS OFF)
set(BUILD_SHARED_LIBS OFF)
set(BUILD_STATIC_LIBS ON)
set(BUILD_TESTING OFF)
set(CURL_ENABLE_EXPORT_TARGET OFF)
set(ENABLE_CURL_MANUAL OFF)

set(HTTP_ONLY ON)

set(CURL_USE_LIBPSL OFF)
set(CURL_USE_LIBSSH2 OFF)
set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
set(ZLIB_FOUND FALSE) # TODO: Remove once we update to cURL >=8.17.1 https://github.com/curl/curl/pull/20147
set(ZSTD_FOUND FALSE) # TODO: Same as above
set(BROTLI_FOUND FALSE) # TODO: Same as above
set(USE_LIBIDN2 OFF)
set(USE_NGHTTP2 OFF)

if(UNIX)
  # We use mbedtls on Unix(-like) systems and Android.
  set(CURL_USE_OPENSSL OFF)

  # This is set if the dolphin_find_optional_system_library call from the main CMakeLists.txt
  # is able to find mbedtls on the system.
  if(MBEDTLS_FOUND)
    # We can just enable CURL_USE_MBEDTLS.
    set(CURL_USE_MBEDTLS ON)
  else()
    # HACK: Set some internal variables to pretend like mbedtls was found on the system.
    # We can't use CURL_USE_MBEDTLS with our copy from Externals, as that flag expects
    # mbedtls to be installed (the CMakeLists attempts to search for it with find_package).
    set(_ssl_enabled ON)
    set(USE_MBEDTLS ON)
    set(_curl_ca_bundle_supported TRUE)
  endif()
endif()

if(WIN32)
  set(CURL_USE_SCHANNEL ON)
endif()

if(ANDROID)
  set(CURL_CA_PATH "/system/etc/security/cacerts" CACHE STRING "")
endif()

add_subdirectory(curl)

# Let WebSocket::Connect read HTTP refusal bodies on the original request. curl otherwise
# aborts at the headers; the client still requires a 101 before using the WebSocket API.
# Compile a local copy so the vendored submodule stays unchanged.
set(orca_http_source "${CMAKE_CURRENT_SOURCE_DIR}/curl/lib/http.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${orca_http_source}")
file(READ "${orca_http_source}" orca_http)
set(orca_ws_check "if(data->req.upgr101 == UPGR101_WS) {")
string(FIND "${orca_http}" "${orca_ws_check}" orca_ws_check_at)
if(orca_ws_check_at EQUAL -1)
  message(FATAL_ERROR "curl WebSocket refusal handling changed; review the local body adjustment")
endif()
string(REPLACE "${orca_ws_check}"
  "if(data->req.upgr101 == UPGR101_WS && k->httpcode < 400) {" orca_http "${orca_http}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/orca-http.c" "${orca_http}")
if(TARGET libcurl_object)
  set(orca_curl_target libcurl_object)
else()
  set(orca_curl_target libcurl_static)
endif()
get_target_property(orca_curl_sources ${orca_curl_target} SOURCES)
list(FILTER orca_curl_sources EXCLUDE REGEX "(^|/)http\\.c$")
list(APPEND orca_curl_sources "${CMAKE_CURRENT_BINARY_DIR}/orca-http.c")
set_property(TARGET ${orca_curl_target} PROPERTY SOURCES "${orca_curl_sources}")
target_include_directories(${orca_curl_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/curl/lib")

if(UNIX AND NOT MBEDTLS_FOUND)
  # HACK: Manually link with the mbedtls libraries.
  target_link_libraries(libcurl_static PRIVATE
    MbedTLS::mbedtls
    MbedTLS::mbedx509)
endif()
