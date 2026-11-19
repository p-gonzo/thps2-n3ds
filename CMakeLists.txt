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
    # Constrained-host profile: compile out the per-store/per-dispatch
    # diagnostic hooks nothing on the console can read (no debug server).
    # Azahar, 804 MHz: BIOS start 17.3 -> 38.3 fps, Sony logo 6.6 -> 7.0.
    set(PSX_LEAN_HOST ON CACHE BOOL "Lean runtime profile on Nintendo 3DS" FORCE)
    # Observation time: ordinary block edges decide inline whether anything is
    # due; only slow edges call the full interrupt check (2,232 instructions,
    # 23% of 3DS gameplay). Gameplay frames are pixel-identical with it off.
    set(PSX_OBSERVATION_TIME ON CACHE BOOL "Inline block-edge policy on Nintendo 3DS" FORCE)
    set(_psx_title_surfaces "")
    # Release optimisation for the 3DS (ARM11, ARM mode). Measured in Azahar at
    # 804 MHz, Sony logo: -O2 6.0 fps, -O3 6.6, -Os 5.0; see docs/N3DS_PORT_PLAN.md.
    # Azahar has no cache model, so -Os (half the code) may do better on
    # hardware; THPS2_N3DS_GENERATED_CFLAGS=-Os is the mixed option.
    set(THPS2_N3DS_OPT "-O3" CACHE STRING "Release optimisation level for 3DS builds")
    # -g only adds debug info (line numbers for GDB profiles); code is unchanged.
    set(CMAKE_C_FLAGS_RELEASE "${THPS2_N3DS_OPT} -g -DNDEBUG")
    set(CMAKE_CXX_FLAGS_RELEASE "${THPS2_N3DS_OPT} -g -DNDEBUG")
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
        "${CMAKE_CURRENT_SOURCE_DIR}/platform/n3ds/n3ds_platform.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/platform/n3ds/n3ds_hud.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/platform/n3ds/n3ds_fatal.cpp")
    # Commit ids for the log header (log.txt names the exact build).
    execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                    OUTPUT_VARIABLE _thps2_rev OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY "${PSXRECOMP_ROOT}"
                    OUTPUT_VARIABLE _psx_rev OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    target_compile_definitions(psx-runtime PRIVATE
        THPS2_N3DS_DATA_DIR="${THPS2_N3DS_DATA_DIR}"
        THPS2_N3DS_BUILD="${_thps2_rev}" THPS2_N3DS_PSXRECOMP="${_psx_rev}")
    # Scripted input (input-script.txt, platform/n3ds/n3ds_platform.c) is
    # ORed into the buttons SDL's 3DS joystick driver reads from libctru.
    target_link_options(psx-runtime PRIVATE
        "LINKER:--wrap=hidKeysDown" "LINKER:--wrap=hidKeysUp" "LINKER:--wrap=hidKeysHeld")
    ctr_generate_smdh(thps2.smdh
        NAME "THPS2 Recompiled"
        DESCRIPTION "Tony Hawk's Pro Skater 2 static recompilation (your own disc)"
        AUTHOR "thps2-n3ds")
    ctr_create_3dsx(psx-runtime OUTPUT thps2.3dsx SMDH thps2.smdh)

    # Optional extra flags for the recompiled game and BIOS code only (about
    # 20 MB of machine code, mostly cold), e.g. "-Os" to halve its size while
    # the runtime keeps CMAKE_BUILD_TYPE's optimisation. Appended after the
    # build-type flags, so its -O level wins for these files.
    set(THPS2_N3DS_GENERATED_CFLAGS "" CACHE STRING
        "Extra compile flags for generated game/BIOS C (3DS builds)")
    if(THPS2_N3DS_GENERATED_CFLAGS)
        separate_arguments(_thps2_gen_flags UNIX_COMMAND "${THPS2_N3DS_GENERATED_CFLAGS}")
        file(GLOB _thps2_gen_sources
            "${CMAKE_CURRENT_SOURCE_DIR}/generated/*.c"
            "${PSXRECOMP_ROOT}/generated/SCPH1001_*.c")
        set_source_files_properties(${_thps2_gen_sources}
            TARGET_DIRECTORY psx-runtime
            PROPERTIES COMPILE_OPTIONS "${_thps2_gen_flags}")
        message(STATUS "thps2-n3ds: generated code gets ${THPS2_N3DS_GENERATED_CFLAGS}")
    endif()
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

