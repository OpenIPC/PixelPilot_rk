cmake_minimum_required(VERSION 3.1)

project(pixelpilot VERSION 1.3.0)
find_package(PkgConfig REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Option to control wmenu simulator build
option(USE_SIMULATOR "Build the simulator version of the application" OFF)
# If build should include tests
option(BUILD_TESTS "Build unit tests" OFF)

# LVGL setup
set(LV_USE_LINUX_DRM ON)
set(LV_CONF_BUILD_DISABLE_DEMOS 1)
set(LV_CONF_BUILD_DISABLE_EXAMPLES 1)
add_subdirectory(lvgl)

# Set include directories for LVGL BEFORE any other targets
target_include_directories(lvgl PUBLIC 
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/lvgl
)
set(CMAKE_C_STANDARD 99) # LVGL officially supports C99 and above
set(CMAKE_CXX_STANDARD 17) #C17
set(CMAKE_CXX_STANDARD_REQUIRED ON)

pkg_check_modules(LIBDRM REQUIRED libdrm)
pkg_check_modules(CAIRO IMPORTED_TARGET REQUIRED cairo)
pkg_check_modules(SPDLOG REQUIRED spdlog)

find_package(PNG REQUIRED)
find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)

include_directories(${PNG_INCLUDE_DIR})

# Simulator
set(SIMULATOR_SOURCES
        src/simulator.c
        src/gsmenu/executor.h
        src/gsmenu/executor.c
        src/gsmenu/air_presets.h
        src/gsmenu/air_presets.c
        src/gsmenu/air_telemetry.h
        src/gsmenu/air_telemetry.c
        src/gsmenu/air_txprofiles.h
        src/gsmenu/air_txprofiles.c
        src/gsmenu/air_camera.h
        src/gsmenu/air_camera.c
        src/gsmenu/air_actions.h
        src/gsmenu/air_actions.c
        src/gsmenu/gs_wifi.h
        src/gsmenu/gs_wifi.c
        src/gsmenu/ui.h
        src/gsmenu/helper.h
        src/gsmenu/helper.c
        src/gsmenu/gs_main.c
        src/gsmenu/gs_main.h
        src/gsmenu/gs_dvr.h
        src/gsmenu/gs_dvr.c
        src/gsmenu/gs_dvrplayer.h
        src/gsmenu/gs_dvrplayer.c
        src/gsmenu/gs_wfbng.h
        src/gsmenu/gs_wfbng.c
        src/gsmenu/gs_apfpv.h
        src/gsmenu/gs_apfpv.c
        src/gsmenu/gs_system.h
        src/gsmenu/gs_system.c
        src/gsmenu/gs_actions.h
        src/gsmenu/gs_actions.c
        src/gsmenu/gs_connection_checker.h
        src/gsmenu/gs_connection_checker.c
        src/gsmenu/air_wfbng.h
        src/gsmenu/air_wfbng.c
        src/gsmenu/air_alink.h
        src/gsmenu/air_alink.c
        src/gsmenu/air_aalink.h
        src/gsmenu/air_aalink.c
        src/gsmenu/ui.c
        src/gsmenu/styles.h
        src/gsmenu/styles.c
        src/lvosd.h
        src/lvosd.c
        src/menu.h
        src/menu.c
        src/input.h
        src/input.cpp
)

add_compile_options("-Wno-address-of-packed-member")

set(LIB_SOURCE_FILES 
        src/drm.h
        src/drm.c
        src/gsmenu/executor.h
        src/gsmenu/executor.c
        src/gsmenu/air_presets.h
        src/gsmenu/air_presets.c
        src/gsmenu/air_telemetry.c
        src/gsmenu/air_txprofiles.h
        src/gsmenu/air_txprofiles.c
        src/gsmenu/air_camera.h
        src/gsmenu/air_camera.c
        src/gsmenu/air_actions.h
        src/gsmenu/air_actions.c
        src/gsmenu/gs_wifi.h
        src/gsmenu/gs_wifi.c
        src/gsmenu/ui.h
        src/gsmenu/helper.h
        src/gsmenu/helper.c
        src/gsmenu/gs_main.c
        src/gsmenu/gs_main.h
        src/gsmenu/gs_dvr.h
        src/gsmenu/gs_dvr.c
        src/gsmenu/gs_dvrplayer.h
        src/gsmenu/gs_dvrplayer.c
        src/gsmenu/gs_wfbng.h
        src/gsmenu/gs_wfbng.c
        src/gsmenu/gs_apfpv.h
        src/gsmenu/gs_apfpv.c
        src/gsmenu/gs_system.h
        src/gsmenu/gs_system.c
        src/gsmenu/air_wfbng.h
        src/gsmenu/air_wfbng.c
        src/gsmenu/air_alink.h
        src/gsmenu/air_alink.c
        src/gsmenu/air_aalink.h
        src/gsmenu/air_aalink.c
        src/gsmenu/gs_actions.h
        src/gsmenu/gs_actions.c
        src/gsmenu/gs_connection_checker.h
        src/gsmenu/gs_connection_checker.c
        src/gsmenu/styles.h
        src/gsmenu/styles.c
        src/gsmenu/ui.c 
        src/lvosd.h
        src/lvosd.c
        src/menu.h
        src/menu.c
        src/input.h
        src/input.cpp
        src/osd.h
        src/osd.hpp
        src/osd.cpp
        src/os_mon.hpp
        src/os_mon.cpp
        src/dvr.h
        src/dvr.cpp
        src/mavlink.h
        src/mavlink.c
        src/wfbcli.hpp
        src/wfbcli.cpp
        src/WiFiRSSIMonitor.hpp
        src/WiFiRSSIMonitor.cpp
        src/scheduling_helper.hpp
        src/gstrtpreceiver.cpp
        src/gstrtpreceiver.h)
