cmake_minimum_required(VERSION 3.10)
project(weather_acap C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB    REQUIRED glib-2.0)
pkg_check_modules(CURL    REQUIRED libcurl)
pkg_check_modules(FCGI    REQUIRED fcgi)
# axparameter intentionally not required — daemon uses file-based storage
# (localdata/params.json) since AXIS OS 12 sandbox rejects ax_parameter_set.

# ── Shared utility sources (used by both daemon and CGI) ─────────────────
set(SHARED_SRCS
    cJSON.c
    vapix.c
    nws.c
    openmeteo.c
    weather_api.c
    alerts.c
    overlay.c
    webhook.c
    snapshot.c
)

# ── Main daemon ──────────────────────────────────────────────────────────
add_executable(weather_acap
    ${SHARED_SRCS}
    params.c
    history.c
    weather_acap.c
)

target_include_directories(weather_acap PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${GLIB_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(weather_acap PRIVATE
    ${GLIB_LIBRARIES}
    ${CURL_LIBRARIES}
    m
)

target_compile_options(weather_acap PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
)

# ── Config CGI binary (web UI backend, FastCGI) ──────────────────────────
# The CGI is spawned as a child by the main daemon so it inherits the
# FCGI_SOCKET_NAME env var from the ACAP runtime.  It does not link
# axparameter or glib (config exchange is file-based — daemon writes JSON,
# CGI reads it) but does link libcurl so the Diagnostics tab's in-process
# VAPIX / weather / webhook test endpoints can make real HTTP calls.
add_executable(weather_acap_cgi
    ${SHARED_SRCS}
    config_cgi.c
)
set_target_properties(weather_acap_cgi PROPERTIES OUTPUT_NAME weather_acap.cgi)

target_include_directories(weather_acap_cgi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FCGI_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(weather_acap_cgi PRIVATE
    ${FCGI_LIBRARIES}
    ${CURL_LIBRARIES}
    m
)

target_compile_options(weather_acap_cgi PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
)

# ── Install rules ────────────────────────────────────────────────────────
install(TARGETS weather_acap     DESTINATION .)
# CGI binary at package root (NOT html/) — Apache's CGI handler resolves
# /local/weather_acap/weather_acap.cgi to <package_root>/weather_acap.cgi.
install(TARGETS weather_acap_cgi DESTINATION .)
install(FILES   html/index.html html/style.css html/app.js DESTINATION html)
