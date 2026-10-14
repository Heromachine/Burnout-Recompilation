cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Burnout - Static Recompilation
#
# Builds the Windows host executable around the lifted game code in
# src/recomp/gen/, which ./regenerate.sh produces from the user's own disc
# image. Built with mingw-w64 and run under Wine:
#
#   cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=mingw-toolchain.cmake
#   cmake --build build-win -j
# ============================================================================

project(burnout_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX

    # Bring-up: record every indirect-branch target the title actually reaches,
    # so unresolved ICALLs are closed by measurement rather than by reading
    # disassembly. Feeds config/seeds/icall-feedback.json via
    # tools/recomp/icall_feedback.py. Must be defined before the
    # add_subdirectory() below, so xbox_kernel (which builds icall_feedback.c)
    # sees it too and not just this executable's own sources.
    RECOMP_ICALL_FEEDBACK

    # The default observation window is 8 MiB from 0x00010000, which stops at
    # 0x00810000. Burnout's image runs to 0x008C7140 and its DOLBY section
    # (0x008AAFC0) is executable, so the default would silently drop any target
    # in it. 9 MiB covers the whole image.
    RECOMP_ICALL_FB_SIZE=0x00900000u

    # The runtime's kernel data exports (KeTickCount among them), guest thread
    # objects and stacks must sit above the image, BSS included. The defaults
    # (0x00740000 / 0x00780000) lie inside Burnout's BSS, which kept KeTickCount
    # at 0 for the game and silenced its music (HeroLab task 78a6032f). Needs
    # xboxrecomp 4e90527 or later; an older fork ignores these with a warning.
    XBOX_KERNEL_DATA_BASE=0x008D0000
    XBOX_STACK_BASE=0x00900000

    # Real waits on the KEVENTs D3D builds in guest memory, plus the
    # NO_OPERATION notify that sets them. Under async PFIFO, D3D's push-buffer
    # space wait never ends without them. Off by default in the runtime,
    # because Breakdown's spin fixes assume the old fail-at-once waits
    # (HeroLab task 7b9e050b).
    XBOXRECOMP_GUEST_KEVENTS=1
)

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# The repo is named xboxrecomp-fork on this machine, not xboxrecomp, so the
# template's default relative path is overridden. The revision this project is
# pinned to is recorded in config/source-manifest.json and checked by
# tools/verify-inputs.sh.
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp-fork" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

# BURNOUT_BUILD_DLL builds burnout_game.dll instead of the exe: the same code,
# started by the Windows launcher (launcher-win/) in its own window; see the
# exports at the end of src/main.c. The target name stays the same either way
# so every setting below applies to both.
option(BURNOUT_BUILD_DLL "Build burnout_game.dll for the Windows launcher" OFF)
if(BURNOUT_BUILD_DLL)
    add_library(${PROJECT_NAME} SHARED
        ${GAME_SOURCES}
        ${RECOMP_GEN_SOURCES}
    )
    set_target_properties(${PROJECT_NAME} PROPERTIES
        OUTPUT_NAME burnout_game
        PREFIX "")
    target_compile_definitions(${PROJECT_NAME} PRIVATE BURNOUT_DLL)
else()
    add_executable(${PROJECT_NAME} WIN32
        ${GAME_SOURCES}
        ${RECOMP_GEN_SOURCES}
    )
endif()

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)

# Windows SDK libraries needed at link time
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    dxguid
    xinput
    winmm
)

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )
