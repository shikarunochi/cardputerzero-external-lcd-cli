cmake_minimum_required(VERSION 3.20)
project(cardputer_cli LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The application requires Linux GPIO, spidev and PTY APIs.")
endif()

find_package(Freetype REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(KEYBOARD REQUIRED IMPORTED_TARGET libevdev xkbcommon)
set(LVGL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lvgl" CACHE PATH "LVGL v9.5.0 checkout")
if(NOT EXISTS "${LVGL_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Clone LVGL v9.5.0 into lvgl/ or set LVGL_SOURCE_DIR.")
endif()
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${LVGL_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/lvgl")

# Same three FreeType implementations overridden by APPLaunch's build.
get_target_property(lvgl_sources lvgl SOURCES)
foreach(file IN ITEMS lv_freetype.c lv_freetype_glyph.c lv_freetype_image.c)
    list(FILTER lvgl_sources EXCLUDE REGEX "/${file}$")
    list(APPEND lvgl_sources "${CMAKE_CURRENT_SOURCE_DIR}/freetype/${file}")
endforeach()
set_property(TARGET lvgl PROPERTY SOURCES "${lvgl_sources}")
target_include_directories(lvgl PRIVATE "${LVGL_SOURCE_DIR}/src/libs/freetype")
target_link_libraries(lvgl PUBLIC Freetype::Freetype)

add_executable(cardputer-cli
    main.cpp st7789_lvgl.cpp status_display.cpp
    keyboard/keyboard.cpp
    keyboard/cp0_keyboard_keymap.c
    keyboard/st_key_encoder.cpp
    model/terminal_unicode.cpp
    page_app/ui_app_st.cpp
    page_app/ui_app_st_parser.cpp
    page_app/ui_app_st_renderer.cpp
    page_app/ui_app_st_session.cpp
    page_app/ui_app_st_viewport.cpp)
target_compile_features(cardputer-cli PRIVATE cxx_std_17)
target_link_libraries(cardputer-cli PRIVATE lvgl::lvgl Threads::Threads PkgConfig::KEYBOARD util m)
