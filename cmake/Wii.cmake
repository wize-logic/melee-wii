# Super Smash Bros. Melee for the Wii: the game's sources built with devkitPPC
# over libogc and wiilib (libs/wiilib) (configure with the "wii" preset)
include_guard(GLOBAL)

# No unwind tables: C here never unwinds, and they would take close to 1 MiB
# of the MEM1 the game's heaps need
add_compile_options(-fno-asynchronous-unwind-tables -fno-unwind-tables)

get_filename_component(_melee_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
enable_language(ASM)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(MELEE_ORIG_DOL "${_melee_root}/orig/GALE01/sys/main.dol" CACHE FILEPATH
    "The game's main.dol (1.02), for the tables the sources include from it")

# The game as MWCC built it (the decomp's matching objects, at the GameCube's
# addresses: wii/tools/orig_image.py), or the game's sources built with GCC.
# Online play needs the first: the Wii has to compute the same frames as the
# GameCube bit for bit, and Slippi's codes patch the GameCube's code.
option(MELEE_ORIG_CODE "Link the decomp's MWCC objects at the GameCube's addresses" ON)
set(MELEE_DECOMP_ROOT "${_melee_root}" CACHE PATH
    "The decomp whose MWCC objects (build/GALE01/src) MELEE_ORIG_CODE links")

# The byte tables configure.py extracts with dtk
set(_melee_gen "${CMAKE_BINARY_DIR}/include")
set(_melee_incs
    "${_melee_gen}/sysdolphin/baselib/sislib_font.inc"
    "${_melee_gen}/sysdolphin/baselib/debug_font.inc")
add_custom_command(
    OUTPUT ${_melee_incs}
    COMMAND Python3::Interpreter "${_melee_root}/wii/tools/extract_incs.py"
            "${MELEE_ORIG_DOL}" "${_melee_gen}"
    DEPENDS "${_melee_root}/wii/tools/extract_incs.py" "${MELEE_ORIG_DOL}"
    COMMENT "Extracting the DOL's tables")
add_custom_target(melee_incs DEPENDS ${_melee_incs})

# What every game and SDK source is compiled with
add_library(melee_wii_flags INTERFACE)
target_include_directories(melee_wii_flags INTERFACE
    ${_melee_root}/wii/include
    ${_melee_root}/src
    ${_melee_root}/libs/doldecomp/include
    ${_melee_root}/libs/dolphin/include
    ${_melee_gen}
)
target_compile_definitions(melee_wii_flags INTERFACE
    TARGET_WII
    BUILD_VERSION=0
    VERSION_GALE01
    NDEBUG=1
)
target_compile_options(melee_wii_flags INTERFACE
    $<$<COMPILE_LANGUAGE:C>:-std=gnu17>
    -fsigned-char # MWCC's char
    # The sources' text is UTF-8; the game's is Shift-JIS (sjiswrap converts it
    # for MWCC, as code page 932)
    -finput-charset=UTF-8
    -fexec-charset=CP932
    -fno-strict-aliasing
    -fwrapv
    -fno-common
    # The game's small variables would overflow the 64 KiB that r13 reaches,
    # which libogc's and newlib's own small data need
    -G0
    # The sources match MWCC's output: GCC is not to optimise on their undefined
    # behaviour (reads past an array's end, uninitialised locals, null checks)
    -fno-aggressive-loop-optimizations
    -fno-delete-null-pointer-checks
    -ftrivial-auto-var-init=zero
    -Wno-stringop-overflow
    -Wno-error=incompatible-pointer-types
    -Os # MEM1 is the game's: its heaps are sized from what the program leaves
    -g
)

if(NOT MELEE_ORIG_CODE)
file(GLOB_RECURSE _melee_sources
    "${_melee_root}/src/melee/*.c"
    "${_melee_root}/src/sysdolphin/*.c")
# MSL's math, which the game's results depend on (newlib's differs in the last
# bits)
list(APPEND _melee_sources
    "${_melee_root}/src/MSL/math.c"
    "${_melee_root}/src/MSL/math_1.c"
    "${_melee_root}/src/MSL/math_data.c"
    "${_melee_root}/src/MSL/trigf.c")

add_library(melee_game STATIC ${_melee_sources})
# src/melee/gm/synctest.inc: a hash of the game's state each frame, to compare with
# the GameCube build's
option(MELEE_SYNCTEST "Log the game's state each frame (a determinism test)" OFF)
if(MELEE_SYNCTEST)
    target_compile_definitions(melee_game PRIVATE MELEE_SYNCTEST)
endif()
target_link_libraries(melee_game PRIVATE melee_wii_flags)
# The game's sources address data across objects as MWCC laid them out (one
# table read past another's end, a struct over neighbouring statics): GCC is not
# to drop a static only reached that way (-fno-toplevel-reorder keeps every
# static, in the source's order), to fold reads from a static it finds never
# written (from its initializer, to zero past its end), nor to assume a store
# through a pointer leaves another object alone.
target_compile_options(melee_game PRIVATE
    # MWCC fuses a multiply and an add written in one expression (fp_contract
    # on) but not across statements: the game's results have to be the same,
    # bit for bit, as the GameCube's (online play runs both in lockstep)
    -ffp-contract=on
    -fno-toplevel-reorder
    -fno-ipa-reference-addressable
    -fno-tree-pta)
add_dependencies(melee_game melee_incs)
endif()

# The SDK's own sources where they run on the Wii unchanged: GX drives the
# same registers and FIFO; the rest is plain C. Functions written in MWCC's
# assembly are left out of the copies built here (wii/tools/strip_asm.py) and
# provided by wii/src.
set(_sdk_root "${_melee_root}/libs/dolphin/src/dolphin")
set(_sdk_sources
    os/OSTime.c
    thp/THPDec.c
    ai/ai.c
    ax/AX.c ax/AXAlloc.c ax/AXAux.c ax/AXCL.c ax/AXOut.c ax/AXProf.c
    ax/AXSPB.c ax/AXVPB.c ax/DSPCode.c
    dsp/dsp.c dsp/dsp_debug.c dsp/dsp_task.c
)
# The units the game as MWCC built it keeps as they are (MELEE_ORIG_CODE:
# wii/tools/orig_image.py --keep), built here for the game built with GCC
set(_sdk_kept_units dolphin/gx/ dolphin/mtx/ dolphin/os/OSAlloc.c
    dolphin/pad/PadClamp.c dolphin/axfx/)
if(NOT MELEE_ORIG_CODE)
    list(APPEND _sdk_sources
        gx/GXAttr.c gx/GXBump.c gx/GXDisplayList.c gx/GXFifo.c
        gx/GXFrameBuf.c gx/GXGeometry.c gx/GXInit.c gx/GXLight.c gx/GXMisc.c
        gx/GXPerf.c gx/GXPixel.c gx/GXSave.c gx/GXStubs.c gx/GXTev.c
        gx/GXTexture.c gx/GXTransform.c gx/GXVert.c
        mtx/mtx.c mtx/mtx44.c mtx/mtxvec.c mtx/vec.c
        os/OSAlloc.c
        pad/PadClamp.c
        axfx/axfx.c axfx/chorus.c axfx/delay.c axfx/reverb_hi.c axfx/reverb_std.c)
endif()
# What wii/src provides instead, of the C that is left: THP's start and its
# rows, which go through the locked cache (wii/src/thp.c)
set(_sdk_drop_thp/THPDec.c
    THPInit __THPDecompressiMCURow640x480 __THPDecompressiMCURowNxN)
set(_sdk_built)
foreach(_src IN LISTS _sdk_sources)
    set(_out "${CMAKE_BINARY_DIR}/sdk/${_src}")
    set(_drop)
    if(DEFINED _sdk_drop_${_src})
        set(_drop --drop ${_sdk_drop_${_src}})
    endif()
    add_custom_command(
        OUTPUT "${_out}"
        COMMAND Python3::Interpreter "${_melee_root}/wii/tools/strip_asm.py"
                "${_sdk_root}/${_src}" "${_out}" ${_drop}
        DEPENDS "${_melee_root}/wii/tools/strip_asm.py" "${_sdk_root}/${_src}"
        COMMENT "Stripping MWCC assembly from ${_src}")
    list(APPEND _sdk_built "${_out}")
endforeach()

add_library(melee_sdk STATIC ${_sdk_built})
target_link_libraries(melee_sdk PRIVATE melee_wii_flags)
target_include_directories(melee_sdk PRIVATE
    ${_sdk_root}
    ${_melee_root}/libs/dolphin/src
    ${_sdk_root}/gx
    ${_sdk_root}/os
    ${_sdk_root}/ax
    ${_sdk_root}/dsp
    ${_sdk_root}/ai
    ${_sdk_root}/axfx)
target_compile_options(melee_sdk PRIVATE
    # (the SDK is built without MWCC's fp_contract)
    -ffp-contract=off
    -include "${_melee_root}/wii/include/melee_wii/sdk_compat.h")
# AX's parameter blocks' sync, under the port's (wii/src/ax_wii.c: the DSP's
# addresses for the ARAM in MEM2)
set_source_files_properties("${CMAKE_BINARY_DIR}/sdk/ax/AXVPB.c" PROPERTIES
    COMPILE_DEFINITIONS "__AXSyncPBs=__AXSyncPBs_sdk")
# The waits on the DSP's mailboxes, which spin with interrupts off, under
# the port's, which give up (wii/src/stall.c)
set_source_files_properties("${CMAKE_BINARY_DIR}/sdk/ax/AXOut.c"
    "${CMAKE_BINARY_DIR}/sdk/dsp/dsp_task.c" PROPERTIES
    COMPILE_DEFINITIONS "DSPCheckMailToDSP=melee_wii_dsp_mail_to_dsp;DSPCheckMailFromDSP=melee_wii_dsp_mail_from_dsp")

# wiilib: files, the log, the buttons, the stop screen, and the GameCube disc
# reader
add_subdirectory("${_melee_root}/libs/wiilib" wiilib EXCLUDE_FROM_ALL)

# The SDK on libogc and wiilib
file(GLOB _port_sources CONFIGURE_DEPENDS "${_melee_root}/wii/src/*.c")
list(REMOVE_ITEM _port_sources "${_melee_root}/wii/src/main.c"
                                "${_melee_root}/wii/src/os_irq.c"
                                "${_melee_root}/wii/src/cache.c"
                                "${_melee_root}/wii/src/libc_compat.c"
                                "${_melee_root}/wii/src/gecko.c"
                                "${_melee_root}/wii/src/launcher.c"
                                "${_melee_root}/wii/src/launcher_settings.c"
                                "${_melee_root}/wii/src/classic.c"
                                "${_melee_root}/wii/src/mmu_track.c"
                                "${_melee_root}/wii/src/copy.c"
                                "${_melee_root}/wii/src/stall.c"
                                "${_melee_root}/wii/src/wifi.c"
                                "${_melee_root}/wii/src/diag_dirty.c"
                                "${_melee_root}/wii/src/diag_watch.c"
                                "${_melee_root}/wii/src/fused.c")
if(MELEE_ORIG_CODE)
    list(REMOVE_ITEM _port_sources "${_melee_root}/wii/src/mtx_ps.c")
endif()
add_library(melee_port STATIC ${_port_sources})
# Not the game's bool (wii/include): wiilib's headers have C's
target_include_directories(melee_port PRIVATE
    ${_melee_root}/libs/dolphin/include
    ${_melee_root}/libs/dolphin/src/dolphin/ax
    ${_melee_root}/wii/src)
target_compile_definitions(melee_port PRIVATE TARGET_WII
    $<$<BOOL:${MELEE_ORIG_CODE}>:MELEE_ORIG_CODE>)
target_compile_options(melee_port PRIVATE -std=gnu17 -O2 -g -Wall -Wno-unused-function)
target_link_libraries(melee_port PRIVATE wiilib::platform wiilib::gcm)

# Slippi Online's client (written for Slippi Nintendont's kernel, here on
# libogc): matchmaking, the connection to the opponent, and Slippi's EXI
# device, which the online codeset in the game talks to (wii/src/exi.c)
file(GLOB _slippi_sources CONFIGURE_DEPENDS
    "${_melee_root}/wii/src/slippi/*.c" "${_melee_root}/wii/src/slippi/enet/*.c"
    "${_melee_root}/wii/src/slippi/*.S")
add_library(melee_slippi STATIC ${_slippi_sources})
target_include_directories(melee_slippi PRIVATE
    ${_melee_root}/wii/src/slippi/shim
    ${_melee_root}/wii/src/slippi
    ${_melee_root}/wii/src/slippi/enet/include)
target_compile_definitions(melee_slippi PRIVATE ENET_IOS)
# (gecko_blobs.S: Slippi's codesets, from wii/slippi/gecko)
set_source_files_properties("${_melee_root}/wii/src/slippi/gecko_blobs.S" PROPERTIES
    COMPILE_OPTIONS "-Wa,-I${_melee_root}/wii/slippi/gecko"
    OBJECT_DEPENDS "${_melee_root}/wii/slippi/gecko/g_online_boot.bin")
target_compile_options(melee_slippi PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu99 -O2 -g -G0 -Wall
    -Wno-unused-parameter -Wno-unused-function>)
target_link_libraries(melee_slippi PRIVATE wiilib::platform)

# The launcher's screen (wii/src/launcher): its drawing, controls, screens,
# Slippi's files (VCDIFF patches), the disc's MD5
file(GLOB _launcher_sources CONFIGURE_DEPENDS "${_melee_root}/wii/src/launcher/*.c")

# The files that include libogc's headers instead of the SDK's
add_library(melee_port_ogc STATIC
    "${_melee_root}/wii/src/main.c"
    "${_melee_root}/wii/src/libc_compat.c"
    "${_melee_root}/wii/src/os_irq.c"
    "${_melee_root}/wii/src/cache.c"
    "${_melee_root}/wii/src/classic.c"
    "${_melee_root}/wii/src/mmu_track.c"
    "${_melee_root}/wii/src/copy.c"
    "${_melee_root}/wii/src/stall.c"
    "${_melee_root}/wii/src/wifi.c"
    "${_melee_root}/wii/src/diag_dirty.c"
    "${_melee_root}/wii/src/diag_watch.c"
    $<$<BOOL:${MELEE_ORIG_CODE}>:${_melee_root}/wii/src/gecko.c>
    $<$<BOOL:${MELEE_ORIG_CODE}>:${_melee_root}/wii/src/launcher.c>
    $<$<BOOL:${MELEE_ORIG_CODE}>:${_melee_root}/wii/src/launcher_settings.c>
    $<$<BOOL:${MELEE_ORIG_CODE}>:${_melee_root}/wii/src/overlay.c>
    $<$<BOOL:${MELEE_ORIG_CODE}>:${_melee_root}/wii/src/fptest.c>
    $<$<BOOL:${MELEE_ORIG_CODE}>:${_melee_root}/wii/src/fused.c>
    $<$<BOOL:${MELEE_ORIG_CODE}>:${_launcher_sources}>)
target_include_directories(melee_port_ogc PRIVATE ${_melee_root}/wii/src)
target_compile_options(melee_port_ogc PRIVATE -std=gnu17 -O2 -g -Wall)
target_compile_definitions(melee_port_ogc PRIVATE
    $<$<BOOL:${MELEE_ORIG_CODE}>:MELEE_ORIG_CODE>)
target_link_libraries(melee_port_ogc PRIVATE wiilib::platform)

set(_melee_ldscripts "${DEVKITPRO}/devkitPPC/powerpc-eabi/lib")
set(_melee_specs "${CMAKE_BINARY_DIR}/melee.specs")

if(MELEE_ORIG_CODE)
    # The decomp's DOL (its own build: python configure.py && ninja) where the
    # GameCube has it, the SDK's units the port replaces branching to the
    # port's, and the port above it (wii/tools/orig_image.py)
    set(_decomp_dol "${MELEE_DECOMP_ROOT}/build/GALE01/main.dol")
    set(_orig "${CMAKE_BINARY_DIR}/orig")
    set(_melee_layout "${_orig}/melee.ld")
    set(_keep_args)
    foreach(_unit IN LISTS _sdk_kept_units)
        list(APPEND _keep_args --keep ${_unit})
    endforeach()
    add_custom_command(
        OUTPUT "${_melee_layout}" "${_orig}/image.S" "${_orig}/tramp.S"
        COMMAND Python3::Interpreter "${_melee_root}/wii/tools/orig_image.py"
                "${_decomp_dol}"
                "${MELEE_DECOMP_ROOT}/config/GALE01/symbols.txt"
                "${MELEE_DECOMP_ROOT}/config/GALE01/splits.txt"
                "${_melee_ldscripts}" "${_orig}" ${_keep_args}
                --port "$<TARGET_FILE:melee_sdk>" --port "$<TARGET_FILE:melee_port>"
                --port "$<TARGET_FILE:melee_port_ogc>"
                --port "${DEVKITPRO}/libogc/lib/wii/libogc.a"
                --nm "${DEVKITPRO}/devkitPPC/bin/powerpc-eabi-nm"
                # (wii/src/gx_wii.c: what the Wii's GPU reads that the
                # GameCube's ignores)
                --override GXInitTlutObj
                # who turns interrupts off, noted (wii/src/os.c)
                # (wii/src/os.c)
                --override OSRestoreInterrupts
                --override OSDisableInterrupts
                --override OSEnableInterrupts
                # the port's test hook (wii/src/pad.c): the scene the game enters
                --redirect gm_801A4014:gm_FindGameSceneHandler:melee_wii_find_scene
                --redirect HSD_OSInit:OSCreateHeap:melee_wii_create_heap
                # 20XX's menu from the main menu (wii/src/mod_menu.c): the main
                # menu's frame, and the debug scene opening its menu
                --redirect mnMain_Scene_OnFrame:mn_8022F218:melee_wii_menu_frame
                --redirect gm_Scene_DebugMenu_OnEnter:un_80304168:melee_wii_menu_open
                --redirect un_80303AC4:un_803039A4:melee_wii_menu_stick
                # 20XX's "Disable all music" (wii/src/mod_songs.c)
                --redirect lbAudioAx_80023F28:fn_80023ED4:melee_wii_song_play
                # 20XX's shield colours (wii/src/mod_fighters.c)
                --redirect Fighter_LoadCommonData:lbArchive_LoadSymbols:melee_wii_fighters_loaded
                # 20XX's costumes: L and R on the character select screen
                --redirect mnCharSel_CursorThink:mnCharSel_CostumeChange:melee_wii_css_costume
                --redirect mnCharSel_Scene_OnFrame:lbDvd_80018254:melee_wii_css_preload
                # the memory card scene's question (a card with no save), not
                # asked in Slippi or a mod (wii/src/card.c)
                --redirect gm_Scene_MemCard_OnFrame:gm_801AEDC8:melee_wii_memcard_buttons
                --redirect gm_Scene_MemCard_OnFrame:gm_801AEE6C:melee_wii_memcard_message
                --redirect gm_801AF250:gm_801AEE6C:melee_wii_memcard_message
                --redirect gm_Scene_MemCard_OnFrame:gm_801AF0D4:melee_wii_memcard_choice
                --redirect gm_Scene_MemCard_OnFrame:lbAudioAx_80024030:melee_wii_memcard_sound
                # the boot's progressive scan question, answered by the Wii's
                # own setting (wii/src/vi.c)
                --redirect gm_801A4510:VIGetDTVStatus:melee_wii_boot_dtv_status
                # the texture copies, marked for rollback's page tracking
                # (wii/src/gx_wii.c)
                --redirect HSD_ShadowEndRender:GXCopyTex:melee_wii_copy_tex
                --redirect lbRefract_80022560:GXCopyTex:melee_wii_copy_tex
                --redirect HSD_ImageDescCopyFromEFB:GXCopyTex:melee_wii_copy_tex
                # the frame's wait, which turns interrupts on again should they
                # have been left off (wii/src/stall.c)
                --redirect lb_80019894:OSDisableInterrupts:melee_wii_frame_wait_irq_off
                --elf "${MELEE_DECOMP_ROOT}/build/GALE01/main.elf"
        DEPENDS "${_melee_root}/wii/tools/orig_image.py" "${_decomp_dol}"
                "${MELEE_DECOMP_ROOT}/config/GALE01/symbols.txt"
                "${MELEE_DECOMP_ROOT}/config/GALE01/splits.txt"
                "${_melee_ldscripts}/libogc_common.ld"
                melee_sdk melee_port melee_port_ogc
        COMMENT "Placing the game's DOL and the port's entry points")
    add_custom_target(melee_layout DEPENDS "${_melee_layout}")

    add_executable(melee $<TARGET_OBJECTS:melee_port_ogc>
        "${_orig}/image.S" "${_orig}/tramp.S" "${_melee_root}/wii/src/boot_mem2.S")
    target_link_libraries(melee PRIVATE
        -Wl,--start-group
        melee_sdk melee_port melee_port_ogc melee_slippi
        -Wl,--end-group
        wiilib::platform wiilib::gcm)
    target_link_options(melee PRIVATE
        -Wl,-Map,${CMAKE_BINARY_DIR}/melee.map
        -Wl,--wrap=vsnprintf
        # (the port runs in MEM2, the game in MEM1: the linker adds long
        # branches where the port's code calls the game's)
        -Wl,--relax
        -Wl,--no-gc-sections)
else()
        set_source_files_properties("${_melee_root}/src/melee/gm/gmmain.c" PROPERTIES
        COMPILE_DEFINITIONS "main=melee_main")

    add_executable(melee
        $<TARGET_OBJECTS:melee_port_ogc>)
    target_link_libraries(melee PRIVATE
        -Wl,--start-group
        melee_game melee_sdk melee_port melee_port_ogc melee_slippi
        -Wl,--end-group
        wiilib::platform wiilib::gcm)
    target_link_options(melee PRIVATE -Wl,-Map,${CMAKE_BINARY_DIR}/melee.map
        -Wl,--wrap=vsnprintf)

    # The game's data where the GameCube's DOL has it (wii/tools/data_layout.py):
    # the sources rely on its order and its alignment. The script is devkitPPC's
    # rvl.ld with the game's sections added; melee.specs has GCC link with it.
    set(_melee_layout "${CMAKE_BINARY_DIR}/melee.ld")
    add_custom_command(
        OUTPUT "${_melee_layout}"
        COMMAND Python3::Interpreter "${_melee_root}/wii/tools/data_layout.py"
                "${_melee_root}/config/GALE01/symbols.txt"
                "${_melee_root}/config/GALE01/splits.txt"
                "${_melee_ldscripts}" "${_melee_layout}"
                --objects "$<TARGET_FILE:melee_game>"
                --readelf "${DEVKITPRO}/devkitPPC/bin/powerpc-eabi-readelf"
        DEPENDS "${_melee_root}/wii/tools/data_layout.py" melee_game
                "${_melee_root}/config/GALE01/symbols.txt"
                "${_melee_root}/config/GALE01/splits.txt"
                "${_melee_ldscripts}/rvl.ld" "${_melee_ldscripts}/libogc_common.ld"
        COMMENT "Laying out the game's data")
    add_custom_target(melee_layout DEPENDS "${_melee_layout}")
endif()

file(WRITE "${_melee_specs}" "*link_start_rvl:\n-T ${_melee_layout}\n")
add_dependencies(melee melee_layout)
target_link_options(melee PRIVATE "-specs=${_melee_specs}")
set_property(TARGET melee APPEND PROPERTY LINK_DEPENDS "${_melee_layout}")
ogc_create_dol(melee)