set(SOURCE_FILES
  ${LIB_SOURCE_FILES}
  src/main.cpp
  src/main.h)
file(GLOB ICONS src/icons/*.png)
file(GLOB OSD_CONFIGS *_osd.json)

configure_file("${PROJECT_NAME}_config.h.in" "${PROJECT_NAME}_config.h")

if(USE_SIMULATOR)
  add_definitions(-DUSE_SIMULATOR)
  target_compile_definitions(lvgl PRIVATE USE_SIMULATOR)
  pkg_check_modules(LIBSDL REQUIRED sdl2)
  add_executable(${PROJECT_NAME} ${SIMULATOR_SOURCES})
  target_link_libraries(${PROJECT_NAME} lvgl ${LIBSDL_LIBRARIES} ${PNG_LIBRARIES} ${LIBDRM_LIBRARIES})
else()
  find_package(nlohmann_json 3 REQUIRED)
  find_package(yaml-cpp REQUIRED)
  pkg_check_modules(LIBGPIOD REQUIRED libgpiod)
  include_directories(${LIBGPIOD_INCLUDE_DIR})
  
  # GStreamer setup
  find_package(PkgConfig REQUIRED)
  pkg_search_module(GST REQUIRED
          gstreamer-1.0>=1.4
          gstreamer-app-1.0>=1.4
  )
  pkg_search_module(gstreamer REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.4)
  pkg_search_module(gstreamer-app REQUIRED IMPORTED_TARGET gstreamer-app-1.0>=1.4)
  
  add_executable(${PROJECT_NAME} ${SOURCE_FILES})
  
  # Add include directories for the main target
  target_include_directories(${PROJECT_NAME} PRIVATE
      ${PROJECT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/lvgl
      ${LIBDRM_INCLUDE_DIRS}
  )
  
  target_link_libraries(${PROJECT_NAME} 
      rockchip_mpp 
      pthread 
      drm 
      m 
      PkgConfig::CAIRO
      spdlog::spdlog 
      fmt::fmt
      nlohmann_json::nlohmann_json 
      yaml-cpp 
      rt 
      lvgl 
      ${LIBGPIOD_LIBRARIES} 
      ${PNG_LIBRARIES}
      PkgConfig::gstreamer 
      PkgConfig::gstreamer-app
  )
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

# Testing
if(BUILD_TESTS)
    find_package(Catch2 REQUIRED)

    # Test source files
    set(TEST_SOURCES
      tests/test_osd.cpp
      src/main.h
      src/main.cpp
    )
    find_package(spdlog REQUIRED)
    find_package(nlohmann_json 3 REQUIRED)
    find_package(yaml-cpp REQUIRED)

    # Test executable
    add_executable(pixelpilot_tests ${LIB_SOURCE_FILES} ${TEST_SOURCES})
    target_include_directories(pixelpilot_tests PUBLIC "${PROJECT_BINARY_DIR}" ${LIBDRM_INCLUDE_DIRS} ${CAIRO_INCLUDE_DIRS})
    # Define the TEST preprocessor directive
    target_compile_definitions(pixelpilot_tests PRIVATE TEST)
    target_link_libraries(pixelpilot_tests rockchip_mpp pthread drm m cairo spdlog::spdlog nlohmann_json::nlohmann_json yaml-cpp rt lvgl ${LIBGPIOD_LIBRARIES} ${PNG_LIBRARIES} Catch2::Catch2WithMain)

    find_package(PkgConfig REQUIRED)
    pkg_search_module(GST REQUIRED
          gstreamer-1.0>=1.4
          gstreamer-app-1.0>=1.4
    )
    pkg_search_module(gstreamer REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.4)
    pkg_search_module(gstreamer-app REQUIRED IMPORTED_TARGET gstreamer-app-1.0>=1.4)
    target_link_libraries(pixelpilot_tests PkgConfig::gstreamer PkgConfig::gstreamer-app)

    target_link_options(pixelpilot_tests
      BEFORE PUBLIC -fsanitize=undefined PUBLIC -fsanitize=address
    )

    # Optionally, establish testing options
    #enable_testing()
    #add_test(NAME PixelPilotTests COMMAND pixelpilot_tests)
endif()


# Installation

include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES ${ICONS}
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/${PROJECT_NAME}
)

# Install configuration files to /etc
install(FILES ${OSD_CONFIGS}
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/${PROJECT_NAME}
)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/pixelpilot.yaml
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/${PROJECT_NAME}
)

# Install gsmenu.sh to bin directory
install(FILES 
    ${CMAKE_CURRENT_SOURCE_DIR}/gsmenu.sh
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
)
