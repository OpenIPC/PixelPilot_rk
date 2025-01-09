cmake_minimum_required(VERSION 3.1)

project(pixelpilot VERSION 1.2.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options("-Wno-address-of-packed-member")

set(SOURCE_FILES 
        src/drm.h
        src/drm.c
        src/osd.h
        src/osd.hpp
        src/osd.cpp
        src/dvr.h
        src/dvr.cpp
        src/rtp.h
        src/rtp.c
        src/mavlink.h
        src/mavlink.c
        src/main.cpp
        src/main.h
        src/scheduling_helper.hpp
        src/gstrtpreceiver.cpp
        src/gstrtpreceiver.h
        src/json_stats.h
        src/json_stats.cpp)
file(GLOB ICONS src/icons/*.png)
file(GLOB OSD_CONFIGS *_osd.json)

include_directories("/usr/include/libdrm" "/usr/include/cairo" "/usr/include/spdlog")

configure_file("${PROJECT_NAME}_config.h.in" "${PROJECT_NAME}_config.h")

find_package(spdlog REQUIRED)
find_package(nlohmann_json 3 REQUIRED)

add_executable(${PROJECT_NAME} ${SOURCE_FILES})
target_link_libraries(${PROJECT_NAME} rockchip_mpp pthread drm m cairo spdlog::spdlog nlohmann_json::nlohmann_json)

# Embed gstreamer
find_package(PkgConfig REQUIRED)
pkg_search_module(GST REQUIRED
        gstreamer-1.0>=1.4
        gstreamer-app-1.0>=1.4
)
pkg_search_module(gstreamer REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.4)
pkg_search_module(gstreamer-app REQUIRED IMPORTED_TARGET gstreamer-app-1.0>=1.4)
target_link_libraries(${PROJECT_NAME} PkgConfig::gstreamer PkgConfig::gstreamer-app)

if(CMAKE_BUILD_TYPE MATCHES "Debug")
  add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
  set(
    CMAKE_C_FLAGS
    "${CMAKE_C_FLAGS} -Werror -fsanitize=undefined -fsanitize=address"
  )
  target_link_options(${PROJECT_NAME}
    BEFORE PUBLIC -fsanitize=undefined PUBLIC -fsanitize=address
  )
endif()

target_include_directories(${PROJECT_NAME} PUBLIC "${PROJECT_BINARY_DIR}")

include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES ${ICONS}
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/${PROJECT_NAME}
)
install(FILES ${OSD_CONFIGS}
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/${PROJECT_NAME}
)
