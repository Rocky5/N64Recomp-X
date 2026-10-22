# N64Recomp-X: default.xbe. Configure with nxdk's toolchain file (xbox/build.sh;
# n64rx runs it per game).
#
# Two kinds of code, two compile setups (PLAN.md 3.1, docs/toolchain.md):
#   game  xbox/src/rt (the runtime), xbox/src/gfx (the interpreter) and the
#         game's recompiled C (XRC_GEN_DIR): the game triple
#         (i686-pc-windows-gnu -mno-ms-bitfields), compiled by
#         tools/xbox/compile_game.py and linked here as objects
#   hw    xbox/src/hw: nxdk's own triple, talks to the kernel, pbkit and USB
# They share only the plain-scalar structs of xhw.h, xgfx.h and xrom.h.
cmake_minimum_required(VERSION 3.16)
project(n64recomp_x C)

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
get_filename_component(ROOT_ABS "${ROOT}" ABSOLUTE)
# __FILE__ relative to the checkout: no builder's home directory in the XBE
add_compile_options("-ffile-prefix-map=${ROOT_ABS}/=")
set(NXDK_DIR $ENV{NXDK_DIR})

# ---- what this build is (n64rx sets these from the game pack) ----
set(XRC_GEN_DIR "" CACHE PATH "the game's generated C and xrc_game.c (empty: no game built in)")
set(XRC_TITLE_ID "4E520000" CACHE STRING "the certificate's title ID, 8 hex digits (PLAN.md 3.6)")
set(XRC_XBE_TITLE "N64Recomp-X" CACHE STRING "the XBE's title")
set(XRC_GAME_CFLAGS "" CACHE STRING "the pack's flags for the game side (-DXRC_GPR32=1 ...)")
file(STRINGS ${ROOT}/VERSION XRC_VERSION LIMIT_COUNT 1)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/xrc_build.h.in ${CMAKE_BINARY_DIR}/include/xrc_build.h @ONLY)

# ---- pbkit, patched so a GPU fault can't freeze the console (Melee-X) ----
set(PBKIT_SRC ${NXDK_DIR}/lib/pbkit)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/pbkit/pbkit.c
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/pbkit
    COMMAND python3 ${ROOT}/tools/xbox/patch_pbkit.py ${PBKIT_SRC}/pbkit.c ${CMAKE_BINARY_DIR}/pbkit/pbkit.c
    DEPENDS ${PBKIT_SRC}/pbkit.c ${ROOT}/tools/xbox/patch_pbkit.py
    COMMENT "patch pbkit.c")
add_library(xrc_pbkit STATIC
    ${CMAKE_BINARY_DIR}/pbkit/pbkit.c
    ${PBKIT_SRC}/pbkit_gamma.c ${PBKIT_SRC}/pbkit_dma.c ${PBKIT_SRC}/pbkit_draw.c
    ${PBKIT_SRC}/pbkit_print.c ${PBKIT_SRC}/pbkit_pushbuffer.c)
target_include_directories(xrc_pbkit PRIVATE ${PBKIT_SRC})
target_compile_options(xrc_pbkit PRIVATE -w)
# pbkit's push helpers run on every draw; they were built with no -O at all
# (docs/review-2026-10.md section 2). XBOX_CMAKE_ARGS=-DXBOX_PBKIT_O2=OFF: as before
option(XBOX_PBKIT_O2 "pbkit at -O2, as the platform layer" ON)
if(XBOX_PBKIT_O2)
    target_compile_options(xrc_pbkit PRIVATE -O2 -march=pentium3)
endif()

# ---- game side (tools/xbox/compile_game.py, incremental via depfiles) ----
# XBOX_LTO (default ON; XBOX_LTO=0 turns it off): bitcode, ThinLTO at the
# link (tools/xbox/thinlto_link.py); its own object folder.
option(XBOX_LTO "ThinLTO over the game side (PLAN.md 4.4: the default)" ON)
set(GAME_OUT ${CMAKE_BINARY_DIR}/game)
set(GAME_ENV XBOX_LTO=)
if(XBOX_LTO)
    set(GAME_OUT ${GAME_OUT}-lto)
    set(GAME_ENV XBOX_LTO=1)
