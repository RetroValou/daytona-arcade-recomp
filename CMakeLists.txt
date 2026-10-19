cmake_minimum_required(VERSION 3.20)
project(daytona_recomp LANGUAGES C CXX)
if(IOS)
    enable_language(OBJCXX)
endif()

# Mobile frontends use the same runtime/app code as desktop. Android needs
# SDL's shared JNI library; iOS uses the static library in an app bundle.
set(M2_MOBILE OFF)
if(ANDROID OR IOS)
    set(M2_MOBILE ON)
endif()
set(M2_GEN_ROOT "${CMAKE_BINARY_DIR}/gen" CACHE PATH
    "Directory containing recompiled daytona93/daytona93_tgp/daytona93_snd sources")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # /bigobj: the generated game code has many labels per object file
    add_compile_options(/W4 /fp:strict /bigobj /utf-8)
else()
    # No -ffast-math, ever (rules.md): exact rounding is the point. No FP
    # contraction either: a fused multiply-add (ARM64 always has one) rounds
    # once where the TGP's code rounds twice.
    add_compile_options(-Wall -Wextra -fno-fast-math -ffp-contract=off)
endif()

# Berkeley SoftFloat 3e (scripts/fetch_softfloat.sh): the extF80 reference
# model for i960 FP. 8086-SSE specialisation, FAST_INT64 build.
set(SOFTFLOAT_DIR ${CMAKE_SOURCE_DIR}/extern/softfloat)
include(cmake/softfloat_sources.cmake)
add_library(softfloat STATIC ${SOFTFLOAT_SOURCES})
# SoftFloat's GCC platform header needs __int128, __builtin_clz and GNU inline
# rules, which MSVC rejects (and clang-cl's are not GCC's). Those compilers get
# our portable one (cmake/softfloat-portable): same results, plain functions.
# M2_SOFTFLOAT_PORTABLE forces it anywhere, to test that path off Windows.
option(M2_SOFTFLOAT_PORTABLE "Build SoftFloat without GCC extensions" OFF)
if(MSVC OR M2_SOFTFLOAT_PORTABLE)
    set(SOFTFLOAT_PLATFORM ${CMAKE_SOURCE_DIR}/cmake/softfloat-portable)
    set(SOFTFLOAT_INLINE)
else()
    set(SOFTFLOAT_PLATFORM ${SOFTFLOAT_DIR}/build/Linux-x86_64-GCC)
    set(SOFTFLOAT_INLINE INLINE_LEVEL=5)
endif()
target_include_directories(softfloat
    PUBLIC ${SOFTFLOAT_DIR}/source/include
    PRIVATE ${SOFTFLOAT_PLATFORM} ${SOFTFLOAT_DIR}/source/8086-SSE)
target_compile_definitions(softfloat PRIVATE
    SOFTFLOAT_ROUND_ODD ${SOFTFLOAT_INLINE} SOFTFLOAT_FAST_DIV32TO16 SOFTFLOAT_FAST_DIV64TO32)
# FAST_INT64 selects the by-value extF80 API and LITTLEENDIAN the field order of
# extFloat80_t in the public headers, so users must see both (a mismatch silently
# swaps signExp and signif across the call boundary). Little-endian hosts only.
# Rounding mode and flags are globals in SoftFloat: make them per thread.
# C++ users get __thread, not thread_local: on macOS an extern thread_local is
# reached through a C++ wrapper function the C definitions never emit. MSVC and
# clang-cl (Windows TLS has no such wrapper) use __declspec(thread) in C, which
# both accept where MSVC's C may not take _Thread_local, and thread_local in C++.
if(MSVC)
    set(SOFTFLOAT_TLS_C "__declspec(thread)")
    set(SOFTFLOAT_TLS_CXX thread_local)
else()
    set(SOFTFLOAT_TLS_C _Thread_local)
    set(SOFTFLOAT_TLS_CXX __thread)
endif()
target_compile_definitions(softfloat PUBLIC SOFTFLOAT_FAST_INT64 LITTLEENDIAN=1
    $<$<COMPILE_LANGUAGE:C>:THREAD_LOCAL=${SOFTFLOAT_TLS_C}>
    $<$<COMPILE_LANGUAGE:CXX>:THREAD_LOCAL=${SOFTFLOAT_TLS_CXX}>)
set_target_properties(softfloat PROPERTIES C_STANDARD 11)
if(NOT MSVC)
    target_compile_options(softfloat PRIVATE -w) # upstream code, not ours
endif()

add_library(i960 STATIC
    src/i960/isa.cpp
    src/i960/decode.cpp
    src/i960/reach.cpp
    src/i960/fp.cpp
)
target_include_directories(i960 PUBLIC src)
target_link_libraries(i960 PUBLIC softfloat)

add_library(trace STATIC src/trace/trace.cpp)
target_include_directories(trace PUBLIC src)

