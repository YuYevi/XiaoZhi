# Keep all BAJI-specific implementation and build settings in this board.
set(BAJI_BOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_DIR}")
file(GLOB BAJI_COMMON_SOURCES CONFIGURE_DEPENDS
    "${BAJI_BOARD_DIR}/common/*.cc"
    "${BAJI_BOARD_DIR}/common/*.c"
)
list(APPEND SOURCES ${BAJI_COMMON_SOURCES})
list(APPEND INCLUDE_DIRS "${BAJI_BOARD_DIR}" "${BAJI_BOARD_DIR}/common")

set(BUILTIN_TEXT_FONT font_noto_sans_basic_16_4)
set(BUILTIN_ICON_FONT font_material_symbols_16_4)
set(DEFAULT_EMOJI_COLLECTION noto-color-emoji_64)

# Run the board's charging/power-on gate before the native application entry.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=app_main" APPEND)
endif()