endif()
add_custom_target(game_objs
    COMMAND ${CMAKE_COMMAND} -E env XBOX_GAME_OUT=${GAME_OUT} ${GAME_ENV} "XBOX_GAME_CFLAGS=${CMAKE_C_FLAGS}"
        "XRC_GAME_CFLAGS=${XRC_GAME_CFLAGS}" "XRC_GEN_DIR=${XRC_GEN_DIR}"
        "XRC_BUILD_INCLUDE=${CMAKE_BINARY_DIR}/include" python3 ${ROOT}/tools/xbox/compile_game.py
    WORKING_DIRECTORY ${ROOT}
    BYPRODUCTS ${GAME_OUT}/objects.txt ${GAME_OUT}/objects.stamp
    COMMENT "game side: compile"
    USES_TERMINAL)

# ---- hw: kernel, pbkit, AC97, USB (nxdk triple) ----
file(GLOB HW_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/hw/*.c)
add_library(xrc_hw OBJECT ${HW_SOURCES})
target_include_directories(xrc_hw PRIVATE
    ${CMAKE_BINARY_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hw
    ${NXDK_DIR}/lib/sdl/SDL2/include
    ${NXDK_DIR}/lib/usb/libusbohci/inc
    ${NXDK_DIR}/lib/usb/libusbohci_xbox
    ${NXDK_DIR}/lib/zlib/zlib)
target_compile_definitions(xrc_hw PRIVATE XBOX "USBH_USE_EXTERNAL_CONFIG=\"usbh_config_xbox.h\"")
target_compile_options(xrc_hw PRIVATE -O2 -march=pentium3 -ffunction-sections -Wall -Wno-unused-function
    "SHELL:-include \"${CMAKE_CURRENT_SOURCE_DIR}/include/xbuiltin.h\"")   # quoted: the app's data folder has a space
# the string routines must not be recognised as themselves (xhw_string.c)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/hw/xhw_string.c PROPERTIES
    COMPILE_OPTIONS "-fno-builtin;-ffreestanding" COMPILE_DEFINITIONS XBUILTIN_OFF)

# ---- link ----
add_executable(n64recomp_x $<TARGET_OBJECTS:xrc_hw>)
target_link_libraries(n64recomp_x
    xrc_pbkit
    ${NXDK_DIR}/lib/libSDL2.lib
    ${NXDK_DIR}/lib/libzlib.lib
    ${NXDK_DIR}/lib/libnxdk_automount_d.lib)
target_link_options(n64recomp_x PRIVATE
    -stack:1048576
    -include:_automount_d_drive
    -map:${CMAKE_BINARY_DIR}/n64recomp_x.map
    # every undefined symbol, not the first 20
    -errorlimit:0
    # never fold two identical functions into one address: the game compares
    # function pointers and get_function's tables must not fold
    -opt:noicf
    "@${GAME_OUT}/objects.txt"
    # the runtime's boot (xbox/src/rt/boot.c) replaces the platform's weak one
    -include:_xrc_boot)
add_dependencies(n64recomp_x game_objs)
if(XBOX_LTO)
    set_property(TARGET n64recomp_x PROPERTY RULE_LAUNCH_LINK "python3 \"${ROOT}/tools/xbox/thinlto_link.py\"")
endif()
set_property(TARGET n64recomp_x APPEND PROPERTY LINK_DEPENDS ${GAME_OUT}/objects.stamp)
add_custom_command(TARGET n64recomp_x POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/xbe
    COMMAND ${NXDK_DIR}/tools/cxbe/cxbe "-OUT:${CMAKE_BINARY_DIR}/xbe/default.xbe" "-TITLE:${XRC_XBE_TITLE}" $<TARGET_FILE:n64recomp_x>
    COMMENT "cxbe -> xbe/default.xbe"
    VERBATIM)
