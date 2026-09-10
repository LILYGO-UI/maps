cmake_minimum_required(VERSION 3.21)

include(cmake/LpmMetadata.cmake)
project(${LILYGO_UI_CMAKE_PROJECT_NAME} LANGUAGES C CXX)

include(GNUInstallDirs)
include(CTest)
find_package(LilyGoUI CONFIG REQUIRED)
find_package(Threads REQUIRED)

find_path(MAPS_JSON_INCLUDE_DIR NAMES json-c/json.h)
find_library(MAPS_JSON_LIBRARY NAMES json-c)
if(NOT MAPS_JSON_INCLUDE_DIR OR NOT MAPS_JSON_LIBRARY)
    message(FATAL_ERROR "json-c headers and library are required")
endif()
add_library(MapsJSON::MapsJSON UNKNOWN IMPORTED)
set_target_properties(MapsJSON::MapsJSON PROPERTIES
    IMPORTED_LOCATION "${MAPS_JSON_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${MAPS_JSON_INCLUDE_DIR}")
mark_as_advanced(MAPS_JSON_INCLUDE_DIR MAPS_JSON_LIBRARY)

configure_file(
    cmake/app_identity.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/app_identity.h
    @ONLY)

add_library(lilygo_ui_maps_core STATIC
    src/domain/map_model.cpp
    src/domain/tile_cache.cpp)
target_include_directories(lilygo_ui_maps_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_options(lilygo_ui_maps_core PRIVATE
    -Wall -Wextra -Wpedantic)
target_compile_features(lilygo_ui_maps_core PUBLIC cxx_std_17)

add_library(lilygo_ui_maps_services STATIC
    src/services/http_client.cpp
    src/services/map_service.cpp)
target_include_directories(lilygo_ui_maps_services PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_options(lilygo_ui_maps_services PRIVATE
    -Wall -Wextra -Wpedantic)
target_compile_features(lilygo_ui_maps_services PUBLIC cxx_std_17)
target_link_libraries(lilygo_ui_maps_services PUBLIC
    lilygo_ui_maps_core
    Threads::Threads
    MapsJSON::MapsJSON)

add_library(lilygo_ui_maps_ui STATIC
    src/app.cpp
    src/components/components.cpp
    src/pages/map/map_view.cpp
    src/pages/map/map_view_model.cpp)
target_include_directories(lilygo_ui_maps_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_options(lilygo_ui_maps_ui PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(lilygo_ui_maps_ui PUBLIC
    lilygo_ui_maps_services
    LilyGoUI::Runtime)

add_executable(lilygo_ui_maps src/main.cpp)
target_compile_options(lilygo_ui_maps PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(lilygo_ui_maps PRIVATE lilygo_ui_maps_ui)
set_target_properties(lilygo_ui_maps PROPERTIES
    OUTPUT_NAME "${LILYGO_UI_EXECUTABLE_NAME}")

configure_file(data/launcher.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.json @ONLY)
configure_file(data/application.desktop.in
    ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.desktop @ONLY)
configure_file(data/application.metainfo.xml.in
    ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.metainfo.xml @ONLY)

install(TARGETS lilygo_ui_maps
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT app)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.json
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/launcher/apps
    COMPONENT app)
lilygo_ui_install_launcher_icon(
    "${LILYGO_UI_APP_ID}"
    "${CMAKE_CURRENT_SOURCE_DIR}/${LILYGO_UI_APP_ICON}")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.desktop
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
    COMPONENT app)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo
    COMPONENT app)
lilygo_ui_install_font_licenses(
    "${CMAKE_INSTALL_DATAROOTDIR}/doc/${LILYGO_UI_PACKAGE_NAME}"
    app)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_executable(maps-core-tests tests/test_core.cpp)
    target_compile_options(maps-core-tests PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(maps-core-tests PRIVATE
        lilygo_ui_maps_services)
    add_test(NAME maps-core COMMAND maps-core-tests)

    add_executable(maps-location-tests tests/test_location.cpp)
    target_compile_options(maps-location-tests PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(maps-location-tests PRIVATE
        lilygo_ui_maps_services)
    add_test(NAME maps-location COMMAND maps-location-tests)
    set_tests_properties(maps-location PROPERTIES TIMEOUT 20)

    add_executable(maps-view-model-tests tests/test_view_model.cpp)
    target_compile_options(maps-view-model-tests PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(maps-view-model-tests PRIVATE
        lilygo_ui_maps_ui)
    add_test(NAME maps-view-model COMMAND maps-view-model-tests)

    add_executable(maps-render-ui tests/render_ui.cpp)
    target_compile_options(maps-render-ui PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(maps-render-ui PRIVATE
        lilygo_ui_maps_ui)
    foreach(viewport IN ITEMS portrait landscape)
        if(viewport STREQUAL "portrait")
            set(_width 568)
            set(_height 1232)
        else()
            set(_width 1232)
            set(_height 568)
        endif()
        add_test(NAME maps-render-${viewport} COMMAND maps-render-ui
            ${_width} ${_height}
            ${CMAKE_CURRENT_BINARY_DIR}/maps-${viewport}.ppm)
    endforeach()
endif()

# New applications have no legacy CM0 package to replace. Add Debian migration
# fields only when this project supersedes an actually published package.
set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME "${LILYGO_UI_PACKAGE_NAME}")
set(CPACK_PACKAGE_VERSION "${LILYGO_UI_VERSION}")
if(LILYGO_UI_VERSION MATCHES
   "^([0-9]+:)?([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(CPACK_PACKAGE_VERSION_MAJOR "${CMAKE_MATCH_2}")
    set(CPACK_PACKAGE_VERSION_MINOR "${CMAKE_MATCH_3}")
    set(CPACK_PACKAGE_VERSION_PATCH "${CMAKE_MATCH_4}")
else()
    set(CPACK_PACKAGE_VERSION_MAJOR 0)
    set(CPACK_PACKAGE_VERSION_MINOR 0)
    set(CPACK_PACKAGE_VERSION_PATCH 0)
endif()
set(CPACK_PACKAGE_CONTACT "${LILYGO_UI_PACKAGE_CONTACT}")
set(CPACK_PACKAGE_VENDOR "${LILYGO_UI_PACKAGE_CONTACT}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${LILYGO_UI_APP_SUMMARY}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${LILYGO_UI_APP_HOMEPAGE}")
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_SET_DESTDIR ON)
set(CPACK_COMPONENTS_ALL app)
set(CPACK_DEBIAN_APP_PACKAGE_NAME "${LILYGO_UI_PACKAGE_NAME}")
set(CPACK_DEBIAN_APP_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_APP_PACKAGE_DEPENDS
    "libc6, libstdc++6, libdrm2, libfreetype6, libpng16-16t64, zlib1g, libbz2-1.0, libbrotli1, libjson-c5, curl, ca-certificates, lilygo-ui-appkit-dev (>= ${LILYGO_UI_MIN_APPKIT_VERSION})")
set(CPACK_DEBIAN_APP_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE
        "${LILYGO_UI_PACKAGE_ARCHITECTURE}")
endif()
include(CPack)
