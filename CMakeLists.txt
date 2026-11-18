cmake_minimum_required(VERSION 3.20)
project(Tony_Hawks_Pro_Skater_2_Recomp LANGUAGES C CXX)

set(PSXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/psxrecomp" CACHE PATH
    "Path to psxrecomp")

# --- Opt-in product surfaces (OFF by default in the framework) -------------
# Scaffold prompts / flags fill these. Without recomp-ui, PSX_RECOMP_UI is OFF.
# Setup-host CI requires WIZARD_CMAKE_BLOCK / ENABLE_SETUP_WIZARD (FORCE_SETUP_HOST
# alone does not open first-run / Generate & rebuild).
# recomp-ui submodule present (PSX_RECOMP_UI defaults ON).
# Full netplay UI — enable after adding recomp-ui + testing:
# if(EXISTS "${PSXRECOMP_ROOT}/lib/recomp-net/CMakeLists.txt")
#     set(PSX_NETPLAY ON CACHE BOOL "…" FORCE)
# endif()
if(NINTENDO_3DS)
    # New 3DS port: no desktop launcher, setup wizard, Vulkan, netplay, rewind
    # or debug server. Game C is generated on a PC; this build only links it.
    set(PSX_SETUP_WIZARD OFF CACHE BOOL "No setup wizard on 3DS" FORCE)
    foreach(_psx_off PSX_RECOMP_UI PSX_ENABLE_VULKAN PSX_NETPLAY PSX_REWIND PSX_DEBUG_TOOLS)
        set(${_psx_off} OFF CACHE BOOL "Off on Nintendo 3DS" FORCE)
    endforeach()
    set(_psx_title_surfaces "")
else()
    set(PSX_SETUP_WIZARD ON CACHE BOOL
        "Advertise first-run setup wizard + Generate & rebuild in recomp-ui" FORCE)
    set(_psx_title_surfaces ENABLE_SETUP_WIZARD
        APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/psxrecomp.ico")
endif()
# Release standard: the retail BIOS of the disc region, no OpenBIOS (PS1B-269).
set(PSXRECOMP_BIOS_STEMS "SCPH1001" CACHE STRING "Accepted BIOS" FORCE)
set(PSXRECOMP_BIOS_STEM "SCPH1001" CACHE STRING "Accepted BIOS" FORCE)
set(PSXRECOMP_BIOS_PROFILE "${PSXRECOMP_ROOT}/bios/SCPH1001.toml" CACHE FILEPATH "Accepted BIOS" FORCE)
include("${PSXRECOMP_ROOT}/runtime/runtime.cmake")

psxrecomp_add_game_runtime(psx-runtime
    # ENABLE_NETPLAY_IF_PRESENT
    # NETPLAY_LOBBY_URL "ws://netplay.retcomm.net:8765"
    ${_psx_title_surfaces}
    WINDOW_TITLE "Tony Hawk's Pro Skater 2 Recompiled"
    EXE_NAME "Tony_Hawks_Pro_Skater_2_Recompiled"
    DEFAULT_GAME_CONFIG_PATH "game.toml"
    MAX_PLAYERS 2
    # LAUNCHER_BOXART "${CMAKE_CURRENT_SOURCE_DIR}/launcher_assets/img/boxart.tga"
    GEN_MARKER "generated/SLUS_010.66_dispatch.c"
    GEN_FULL_GLOB "generated/SLUS_010.66_full_*.c"
    CODEGEN_SETUP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/codegen_setup.c"
)

if(NINTENDO_3DS)
    # Data folder on the SD card (game.toml, bios/, disc/, saves). The game
    # runs from here exactly as the desktop build runs from build-release/.
    set(THPS2_N3DS_DATA_DIR "sdmc:/3ds/thps2" CACHE STRING "SD-card data folder")
    target_sources(psx-runtime PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/platform/n3ds/n3ds_platform.c")
    target_compile_definitions(psx-runtime PRIVATE
        THPS2_N3DS_DATA_DIR="${THPS2_N3DS_DATA_DIR}")
    ctr_generate_smdh(thps2.smdh
        NAME "THPS2 Recompiled"
        DESCRIPTION "Tony Hawk's Pro Skater 2 static recompilation (your own disc)"
        AUTHOR "thps2-n3ds")
    ctr_create_3dsx(psx-runtime OUTPUT thps2.3dsx SMDH thps2.smdh)
endif()

# Stage runtime configs next to the exe for direct build/ launches. Only copy
# files that exist — never fail POST_BUILD when game_options.toml is absent.
foreach(_PSX_RUNTIME_CONFIG game.toml game_options.toml)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_PSX_RUNTIME_CONFIG}")
        add_custom_command(TARGET psx-runtime POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/${_PSX_RUNTIME_CONFIG}"
                "$<TARGET_FILE_DIR:psx-runtime>/${_PSX_RUNTIME_CONFIG}"
            COMMENT "Staging ${_PSX_RUNTIME_CONFIG}"
            VERBATIM)
    endif()
endforeach()