# Runtime the recompiled game runs on: i960 context and services (call/ret,
# interrupts, memory; MAME's semantics, transplanted, BSD-3), replay bus and
# lockstep. No instruction interpreter.
# The ROM set the game is recompiled from (MAME's name, daytona93 by
# default). One set per build directory: its generated code, seeds and saves
# are its own. A set is known when it has a seed file (seeds/<set>.txt), a
# record in src/games/ and an entry in scripts/games.py
# (docs/adding-a-game.md).
set(M2_ROMSET daytona93 CACHE STRING "MAME ROM set to recompile (scripts/games.py lists them)")
if(NOT M2_ROMSET MATCHES "^[a-z0-9_]+$" OR NOT EXISTS ${CMAKE_SOURCE_DIR}/seeds/${M2_ROMSET}.txt)
    message(FATAL_ERROR "M2_ROMSET ${M2_ROMSET}: no seeds/${M2_ROMSET}.txt; known sets are listed in scripts/games.py")
endif()
# The game the set belongs to (scripts/games.py: daytona, segarally), which
# recompile.py and setup.py pass: the desktop executable is named after it.
set(M2_GAME daytona CACHE STRING "The game (scripts/games.py), the desktop executable's name")
if(NOT M2_GAME MATCHES "^[a-z0-9_]+$")
    message(FATAL_ERROR "M2_GAME ${M2_GAME}: lower-case letters, digits and _ only (scripts/games.py)")
endif()

add_library(runtime STATIC
    src/runtime/cpu/cpu.cpp
    src/runtime/tgp/tgp.cpp
    src/runtime/tgp/tgp_profile.cpp
    src/runtime/cpu/i960_profile.cpp
    src/runtime/tgp/tgp_board.cpp
    src/runtime/video/geo.cpp
    src/runtime/video/raster.cpp
    src/runtime/video/video.cpp
    src/runtime/board/m2_board.cpp
    src/runtime/board/game_loop.cpp
    src/runtime/romset/zip.cpp
    src/runtime/romset/rom_import.cpp
    src/runtime/romset/archive.cpp
    src/runtime/cpu/replay_bus.cpp
    src/runtime/cpu/lockstep.cpp
    src/runtime/sound/snd_cpu.cpp
    src/runtime/sound/multipcm.cpp
    src/runtime/sound/model1_board.cpp
    src/runtime/sound/native/native_sample_mixer.cpp
    src/runtime/sound/native/native_sound_sequencer.cpp
    src/runtime/sound/native/native_sound_engine.cpp
    src/runtime/sound/scsp.cpp
    src/runtime/sound/scsp_board.cpp
    src/runtime/io/comm_board.cpp
    # every game's record and hooks (runtime/board/game.h)
    src/games/games.cpp
    src/games/daytona.cpp
    src/games/srallyc.cpp
    src/games/vf2.cpp
    src/games/vcop.cpp
    src/games/vcop2.cpp
    src/games/manxttc.cpp
    src/games/fvipers.cpp
)
target_include_directories(runtime PUBLIC src)
target_compile_definitions(runtime PUBLIC M2_ROMSET="${M2_ROMSET}")
# TGP profiling (src/runtime/tgp/tgp_profile.h; recompile.py --tgp-profile): needs TGP
# code generated with m2tgprecomp --profile. m2run prints the report.
option(M2_TGP_PROFILE "TGP time, work and hot blocks (m2tgprecomp --profile code)" OFF)
if(M2_TGP_PROFILE)
    target_compile_definitions(runtime PUBLIC M2_TGP_PROFILE=1)
endif()
# The game's wait loop skipped by whole passes (Game::spin, M2Board::spin_skip): the
# same state, fewer host instructions. On by default; OFF runs every pass.
option(M2_SPIN_SKIP "Skip whole passes of the game's wait loop (same results)" ON)
if(M2_SPIN_SKIP)
    target_compile_definitions(runtime PUBLIC M2_SPIN_SKIP=1)
endif()
# The SCSP's output as MAME computes it (snd::Scsp::kSteppedDecay, kDspReturnPercent):
# for comparisons with MAME. Off by default: stepped decay, the DSP at 25 %.
option(M2_SCSP_MAME "SCSP sound identical to MAME's (no stepped decay, the DSP at 100 %)" OFF)
if(M2_SCSP_MAME)
    target_compile_definitions(runtime PUBLIC M2_SCSP_MAME=1)
endif()
# i960 profiling (src/runtime/cpu/i960_profile.h; recompile.py --i960-profile): needs i960
# code generated with m2recomp --profile. m2run prints the report.
option(M2_I960_PROFILE "i960 time, work, bus calls and hot blocks (m2recomp --profile code)" OFF)
if(M2_I960_PROFILE)
    target_compile_definitions(runtime PUBLIC M2_I960_PROFILE=1)
