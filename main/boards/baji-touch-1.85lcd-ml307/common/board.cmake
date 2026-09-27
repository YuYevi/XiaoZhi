# Keep all BAJI-specific implementation and build settings in this board.
set(BAJI_BOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_DIR}")
file(GLOB_RECURSE BAJI_COMMON_SOURCES CONFIGURE_DEPENDS
    "${BAJI_BOARD_DIR}/common/*.cc"
    "${BAJI_BOARD_DIR}/common/*.c"
)
list(APPEND SOURCES ${BAJI_COMMON_SOURCES})
list(APPEND INCLUDE_DIRS "${BAJI_BOARD_DIR}" "${BAJI_BOARD_DIR}/common")

set(BUILTIN_TEXT_FONT font_noto_sans_basic_16_4)
set(BUILTIN_ICON_FONT font_material_symbols_16_4)
set(DEFAULT_EMOJI_COLLECTION noto-color-emoji_64)
set(BAJI_WATCH_RESOURCE_DIR "${BAJI_BOARD_DIR}/common/resources")
# Compile embedded icon headers; only raw assets belong in the resource partition.
set(DEFAULT_ASSETS_EXTRA_FILES "${CMAKE_BINARY_DIR}/baji_watch_resources")
file(GLOB BAJI_WATCH_ASSET_FILES CONFIGURE_DEPENDS
    "${BAJI_WATCH_RESOURCE_DIR}/*.rgb565"
    "${BAJI_WATCH_RESOURCE_DIR}/*.pack")
foreach(BAJI_WATCH_ASSET IN LISTS BAJI_WATCH_ASSET_FILES)
    get_filename_component(BAJI_WATCH_ASSET_NAME "${BAJI_WATCH_ASSET}" NAME)
    configure_file("${BAJI_WATCH_ASSET}"
        "${DEFAULT_ASSETS_EXTRA_FILES}/${BAJI_WATCH_ASSET_NAME}" COPYONLY)
endforeach()

# Append board resources after the native assets rule has been declared.
# This keeps incremental rebuilds correct without changing main/CMakeLists.txt.
function(baji_watch_asset_dependencies)
    if(CONFIG_FLASH_DEFAULT_ASSETS)
        file(GLOB BAJI_WATCH_RESOURCES CONFIGURE_DEPENDS
            "${BAJI_WATCH_RESOURCE_DIR}/*.rgb565"
            "${BAJI_WATCH_RESOURCE_DIR}/*.pack")
        add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/generated_assets.bin" APPEND
            DEPENDS ${BAJI_WATCH_RESOURCES})
    endif()
endfunction()

# Run the board's charging/power-on gate before the native application entry.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=app_main" APPEND)
    # Give this board's nested watch pages enough LVGL event/layout stack.
    idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=lvgl_port_init" APPEND)
    cmake_language(DEFER CALL baji_watch_asset_dependencies)
endif()
