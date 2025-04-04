cmake_minimum_required(VERSION 3.1)

project(pixelpilot VERSION 1.2.0)
find_package(PkgConfig REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# LVGL setup
set(LV_USE_LINUX_DRM ON)
set(LV_CONF_BUILD_DISABLE_DEMOS 1)
set(LV_CONF_BUILD_DISABLE_EXAMPLES 1)
add_subdirectory(lvgl)
target_include_directories(lvgl PUBLIC 
    ${PROJECT_SOURCE_DIR}
    /usr/include/libdrm
    /usr/include/drm
)
set(CMAKE_C_STANDARD 99) # LVGL officially supports C99 and above
set(CMAKE_CXX_STANDARD 17) #C17
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Simulator
set(SIMULATOR_SOURCES
        src/simulator.c
        src/gsmenu/executor.h
        src/gsmenu/executor.c
        src/gsmenu/air_telemetry.h
        src/gsmenu/air_telemetry.c
        src/gsmenu/air_camera.h
        src/gsmenu/air_camera.c
        src/gsmenu/air_actions.h
        src/gsmenu/air_actions.c
        src/gsmenu/gs_wifi.h
        src/gsmenu/gs_wifi.c
        src/gsmenu/ui.h
        src/gsmenu/helper.h
        src/gsmenu/helper.c
        src/gsmenu/gs_wfbng.h
        src/gsmenu/gs_wfbng.c
        src/gsmenu/gs_system.h
        src/gsmenu/gs_system.c
        src/gsmenu/gs_actions.h
        src/gsmenu/gs_actions.c        
        src/gsmenu/air_wfbng.h
        src/gsmenu/air_wfbng.c
        src/gsmenu/ui.c
        src/gsmenu/images.h
        src/gsmenu/images.c
        src/gsmenu/styles.h
        src/gsmenu/styles.c
        src/lvosd.h
        src/lvosd.c
        src/menu.h
        src/menu.c
        src/input.h
        src/input.c
)
# LVGL setup

add_compile_options("-Wno-address-of-packed-member")

set(SOURCE_FILES 
        src/drm.h
        src/drm.c
        src/gsmenu/executor.h
        src/gsmenu/executor.c
        src/gsmenu/air_telemetry.c
        src/gsmenu/air_camera.h
        src/gsmenu/air_camera.c
        src/gsmenu/air_actions.h
        src/gsmenu/air_actions.c
        src/gsmenu/gs_wifi.h
        src/gsmenu/gs_wifi.c
        src/gsmenu/ui.h
        src/gsmenu/helper.h
        src/gsmenu/helper.c
        src/gsmenu/gs_wfbng.h
        src/gsmenu/gs_wfbng.c
        src/gsmenu/gs_system.h
        src/gsmenu/gs_system.c
        src/gsmenu/air_wfbng.h
        src/gsmenu/air_wfbng.c
        src/gsmenu/gs_actions.h
        src/gsmenu/gs_actions.c        
        src/gsmenu/images.h
        src/gsmenu/images.c        
        src/gsmenu/styles.h
        src/gsmenu/styles.c
        src/gsmenu/ui.c 
        src/lvosd.h
        src/lvosd.c
        src/menu.h
        src/menu.c
        src/input.h
        src/input.c
        src/osd.h
        src/osd.hpp
        src/osd.cpp
        src/dvr.h
        src/dvr.cpp
        src/mavlink.h
        src/mavlink.c
        src/main.cpp
        src/main.h
        src/wfbcli.hpp
        src/wfbcli.cpp
        src/scheduling_helper.hpp
        src/gstrtpreceiver.cpp
        src/gstrtpreceiver.h)
file(GLOB ICONS src/icons/*.png)
file(GLOB OSD_CONFIGS *_osd.json)

include_directories("/usr/include/libdrm" "/usr/include/cairo" "/usr/include/spdlog")

configure_file("${PROJECT_NAME}_config.h.in" "${PROJECT_NAME}_config.h")

# Option to control wmenu simulator build
option(USE_SIMULATOR "Build the simulator version of the application" OFF)

if(USE_SIMULATOR)
  add_definitions(-DUSE_SIMULATOR)
  target_compile_definitions(lvgl PRIVATE USE_SIMULATOR)
  pkg_check_modules(LIBSDL REQUIRED sdl2)
  add_executable(${PROJECT_NAME} ${SIMULATOR_SOURCES} lvgl)
  target_link_libraries(${PROJECT_NAME} lvgl ${LIBSDL_LIBRARIES})
else()
  find_package(spdlog REQUIRED)
  find_package(nlohmann_json 3 REQUIRED)
  pkg_check_modules(LIBGPIOD REQUIRED libgpiod)
  add_executable(${PROJECT_NAME} ${SOURCE_FILES})
  target_link_libraries(${PROJECT_NAME} rockchip_mpp pthread drm m cairo spdlog::spdlog nlohmann_json::nlohmann_json rt lvgl ${LIBGPIOD_LIBRARIES})
  # Embed gstreamer
  find_package(PkgConfig REQUIRED)
  pkg_search_module(GST REQUIRED
          gstreamer-1.0>=1.4
          gstreamer-app-1.0>=1.4
  )
  pkg_search_module(gstreamer REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.4)
  pkg_search_module(gstreamer-app REQUIRED IMPORTED_TARGET gstreamer-app-1.0>=1.4)
  target_link_libraries(${PROJECT_NAME} PkgConfig::gstreamer PkgConfig::gstreamer-app)  
endif()

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