endif()
# The bus inlined in the i960 code (src/runtime/cpu/i960_gen_support.h gen::BoardMem; recompile.py
# --inline-bus): needs code generated with m2recomp --board. Off everywhere by default:
# no measurable gain on the Vita (HANDOFF.md, 15 Oct 2026).
option(M2_INLINE_BUS "i960 code with the board's bus inlined (m2recomp --board code)" OFF)
if(M2_INLINE_BUS)
    target_compile_definitions(runtime PUBLIC M2_INLINE_BUS=1)
endif()
# 7z ROM sets: the 7-Zip LZMA SDK's decoder (public-domain files only),
# fetched by scripts/setup.py into extern/lzma.
set(LZMA_DIR ${CMAKE_SOURCE_DIR}/extern/lzma/C)
if(EXISTS ${LZMA_DIR}/7zArcIn.c)
    add_library(lzma7z STATIC
        ${LZMA_DIR}/7zArcIn.c ${LZMA_DIR}/7zDec.c ${LZMA_DIR}/7zBuf.c ${LZMA_DIR}/7zCrc.c ${LZMA_DIR}/7zCrcOpt.c
        ${LZMA_DIR}/7zStream.c ${LZMA_DIR}/7zAlloc.c ${LZMA_DIR}/Alloc.c ${LZMA_DIR}/LzmaDec.c ${LZMA_DIR}/Lzma2Dec.c
        ${LZMA_DIR}/Bra.c ${LZMA_DIR}/Bra86.c ${LZMA_DIR}/BraIA64.c ${LZMA_DIR}/Bcj2.c ${LZMA_DIR}/Delta.c
        ${LZMA_DIR}/CpuArch.c ${LZMA_DIR}/Ppmd7.c ${LZMA_DIR}/Ppmd7Dec.c)
    target_include_directories(lzma7z SYSTEM PUBLIC ${LZMA_DIR})
    target_compile_definitions(lzma7z PRIVATE Z7_PPMD_SUPPORT) # 7z's PPMd method too, not only LZMA/LZMA2
    if(NOT MSVC)
        target_compile_options(lzma7z PRIVATE -w)
    endif()
    target_link_libraries(runtime PUBLIC lzma7z)
    target_compile_definitions(runtime PRIVATE M2_HAVE_7Z)
else()
    message(STATUS "extern/lzma not found: 7z ROM sets unsupported (run scripts/setup.py)")
endif()
target_link_libraries(runtime PUBLIC trace i960)

# YM3438 (the sound board's FM chip): ymfm, BSD-3, fetched by
# scripts/setup.py into extern/ymfm.
set(YMFM_DIR ${CMAKE_SOURCE_DIR}/extern/ymfm/src)
if(NOT EXISTS ${YMFM_DIR}/ymfm_opn.cpp)
    message(FATAL_ERROR "extern/ymfm not found: run scripts/setup.py")
endif()
add_library(ymfm STATIC ${YMFM_DIR}/ymfm_opn.cpp ${YMFM_DIR}/ymfm_adpcm.cpp ${YMFM_DIR}/ymfm_ssg.cpp)
target_include_directories(ymfm SYSTEM PUBLIC ${YMFM_DIR})
if(NOT MSVC)
    target_compile_options(ymfm PRIVATE -w)
endif()
target_link_libraries(runtime PUBLIC ymfm)

