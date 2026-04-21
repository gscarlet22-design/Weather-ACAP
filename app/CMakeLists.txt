cmake_minimum_required(VERSION 3.10)
project(weather_acap C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB    REQUIRED glib-2.0)
pkg_check_modules(AXPARAM REQUIRED axparameter)
pkg_check_modules(CURL    REQUIRED libcurl)
pkg_check_modules(FCGI    REQUIRED fcgi)

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
    ${AXPARAM_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(weather_acap PRIVATE
    ${GLIB_LIBRARIES}
    ${AXPARAM_LIBRARIES}
    ${CURL_LIBRARIES}
    m
)

target_compile_options(weather_acap PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
)

# ── Config CGI binary (web UI backend) ───────────────────────────────────
# The CGI runs under Apache's per-ACAP suexec context, NOT the ACAP runtime,
# so it must NOT link against libaxparameter, libglib, or libcurl — any
# missing library at exec time causes the dynamic linker to fail before main()
# and Apache returns HTTP 500.  CGI_NO_CURL guards all curl code at compile
# time; only libm is needed.  Config exchange is file-based: daemon writes
# JSON, CGI reads it.
add_executable(weather_acap_cgi
    ${SHARED_SRCS}
    config_cgi.c
)
set_target_properties(weather_acap_cgi PROPERTIES OUTPUT_NAME weather_acap.cgi)

target_include_directories(weather_acap_cgi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FCGI_INCLUDE_DIRS}
)

target_link_libraries(weather_acap_cgi PRIVATE
    ${FCGI_LIBRARIES}
    m
)

target_compile_definitions(weather_acap_cgi PRIVATE CGI_NO_CURL)

target_compile_options(weather_acap_cgi PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
)

# ── Install rules ────────────────────────────────────────────────────────
install(TARGETS weather_acap     DESTINATION .)
# CGI binary at package root (NOT html/) — Apache's CGI handler resolves
# /local/weather_acap/weather_acap.cgi to <package_root>/weather_acap.cgi.
install(TARGETS weather_acap_cgi DESTINATION .)
install(FILES   html/index.html html/style.css html/app.js DESTINATION html)
