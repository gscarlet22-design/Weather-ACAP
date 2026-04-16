cmake_minimum_required(VERSION 3.10)
project(weather_acap C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB    REQUIRED glib-2.0)
pkg_check_modules(AXPARAM REQUIRED axparameter)
pkg_check_modules(CURL    REQUIRED libcurl)

# ── Shared utility sources ────────────────────────────────────────────────────
set(UTIL_SRCS
    cJSON.c
    params.c
    vapix.c
)

# ── Main daemon ───────────────────────────────────────────────────────────────
add_executable(weather_acap
    ${UTIL_SRCS}
    nws.c
    openmeteo.c
    weather_api.c
    alerts.c
    overlay.c
    history.c
    webhook.c
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

# ── Config CGI binary (web UI backend) ───────────────────────────────────────
# Output binary is named "weather_acap" (matching httpConfig.name in manifest)
# but CMake target is "weather_acap_cgi" to avoid collision with the daemon.
add_executable(weather_acap_cgi
    ${UTIL_SRCS}
    nws.c
    openmeteo.c
    weather_api.c
    alerts.c
    overlay.c
    webhook.c
    config_cgi.c
)
set_target_properties(weather_acap_cgi PROPERTIES OUTPUT_NAME weather_acap)

target_include_directories(weather_acap_cgi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${GLIB_INCLUDE_DIRS}
    ${AXPARAM_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(weather_acap_cgi PRIVATE
    ${GLIB_LIBRARIES}
    ${AXPARAM_LIBRARIES}
    ${CURL_LIBRARIES}
    m
)

target_compile_options(weather_acap_cgi PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
)

# ── Install rules ─────────────────────────────────────────────────────────────
# acap-build reads the install staging directory to build the .eap
install(TARGETS weather_acap     DESTINATION .)
install(TARGETS weather_acap_cgi DESTINATION html)
install(FILES   html/index.html html/style.css html/app.js DESTINATION html)