# Reference interpreter (MAME's i960 executor, transplanted, BSD-3). Test
# oracle only: m2replay links it; the game never does.
add_library(refcore STATIC src/refcore/i960_ref.cpp)
target_link_libraries(refcore PUBLIC runtime)
if(NOT MSVC)
    # Upstream MAME code: keep its own warnings out of ours.
    set_source_files_properties(src/runtime/cpu/cpu.cpp src/refcore/i960_ref.cpp src/runtime/video/geo.cpp PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-sign-compare;-Wno-unused-but-set-variable;-Wno-attributes")
endif()

# Model 2B's SHARC: MAME's interpreter lifted (src/runtime/sharc, BSD-3;
# THIRD_PARTY.md), its warnings upstream's; and its check against MAME's log.
add_library(sharc STATIC src/runtime/sharc/sharc.cpp)
target_include_directories(sharc PUBLIC src)
if(NOT MSVC)
    target_compile_options(sharc PRIVATE -w)
endif()
add_executable(m2sharccheck tools/m2sharccheck/main.cpp)
target_link_libraries(m2sharccheck PRIVATE sharc)
add_executable(m2sharcrecomp tools/m2sharcrecomp/main.cpp)

add_executable(m2replay tools/m2replay/main.cpp)
target_link_libraries(m2replay PRIVATE refcore)

# The recompiler (ours; committed) and, when it has been run, its output.
add_executable(m2recomp tools/m2recomp/main.cpp)
target_link_libraries(m2recomp PRIVATE i960)

# The sound CPU (68000) decoder and static recompiler.
add_library(m68k STATIC src/m68k/decode.cpp src/m68k/reach.cpp)
target_include_directories(m68k PUBLIC src)
add_executable(m2sndrecomp tools/m2sndrecomp/main.cpp)
target_link_libraries(m2sndrecomp PRIVATE m68k)

# The windowed app: SDL3 (window, input, SDL_GPU: Vulkan, Direct3D 12, Metal),
# fetched by scripts/setup.py into extern/sdl3, Dear ImGui for the launcher,
# and the app's own code. Built whenever SDL is there, ROM set or not, so CI
# compiles all of it; the daytona executable itself also needs the generated
# game code (below).
if(EXISTS ${CMAKE_SOURCE_DIR}/extern/sdl3/CMakeLists.txt)
    if(ANDROID)
        # SDLActivity loads libSDL3.so before the game's libmain.so.
        set(M2_SDL_TARGET SDL3::SDL3-shared)
        set(SDL_SHARED ON CACHE BOOL "" FORCE)
        set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    else()
        set(M2_SDL_TARGET SDL3::SDL3-static)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
    endif()
    if(NOT TARGET ${M2_SDL_TARGET})
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
        # SDL builds with its own flags, not ours: MSVC's /fp:strict (our FP
        # rule, for our code) rejects SDL's float constant tables (C2099).
        get_directory_property(M2_COMPILE_OPTIONS COMPILE_OPTIONS)
        set_directory_properties(PROPERTIES COMPILE_OPTIONS "")
        add_subdirectory(${CMAKE_SOURCE_DIR}/extern/sdl3 ${CMAKE_BINARY_DIR}/sdl3 EXCLUDE_FROM_ALL SYSTEM)
        set_directory_properties(PROPERTIES COMPILE_OPTIONS "${M2_COMPILE_OPTIONS}")
    endif()

    # Dear ImGui (launcher UI), with its SDL3 and SDL_GPU backends
    if(NOT ANDROID AND NOT IOS)
        add_executable(test_gpu_startup tests/app/test_gpu_startup.cpp)
        target_include_directories(test_gpu_startup PRIVATE src ${CMAKE_SOURCE_DIR}/extern/sdl3/include)
        add_test(NAME gpu_startup COMMAND test_gpu_startup)
    endif()
    set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/extern/imgui)
    add_library(imgui STATIC
        ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/imgui_demo.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp ${IMGUI_DIR}/backends/imgui_impl_sdlgpu3.cpp)
    target_include_directories(imgui SYSTEM PUBLIC ${IMGUI_DIR})
    target_link_libraries(imgui PUBLIC ${M2_SDL_TARGET})
    if(NOT MSVC)
        target_compile_options(imgui PRIVATE -w)
    endif()
    # The hardware renderer (SDL_GPU), shared by the app and m2gpushot.
    add_library(gpurender STATIC src/app/gpu/gpu_renderer.cpp)
    target_include_directories(gpurender PUBLIC src)
    target_link_libraries(gpurender PUBLIC runtime ${M2_SDL_TARGET})
    add_library(daytona_app OBJECT src/app/main.cpp src/app/launcher.cpp src/app/config.cpp src/app/controls.cpp src/app/ffb.cpp src/app/link_socket.cpp src/app/pacing.cpp)
    target_link_libraries(daytona_app PRIVATE runtime imgui gpurender ${M2_SDL_TARGET})
    if(WIN32)
        target_link_libraries(daytona_app PUBLIC ws2_32) # link play's sockets
    endif()
    if(IOS)
        target_sources(daytona_app PRIVATE platform/mobile_unmaintained/ios/rom_picker.mm)
        set_source_files_properties(platform/mobile_unmaintained/ios/rom_picker.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
        target_include_directories(daytona_app PRIVATE platform/mobile_unmaintained/ios)
    endif()
    if(M2_MOBILE)
        target_compile_definitions(daytona_app PRIVATE M2_MOBILE=1)
    endif()
endif()

# Recompiled game code lives in build/gen/<set>/ (derived from the game: never
# committed). scripts/recompile.sh generates it and re-runs CMake.
set(GEN_DIR ${M2_GEN_ROOT}/${M2_ROMSET})
set(TGP_GEN ${M2_GEN_ROOT}/${M2_ROMSET}_tgp/tgp_gen.cpp)
set(SND_GEN ${M2_GEN_ROOT}/${M2_ROMSET}_snd/snd_gen.cpp)
# The SCSP DSP program, recompiled (m2dsprecomp; optional: without it, or for a set
# with no program, the DSP interpreter runs).
set(DSP_GEN ${M2_GEN_ROOT}/${M2_ROMSET}_dsp/dsp_gen.cpp)
# Model 2B: the recompiled SHARC program (m2sharcrecomp; it compiles the lifted
# core in), and m2sharccheck on it.
set(SHARC_GEN ${M2_GEN_ROOT}/${M2_ROMSET}_sharc/sharc_gen.cpp)
if(EXISTS ${SHARC_GEN})
    add_executable(m2sharccheck_gen tools/m2sharccheck/main.cpp ${SHARC_GEN})
    target_include_directories(m2sharccheck_gen PRIVATE src)
    target_compile_definitions(m2sharccheck_gen PRIVATE M2SHARC_GEN)
    if(NOT MSVC)
        set_source_files_properties(${SHARC_GEN} PROPERTIES COMPILE_OPTIONS -w)
    endif()
endif()
if(EXISTS ${GEN_DIR}/gen_table.cpp AND EXISTS ${TGP_GEN} AND EXISTS ${SND_GEN})
    file(GLOB GEN_SOURCES ${GEN_DIR}/*.cpp)
    if(EXISTS ${DSP_GEN})
        list(APPEND GEN_SOURCES ${DSP_GEN})
    endif()
    add_executable(m2native tools/m2native/main.cpp ${GEN_SOURCES} ${TGP_GEN} ${SND_GEN})
    target_link_libraries(m2native PRIVATE runtime)
    # The game on its own: native board runtime, no trace.
    add_executable(m2run tools/m2run/main.cpp src/app/link_socket.cpp ${GEN_SOURCES} ${TGP_GEN} ${SND_GEN})
    target_link_libraries(m2run PRIVATE runtime)
    if(WIN32)
        target_link_libraries(m2run PRIVATE ws2_32) # link play's sockets
    endif()

    # The game in a window (needs the app objects below).
    if(TARGET daytona_app)
        if(ANDROID)
            # SDLActivity loads this as libmain.so and calls SDL_main from it.
            add_library(daytona SHARED $<TARGET_OBJECTS:daytona_app> ${GEN_SOURCES} ${TGP_GEN} ${SND_GEN})
            set_target_properties(daytona PROPERTIES OUTPUT_NAME main)
        elseif(IOS)
            add_executable(daytona MACOSX_BUNDLE $<TARGET_OBJECTS:daytona_app> ${GEN_SOURCES} ${TGP_GEN} ${SND_GEN})
            set_target_properties(daytona PROPERTIES
                MACOSX_BUNDLE_BUNDLE_NAME "Daytona USA"
                MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/platform/mobile_unmaintained/ios/Info.plist.in"
                MACOSX_BUNDLE_GUI_IDENTIFIER "com.boucydesigns.daytona"
                MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
                MACOSX_BUNDLE_BUNDLE_VERSION "1"
                XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.boucydesigns.daytona"
                XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
                XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "15.0"
                XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO")
        else()
            add_executable(daytona WIN32 $<TARGET_OBJECTS:daytona_app> ${GEN_SOURCES} ${TGP_GEN} ${SND_GEN})
            set_target_properties(daytona PROPERTIES OUTPUT_NAME ${M2_GAME}) # build_<game>/pc/<set>/<game>
        endif()
        target_link_libraries(daytona PRIVATE runtime imgui gpurender ${M2_SDL_TARGET})
        if(WIN32)
            target_link_libraries(daytona PRIVATE ws2_32) # link play's sockets
        endif()
        # Headless frames through the hardware renderer, to compare with m2run's.
        add_executable(m2gpushot tools/m2gpushot/main.cpp ${GEN_SOURCES} ${TGP_GEN} ${SND_GEN})
        target_link_libraries(m2gpushot PRIVATE runtime gpurender)
    else()
        message(STATUS "extern/sdl3 not found: skipping the windowed game (run scripts/setup.py)")
    endif()
    if(NOT MSVC)
        set_source_files_properties(${GEN_SOURCES} ${TGP_GEN} ${SND_GEN} PROPERTIES COMPILE_OPTIONS "-Wno-unused-label")
    endif()
endif()

# The TGP recompiler and, when it has been run, the lockstep check of its
# output against MAME's TGP log.
# The ROM set importer (zip or 7z) for the build.
add_executable(m2import tools/m2import/main.cpp)
target_link_libraries(m2import PRIVATE runtime)

# The SCSP DSP program recompiler (Game::dsp, m2import's dsp_program.bin).
add_executable(m2dsprecomp tools/m2dsprecomp/main.cpp)

add_executable(m2tgprecomp tools/m2tgprecomp/main.cpp)
target_include_directories(m2tgprecomp PRIVATE src)
if(EXISTS ${TGP_GEN})
    add_executable(m2tgpcheck tools/m2tgpcheck/main.cpp ${TGP_GEN})
    target_link_libraries(m2tgpcheck PRIVATE runtime)
    # The per-instruction hook exists only in this test build.
    target_compile_definitions(m2tgpcheck PRIVATE M2TGP_WITH_HOOK)
    if(NOT MSVC)
        set_source_files_properties(${TGP_GEN} PROPERTIES COMPILE_OPTIONS "-Wno-unused-label")
    endif()
endif()

# The lockstep check of the recompiled sound 68000 against MAME's log
# (M2TRACE_SNDLOG, patches/mame/0003).
if(EXISTS ${SND_GEN})
    add_executable(m2sndcheck tools/m2sndcheck/main.cpp ${SND_GEN})
    target_link_libraries(m2sndcheck PRIVATE runtime)
    if(NOT MSVC)
        set_source_files_properties(${SND_GEN} PROPERTIES COMPILE_OPTIONS "-Wno-unused-label")
    endif()
endif()

add_executable(i960dis tools/i960dis/main.cpp)
target_link_libraries(i960dis PRIVATE i960)

add_executable(tracediff tools/tracediff/main.cpp)
target_link_libraries(tracediff PRIVATE trace)

add_executable(tracedev tools/tracedev/main.cpp)
target_link_libraries(tracedev PRIVATE trace)

add_executable(traceinfo tools/traceinfo/main.cpp)
target_link_libraries(traceinfo PRIVATE trace)

enable_testing()

# The DSP recompiler against MAME's interpreter, on synthetic 128-step programs
# (m2dsprecomp --random, no ROM): every field, every sample's whole state; and one
# that stops half way (an input index past 0x31: the interpreter's early return).
foreach(kind random random-return)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/test_dsp_gen_${kind}.cpp
                       COMMAND m2dsprecomp --${kind} 20261015 128 ${CMAKE_BINARY_DIR}/test_dsp_gen_${kind}.cpp
                       DEPENDS m2dsprecomp VERBATIM)
    add_executable(test_scsp_dsp_${kind} tests/runtime/sound/test_scsp_dsp.cpp ${CMAKE_BINARY_DIR}/test_dsp_gen_${kind}.cpp)
    target_link_libraries(test_scsp_dsp_${kind} PRIVATE runtime)
    add_test(NAME scsp_dsp_${kind} COMMAND test_scsp_dsp_${kind})
endforeach()

# RateConv (the sound boards' instruction count to samples) against count * N / D.
add_executable(test_rate_conv tests/runtime/board/test_rate_conv.cpp)
target_include_directories(test_rate_conv PRIVATE src)
add_test(NAME rate_conv COMMAND test_rate_conv)

# The DSP's PACK against MAME's loop, on every 24-bit value.
add_executable(test_scsp_pack tests/runtime/sound/test_scsp_pack.cpp)
target_include_directories(test_scsp_pack PRIVATE src)
add_test(NAME scsp_pack COMMAND test_scsp_pack)

# Geometrizer clipping and display-list streaming (shared runtime code).
add_executable(test_geo_clip tests/runtime/video/test_geo_clip.cpp)
target_include_directories(test_geo_clip PRIVATE src)
add_test(NAME geo_clip COMMAND test_geo_clip)

add_executable(test_geo_stream tests/runtime/video/test_geo_stream.cpp)
target_include_directories(test_geo_stream PRIVATE src)
add_test(NAME geo_stream COMMAND test_geo_stream)

add_executable(test_decode tests/i960/test_decode.cpp)
target_link_libraries(test_decode PRIVATE i960)
add_test(NAME decode COMMAND test_decode)

add_executable(test_m68k tests/m68k/test_m68k.cpp)
target_link_libraries(test_m68k PRIVATE m68k runtime)
add_test(NAME m68k COMMAND test_m68k)

add_executable(test_fp tests/i960/test_fp.cpp)
target_link_libraries(test_fp PRIVATE i960)
find_package(Threads REQUIRED)
target_link_libraries(test_fp PRIVATE Threads::Threads)
add_test(NAME fp COMMAND test_fp)

add_executable(test_native_sample_mixer tests/runtime/sound/test_native_sample_mixer.cpp src/runtime/sound/native/native_sample_mixer.cpp)
target_include_directories(test_native_sample_mixer PRIVATE src)
add_test(NAME native_sample_mixer COMMAND test_native_sample_mixer)

add_executable(test_multipcm_volumes tests/runtime/sound/test_multipcm_volumes.cpp src/runtime/sound/multipcm.cpp)
target_include_directories(test_multipcm_volumes PRIVATE src)
add_test(NAME multipcm_volumes COMMAND test_multipcm_volumes)

add_executable(test_app_pacing tests/app/test_app_pacing.cpp src/app/pacing.cpp)
target_include_directories(test_app_pacing PRIVATE src)
add_test(NAME app_pacing COMMAND test_app_pacing)

add_executable(test_test_hold tests/runtime/board/test_test_hold.cpp)
target_include_directories(test_test_hold PRIVATE src)
add_test(NAME test_hold COMMAND test_test_hold)

add_executable(test_native_sound_sequencer tests/runtime/sound/test_native_sound_sequencer.cpp src/runtime/sound/native/native_sound_sequencer.cpp)
target_include_directories(test_native_sound_sequencer PRIVATE src)
add_test(NAME native_sound_sequencer COMMAND test_native_sound_sequencer)

# Link play's communication board, two boards wired in memory (no ROM, no network).
add_executable(test_comm_board tests/runtime/io/test_comm_board.cpp src/runtime/io/comm_board.cpp)
target_include_directories(test_comm_board PRIVATE src)
add_test(NAME comm_board COMMAND test_comm_board)

# Wheel and pedal bindings against SDL virtual joysticks.
if(TARGET SDL3::SDL3-static)
    add_executable(test_app_controls tests/app/test_app_controls.cpp src/app/controls.cpp src/app/ffb.cpp)
    target_include_directories(test_app_controls PRIVATE src)
    target_link_libraries(test_app_controls PRIVATE runtime SDL3::SDL3-static)
    add_test(NAME app_controls COMMAND test_app_controls)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/extern/sdl3/include/SDL3/SDL_audio.h)
    add_executable(test_app_native_audio tests/app/test_app_native_audio.cpp)
    target_include_directories(test_app_native_audio PRIVATE extern/sdl3/include)
    target_link_libraries(test_app_native_audio PRIVATE Threads::Threads)
    add_test(NAME app_native_audio COMMAND test_app_native_audio)
endif()

# PS Vita frontend tests: platform/vita code against narrow host mocks of the
# Vita SDK. ROM-free; they check CPU data and lifetimes, not GXM rendering or
# device frame rates. Off by default: not part of the desktop build.
option(M2_VITA_TESTS "Build the PS Vita frontend's host tests" OFF)
if(M2_VITA_TESTS)
    # The menu through the vitaGL backend, render/renderer.h mocked.
    add_executable(test_vita_imgui_gl tests/vita/test_vita_imgui_gl.cpp
        extern/imgui/imgui.cpp extern/imgui/imgui_draw.cpp
        extern/imgui/imgui_tables.cpp extern/imgui/imgui_widgets.cpp)
    target_include_directories(test_vita_imgui_gl PRIVATE . tests/vita/shims/gl tests/vita/shims/gpu src platform/vita extern/imgui)
    add_test(NAME vita_imgui_gl COMMAND test_vita_imgui_gl)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(test_vita_link tests/vita/test_vita_link.cpp src/app/link_socket.cpp src/runtime/io/comm_board.cpp)
        target_include_directories(test_vita_link PRIVATE tests/vita/shims/link src)
        add_test(NAME vita_link COMMAND test_vita_link)
    endif()
    add_executable(test_vita_controls tests/vita/test_vita_controls.cpp)
    add_executable(test_vita_core_policy tests/vita/test_vita_core_policy.cpp)
    add_test(NAME vita_core_policy COMMAND test_vita_core_policy)
    add_test(NAME vita_controls COMMAND test_vita_controls)
    # The tile cache's write tracking against comparing everything (video.cpp, Vita path).
    add_executable(test_vita_tile_cache tests/vita/test_vita_tile_cache.cpp src/runtime/video/video.cpp src/runtime/video/raster.cpp)
    target_include_directories(test_vita_tile_cache PRIVATE src)
    target_compile_definitions(test_vita_tile_cache PRIVATE M2_VITA_RENDER_OPT=1)
    add_test(NAME vita_tile_cache COMMAND test_vita_tile_cache)

    add_executable(test_vita_polygon_order tests/vita/test_vita_polygon_order.cpp)
    target_include_directories(test_vita_polygon_order PRIVATE platform/vita src)
    add_test(NAME vita_polygon_order COMMAND test_vita_polygon_order)

    # The multicore/vitaGL helpers: texel index, flat index, reference audio pacing.
    add_executable(test_vita_texel_index tests/vita/test_vita_texel_index.cpp)
    target_include_directories(test_vita_texel_index PRIVATE src)
    add_test(NAME vita_texel_index COMMAND test_vita_texel_index)
    add_executable(test_vita_flat_index tests/vita/test_vita_flat_index.cpp)
    add_test(NAME vita_flat_index COMMAND test_vita_flat_index)
    add_executable(test_vita_audio_rate tests/vita/test_vita_audio_rate.cpp)
    add_test(NAME vita_audio_rate COMMAND test_vita_audio_rate)

    add_executable(test_vita_native_audio tests/vita/test_vita_native_audio.cpp)
    target_include_directories(test_vita_native_audio PRIVATE tests/vita/shims/audio)
    target_link_libraries(test_vita_native_audio PRIVATE Threads::Threads)
    add_test(NAME vita_native_audio COMMAND test_vita_native_audio)

    add_executable(test_vita_sound_worker tests/vita/test_vita_sound_worker.cpp)
    target_include_directories(test_vita_sound_worker PRIVATE tests/vita/shims/audio)
    target_link_libraries(test_vita_sound_worker PRIVATE Threads::Threads)
    add_test(NAME vita_sound_worker COMMAND test_vita_sound_worker)

    add_executable(test_vita_audio_worker tests/vita/test_vita_audio_worker.cpp)
    target_include_directories(test_vita_audio_worker PRIVATE tests/vita/shims/audio)
    target_link_libraries(test_vita_audio_worker PRIVATE Threads::Threads)
    add_test(NAME vita_audio_worker COMMAND test_vita_audio_worker)

    add_executable(test_vita_async_log tests/vita/test_vita_async_log.cpp)
    target_include_directories(test_vita_async_log PRIVATE tests/vita/shims/log)
    target_compile_definitions(test_vita_async_log PRIVATE M2_ROMSET="${M2_ROMSET}") # the log goes to ux0:data/<set>/
    target_link_libraries(test_vita_async_log PRIVATE Threads::Threads)
    add_test(NAME vita_async_log COMMAND test_vita_async_log)
    set_tests_properties(vita_async_log PROPERTIES TIMEOUT 20)

    add_executable(test_vita_log tests/vita/test_vita_log.cpp)
    target_include_directories(test_vita_log PRIVATE tests/vita/shims/log)
    target_compile_definitions(test_vita_log PRIVATE M2_ROMSET="${M2_ROMSET}") # the log goes to ux0:data/<set>/
    add_test(NAME vita_log COMMAND test_vita_log)

    # The launcher's settings (vita.cfg) against the game records.
    add_executable(test_vita_settings tests/vita/test_vita_settings.cpp platform/vita/frontend/settings.cpp
        platform/vita/frontend/storage.cpp)
    target_link_libraries(test_vita_settings PRIVATE runtime)
    add_test(NAME vita_settings COMMAND test_vita_settings ${CMAKE_CURRENT_BINARY_DIR})

    # platform/vita is the include root of its own headers ("system/core_policy.h").
    foreach(t test_vita_settings test_vita_imgui_gl test_vita_controls test_vita_core_policy test_vita_polygon_order
              test_vita_texel_index test_vita_flat_index test_vita_audio_rate test_vita_native_audio
              test_vita_sound_worker test_vita_audio_worker test_vita_async_log test_vita_log)
        target_include_directories(${t} PRIVATE platform/vita)
    endforeach()
    if(TARGET test_vita_link)
        target_include_directories(test_vita_link PRIVATE platform/vita)
    endif()
endif()

# Measurement, not a test: where MAME's double FP disagrees with the model.
add_executable(fp_vs_mame tests/i960/fp_vs_mame.cpp)
target_link_libraries(fp_vs_mame PRIVATE i960 Threads::Threads)

add_executable(test_reach tests/i960/test_reach.cpp)
target_link_libraries(test_reach PRIVATE i960)
add_test(NAME reach COMMAND test_reach)

add_executable(test_trace tests/trace/test_trace.cpp)
target_link_libraries(test_trace PRIVATE trace)
add_test(NAME trace COMMAND test_trace ${CMAKE_CURRENT_BINARY_DIR})

# The MAME plugin's Lua core against the C++ trace library (needs lupa).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME lua_core
        COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tests/trace/lua_core_test.py
                ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR}/lua_core)
    set_tests_properties(lua_core PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME lua_plugin_mock
        COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tests/trace/lua_plugin_mock_test.py
                ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR}/lua_plugin_mock)
    set_tests_properties(lua_plugin_mock PROPERTIES SKIP_RETURN_CODE 77)
    # Second decode reference: Ghidra SLEIGH (third-party i960 module via
    # pypcode). Needs scripts/fetch_ghidra_i960.sh and pip install pypcode.
    if(EXISTS ${CMAKE_SOURCE_DIR}/extern/ghidra_i960/data/languages/i960.slaspec)
        add_test(NAME ghidra_oracle
            COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tests/oracles/ghidra_oracle.py
                    ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_SOURCE_DIR}/extern/ghidra_i960 --random 200000)
        set_tests_properties(ghidra_oracle PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

# Differential test against MAME's disassembler. Needs scripts/fetch_mame.sh.
set(MAME_DIR ${CMAKE_SOURCE_DIR}/extern/mame CACHE PATH "Pinned MAME checkout")
if(EXISTS ${MAME_DIR}/src/devices/cpu/i960/i960dis.cpp)
    add_executable(mame_oracle
        tests/oracles/mame_oracle.cpp
        ${MAME_DIR}/src/devices/cpu/i960/i960dis.cpp
    )
    target_include_directories(mame_oracle PRIVATE
        tests/oracles/mame_shim
        ${MAME_DIR}/src/devices
        ${MAME_DIR}/src/devices/cpu/i960
    )
    target_link_libraries(mame_oracle PRIVATE i960)
    find_package(Threads REQUIRED)
    target_link_libraries(mame_oracle PRIVATE Threads::Threads)
    # MAME's source, not ours: don't drown our warnings in its.
    set_source_files_properties(${MAME_DIR}/src/devices/cpu/i960/i960dis.cpp
        PROPERTIES COMPILE_OPTIONS "-w")
    add_test(NAME mame_oracle COMMAND mame_oracle)
else()
    message(STATUS "extern/mame not found: skipping mame_oracle (run scripts/fetch_mame.sh)")
endif()
