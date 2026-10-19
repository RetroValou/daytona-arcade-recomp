cmake_minimum_required(VERSION 3.20)
# A separate cross-build: never try to execute the ROM importers on the Vita.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(NOT DEFINED ENV{VITASDK})
        message(FATAL_ERROR "Set VITASDK to your VitaSDK installation first")
    endif()
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE FILEPATH "VitaSDK toolchain")
endif()
project(daytona_vita LANGUAGES C CXX)
if(NOT VITA)
    message(FATAL_ERROR "This directory requires the VitaSDK toolchain, not a desktop compiler")
endif()
if(NOT VITASDK)
    set(VITASDK "$ENV{VITASDK}")
endif()
include("${VITASDK}/share/vita.cmake")
get_filename_component(DAYTONA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -fno-fast-math -ffp-contract=off -ffunction-sections -fdata-sections)

option(DAYTONA_VITA_COMPILE_CHECK "Compile runtime and frontend without ROM-derived code; produces no VPK" OFF)
# The renderer is vitaGL (render/).
# Sources by role: frontend/ (main loop, launcher, controls), render/ (vitaGL),
# audio/, system/ (cores, profiling, logs), net/ (link), overrides/ (the shared
# runtime's geo.h/.cpp replaced). Includes name them from here: "system/core_policy.h".
set(VITA_RENDER_SOURCES render/renderer.cpp render/model2_textures.cpp render/model2_polygons.cpp
    render/layers_2d.cpp render/gl_context.cpp render/shaders.cpp render/draw_list.cpp render/ui_draw.cpp)
set(VITA_FRONTEND_SOURCES frontend/main.cpp frontend/launcher.cpp frontend/session.cpp frontend/diagnostics.cpp frontend/settings.cpp frontend/storage.cpp
    frontend/input.cpp
    ${VITA_RENDER_SOURCES})
option(DAYTONA_VITA_DIAGNOSTICS "GXM startup/performance file logging (faults always recorded)" OFF)
# Link-time optimization (scripts/build_vita.py --release): the generated i960/TGP
# code can inline the runtime's small memory and bus accessors (cpu.cpp,
# m2_board.cpp) instead of calling them across object files. Set before any target
# is created, so it covers the libraries, the frontend and the generated code.
option(DAYTONA_VITA_LTO "Build the whole VPK with GCC link-time optimization" OFF)
if(DAYTONA_VITA_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT DAYTONA_VITA_LTO_OK OUTPUT DAYTONA_VITA_LTO_ERROR LANGUAGES C CXX)
    if(DAYTONA_VITA_LTO_OK)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
        # The link step generates the code: same float rules and level as the compile.
        add_link_options(-O3 -fno-fast-math -ffp-contract=off)
        message(STATUS "Vita LTO: enabled")
    else()
        message(WARNING "Vita LTO: not supported by this toolchain, normal build.\n${DAYTONA_VITA_LTO_ERROR}")
    endif()
endif()
# One multicore flow: the geometrizer always runs pipelined on its own core (3D one
# frame late); vitaGL shows the 2D layers one frame late too, in phase. Application
# cores (0-2) of the busy threads; -1 =
# unpinned. DAYTONA_VITA_FREE_CORES (build_vita.py --free-core) leaves them all
# unpinned: they then follow the fourth-core setting (system/core_policy.h).
# Link play (net/link.h, SceNet): build_vita.py turns it on only for the games it
# was tested on (scripts/games.py vita_link: Daytona). Off: no link code, no
# link rows in the menu.
option(DAYTONA_VITA_LINK "Compile LAN link play in (net/link.h)" ON)
option(DAYTONA_VITA_FREE_CORES "Leave the main, geometry, sound and 2D threads unpinned (the 4th core can take them)" OFF)
set(DAYTONA_VITA_MAIN_CORE 0 CACHE STRING "Core of the main thread (i960, TGP, video, GPU submission)")
set(DAYTONA_VITA_GEO_CORE 1 CACHE STRING "Core of the geometry thread")
set(DAYTONA_VITA_SOUND_CORE 2 CACHE STRING "Core of the reference sound board worker")
set(DAYTONA_VITA_2D_CORE 2 CACHE STRING "Core of the vitaGL 2D worker (System 24 uploads + layer rectangles)")
foreach(role MAIN GEO SOUND 2D)
    if(DAYTONA_VITA_FREE_CORES)
        set(VITA_${role}_CORE -1)
    else()
        set(VITA_${role}_CORE ${DAYTONA_VITA_${role}_CORE})
    endif()
endforeach()
# Frontend and renderer only: the runtime and the game code are the same with or without it.
# DAYTONA_VITA_DIAGNOSTICS (build_vita.py --diagnostics) enables every log file:
# vita-diag.log periodic lines, perf.log (per-core report) and gl.log.
set_source_files_properties(${VITA_RENDER_SOURCES} PROPERTIES COMPILE_DEFINITIONS
    "DAYTONA_VITA_DIAGNOSTICS=$<BOOL:${DAYTONA_VITA_DIAGNOSTICS}>;DAYTONA_VITA_2D_CORE=${VITA_2D_CORE}")
set_source_files_properties(frontend/main.cpp frontend/launcher.cpp frontend/session.cpp frontend/diagnostics.cpp PROPERTIES COMPILE_DEFINITIONS
    "DAYTONA_VITA_DIAGNOSTICS=$<BOOL:${DAYTONA_VITA_DIAGNOSTICS}>;DAYTONA_VITA_MAIN_CORE=${VITA_MAIN_CORE};DAYTONA_VITA_GEO_CORE=${VITA_GEO_CORE};DAYTONA_VITA_SOUND_CORE=${VITA_SOUND_CORE};DAYTONA_VITA_LINK=$<BOOL:${DAYTONA_VITA_LINK}>")
set(DAYTONA_GEN_ROOT "${DAYTONA_ROOT}/build_daytona/recomp/daytona93/gen" CACHE PATH
    "Host-generated i960, TGP and 68000 C++ root (scripts/games.py gen_root: build_<game>/recomp/<set>/gen)")
# The game: any set of scripts/games.py (its seeds/<set>.txt must exist).
# build_vita.py passes the set, its package's title ID and name, and the
# companion set's file name from there.
set(DAYTONA_VITA_ROMSET "daytona93" CACHE STRING "Native game set (scripts/games.py)")
if(NOT DAYTONA_VITA_ROMSET MATCHES "^[a-z0-9_]+$" OR NOT EXISTS "${DAYTONA_ROOT}/seeds/${DAYTONA_VITA_ROMSET}.txt")
    message(FATAL_ERROR "Unknown Vita ROM set ${DAYTONA_VITA_ROMSET}: no seeds/${DAYTONA_VITA_ROMSET}.txt")
endif()
# A game's two sets in one package (Daytona): the companion set's SELF,
# bundled as <companion>.self, which the launcher switches to.
set(DAYTONA_VITA_REV_A_SELF "" CACHE FILEPATH "Optional companion set's SELF to bundle (Daytona: Revision A's)")
set(DAYTONA_VITA_COMPANION "daytona" CACHE STRING "The companion set bundled with DAYTONA_VITA_REV_A_SELF")
# One installed app per game, each with its own title ID (scripts/games.py),
# so several can be installed side by side.
set(DAYTONA_VITA_TITLEID "DAYT00093" CACHE STRING "Nine-character Vita title ID (scripts/games.py vita_title_id)")
set(DAYTONA_VITA_GAME_NAME "Daytona USA" CACHE STRING "The package's name (scripts/games.py vita_name)")
# The package's file name and its icon/LiveArea images, both the game's (build_vita.py:
# <game>.vpk or <game>_diag.vpk, platform/vita/logo_livearea/<game>/).
set(DAYTONA_VITA_VPK "daytona.vpk" CACHE STRING "The package's file name (scripts/games.py vpk_name)")
set(DAYTONA_VITA_LIVEAREA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/logo_livearea/daytona" CACHE PATH
    "The game's icon and LiveArea images (platform/vita/logo_livearea/<game>)")
if(NOT DAYTONA_VITA_VPK MATCHES "^[a-z0-9_]+\\.vpk$")
    message(FATAL_ERROR "DAYTONA_VITA_VPK must be <name>.vpk (lower case, digits, _), not ${DAYTONA_VITA_VPK}")
endif()
set(VITA_TITLEID "${DAYTONA_VITA_TITLEID}")
set(VITA_GAME_NAME "${DAYTONA_VITA_GAME_NAME}")
if(NOT VITA_TITLEID MATCHES "^[A-Z][A-Z][A-Z][A-Z][0-9][0-9][0-9][0-9][0-9]$")
    message(FATAL_ERROR "Vita title ID must be 4 capital letters and 5 digits, not ${VITA_TITLEID}")
endif()
if(NOT DAYTONA_VITA_COMPILE_CHECK)
    foreach(path ${DAYTONA_VITA_ROMSET}/gen_table.cpp ${DAYTONA_VITA_ROMSET}_tgp/tgp_gen.cpp ${DAYTONA_VITA_ROMSET}_snd/snd_gen.cpp)
        if(NOT EXISTS "${DAYTONA_GEN_ROOT}/${path}")
            message(FATAL_ERROR "Missing ${DAYTONA_GEN_ROOT}/${path}. Run scripts/recompile.py with the HOST compiler first; no game code ships in the repository.")
        endif()
    endforeach()
endif()

set(SOFTFLOAT_DIR "${DAYTONA_ROOT}/extern/softfloat")
set(YMFM_DIR "${DAYTONA_ROOT}/extern/ymfm/src")
foreach(path "${SOFTFLOAT_DIR}/source/softfloat_state.c" "${YMFM_DIR}/ymfm_opn.cpp")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Missing dependency ${path}. Run python3 scripts/setup.py --no-build on the host first.")
    endif()
endforeach()
include("${DAYTONA_ROOT}/cmake/softfloat_sources.cmake")
add_library(vita_softfloat STATIC ${SOFTFLOAT_SOURCES})
target_include_directories(vita_softfloat PUBLIC "${SOFTFLOAT_DIR}/source/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/softfloat" "${SOFTFLOAT_DIR}/source/8086-SSE")
target_compile_definitions(vita_softfloat PUBLIC SOFTFLOAT_FAST_INT64 LITTLEENDIAN=1 THREAD_LOCAL=
    PRIVATE SOFTFLOAT_ROUND_ODD INLINE_LEVEL=5 SOFTFLOAT_FAST_DIV32TO16 SOFTFLOAT_FAST_DIV64TO32)
# Only the main thread executes the board. The SDL callback consumes already
# converted samples and never calls SoftFloat. No Vita TLS runtime is required.
target_compile_options(vita_softfloat PRIVATE -w)

add_library(vita_i960 STATIC
    "${DAYTONA_ROOT}/src/i960/isa.cpp" "${DAYTONA_ROOT}/src/i960/decode.cpp"
    "${DAYTONA_ROOT}/src/i960/reach.cpp" "${DAYTONA_ROOT}/src/i960/fp.cpp")
target_include_directories(vita_i960 PUBLIC "${DAYTONA_ROOT}/src")
target_link_libraries(vita_i960 PUBLIC vita_softfloat)
add_library(vita_trace STATIC "${DAYTONA_ROOT}/src/trace/trace.cpp")
target_include_directories(vita_trace PUBLIC "${DAYTONA_ROOT}/src")
add_library(vita_ymfm STATIC "${YMFM_DIR}/ymfm_opn.cpp" "${YMFM_DIR}/ymfm_adpcm.cpp" "${YMFM_DIR}/ymfm_ssg.cpp")
target_include_directories(vita_ymfm SYSTEM PUBLIC "${YMFM_DIR}")
target_compile_options(vita_ymfm PRIVATE -w)

# The same native runtime as the desktop; no refcore or interpreter is linked.
# video/geo.cpp is replaced by its Vita version (platform/vita/overrides/runtime/video/geo.cpp),
# which compiles the original unchanged and runs it on its own core.
set(RUNTIME_FILES
    board/m2_board board/game_loop
    cpu/cpu cpu/i960_profile cpu/replay_bus cpu/lockstep
    tgp/tgp tgp/tgp_board tgp/tgp_profile
    video/raster video/video
    sound/snd_cpu sound/model1_board sound/multipcm sound/scsp sound/scsp_board
    sound/native/native_sample_mixer sound/native/native_sound_sequencer sound/native/native_sound_engine
    io/comm_board
    romset/zip romset/rom_import romset/archive)
set(RUNTIME_SOURCES)
foreach(name IN LISTS RUNTIME_FILES)
    list(APPEND RUNTIME_SOURCES "${DAYTONA_ROOT}/src/runtime/${name}.cpp")
endforeach()
foreach(name games daytona srallyc vf2 vcop vcop2 manxttc fvipers) # every game's record and hooks (src/games/)
    list(APPEND RUNTIME_SOURCES "${DAYTONA_ROOT}/src/games/${name}.cpp")
endforeach()
# Vita replacement of the geometrizer (platform/vita/overrides/runtime/video/geo.h/.cpp).
# Nothing in src/ is modified: platform/vita/overrides comes FIRST on the include
# path, so the shared runtime's "runtime/video/geo.h" resolves to the Vita one.
add_library(vita_runtime STATIC ${RUNTIME_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/overrides/runtime/video/geo.cpp")
target_compile_options(vita_runtime PRIVATE -fno-math-errno)
target_include_directories(vita_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/overrides" "${DAYTONA_ROOT}/src"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vita_runtime PUBLIC vita_trace vita_i960 vita_ymfm)
target_compile_definitions(vita_runtime PUBLIC M2_ROMSET="${DAYTONA_VITA_ROMSET}")
# Runtime support of the i960 code rewritten by scripts/fast_gen.py
# (scripts/build_vita.py --fast-gen: the whole pass; --fast-inaccuracy: its direct
# work-RAM accesses only, gen::wram_* and Cpu::work_ram). It also runs the code that is
# not rewritten (default or --fast_inaccuracy output) unchanged. PUBLIC: the generated
# code must see the same Cpu and Lockstep layouts as the runtime.
target_compile_definitions(vita_runtime PUBLIC M2_FAST_GEN=1)
# The shared runtime's fast paths from the Dreamcast port (src/runtime/board/fast_paths.h):
# bus decode of the TGP ports and remembered RAM pages, the TGP's FIFOs as a ring read
# directly, register frames copied through work RAM, the geometrizer's block reads and
# culled-polygon shortcuts. The same results; the Dreamcast's display-specific ones
# (16-bit layers, frame skip) stay under M2_DC_SPEED. OFF (build_vita.py
# --no-fast-paths) builds the runtime as the desktop does, for comparison.
option(DAYTONA_VITA_FAST_PATHS "Shared runtime fast paths (M2_FAST_BUS/TGP/CPU/GEO)" ON)
if(DAYTONA_VITA_FAST_PATHS)
    target_compile_definitions(vita_runtime PUBLIC M2_FAST_BUS=1 M2_FAST_TGP=1 M2_FAST_CPU=1 M2_FAST_GEO=1)
endif()
# TGP profiling (build_vita.py --tgp-profile, src/runtime/tgp/tgp_profile.h): a TGP section
# in perf.log (diagnostics builds); the TGP code is generated with m2tgprecomp --profile.
option(DAYTONA_VITA_TGP_PROFILE "TGP time, work and hot blocks in perf.log" OFF)
if(DAYTONA_VITA_TGP_PROFILE)
    target_compile_definitions(vita_runtime PUBLIC M2_TGP_PROFILE=1)
endif()
# i960 profiling (build_vita.py --i960-profile, src/runtime/cpu/i960_profile.h): an i960 section
# in perf.log (diagnostics builds); the i960 code is generated with m2recomp --profile.
option(DAYTONA_VITA_I960_PROFILE "i960 time, work, bus calls and hot blocks in perf.log" OFF)
if(DAYTONA_VITA_I960_PROFILE)
    target_compile_definitions(vita_runtime PUBLIC M2_I960_PROFILE=1)
endif()
# The game's wait loop skipped by whole passes (Game::spin, M2Board::spin_skip), as
# the desktop: the same state, fewer instructions run. ON by default.
option(DAYTONA_VITA_SPIN_SKIP "Skip whole passes of the game's wait loop (same results)" ON)
if(DAYTONA_VITA_SPIN_SKIP)
    target_compile_definitions(vita_runtime PUBLIC M2_SPIN_SKIP=1)
endif()
# Sound board probe (src/runtime/sound/sound_profile.h), diagnostics builds only: the
# 68000, SCSP render and DSP times and the 68000's dispatches and register
# accesses, in perf.log's core 2 section (~0.1 ms a frame of clock reads on core 2).
# The 68000 code's M2_SND_DISPATCH() counts only with it.
if(DAYTONA_VITA_DIAGNOSTICS)
    target_compile_definitions(vita_runtime PUBLIC M2_SOUND_PROFILE=1)
endif()
# The bus inlined in the i960 code (src/runtime/cpu/i960_gen_support.h gen::BoardMem): with
# build_vita.py --inline-bus, its fast modes' i960 code is generated with m2recomp --board
# (the set's board, scripts/games.py). The host's own code (the default mode) runs on it
# unchanged. OFF by default: no measurable gain on the Vita (HANDOFF.md, 15 Oct 2026).
option(DAYTONA_VITA_INLINE_BUS "i960 code with the board's bus inlined (M2_INLINE_BUS)" OFF)
if(DAYTONA_VITA_INLINE_BUS)
    target_compile_definitions(vita_runtime PUBLIC M2_INLINE_BUS=1)
endif()
option(DAYTONA_VITA_RENDER_OPT "Exact tile/layer caching and optimized CPU rasterizer" ON)
if(DAYTONA_VITA_RENDER_OPT)
    # Public because the flag affects Video/Raster class layouts.
    target_compile_definitions(vita_runtime PUBLIC M2_VITA_RENDER_OPT=1)
endif()
# ZIP uses the existing built-in decoder. 7z is deliberately excluded to avoid
# an unbounded solid-archive working set on the handheld. Convert on the host.

# Require the target SDK package. Do not accidentally pick up host SDL2.
find_package(SDL2 2.0.22 CONFIG REQUIRED PATHS "${VITASDK}/arm-vita-eabi/lib/cmake/SDL2" NO_DEFAULT_PATH)
if(TARGET SDL2::SDL2-static)
    set(VITA_SDL_TARGET SDL2::SDL2-static)
else()
    set(VITA_SDL_TARGET SDL2::SDL2)
endif()
# vitaGL + runtime shader compiler (vitashark). Needs libshacccg.suprx on the console.
# Preferred: the pinned vitaGL that scripts/setup_vitagl.py builds into extern/vitagl
# (release flags in platform/vita/README.md). Plain variables, not find_*() cache
# entries: running the script after a first configure needs no fresh build directory.
set(DAYTONA_VITAGL_DIR "${DAYTONA_ROOT}/extern/vitagl/install" CACHE PATH
    "vitaGL built by scripts/setup_vitagl.py; empty = the VitaSDK's own vitaGL")
set(VITAGL_LIBRARIES)
if(DAYTONA_VITAGL_DIR AND EXISTS "${DAYTONA_VITAGL_DIR}/lib/libvitaGL.a")
    set(VITAGL_INCLUDE_DIR "${DAYTONA_VITAGL_DIR}/include")
    foreach(lib vitaGL vitashark mathneon SceShaccCgExt)
        if(NOT EXISTS "${DAYTONA_VITAGL_DIR}/lib/lib${lib}.a")
            message(FATAL_ERROR "GPU GL: ${DAYTONA_VITAGL_DIR}/lib/lib${lib}.a is missing: "
                    "run python3 scripts/setup_vitagl.py --force")
        endif()
        list(APPEND VITAGL_LIBRARIES "${DAYTONA_VITAGL_DIR}/lib/lib${lib}.a")
    endforeach()
    list(APPEND VITAGL_LIBRARIES taihen_stub) # SceShaccCgExt (the script checks the SDK has it)
    include("${DAYTONA_VITAGL_DIR}/vitagl.cmake" OPTIONAL)
    message(STATUS "GPU GL: pinned vitaGL ${DAYTONA_VITAGL_COMMIT} (${DAYTONA_VITAGL_PROFILE}: "
            "${DAYTONA_VITAGL_FLAGS}) from ${DAYTONA_VITAGL_DIR}")
else()
    find_path(VITAGL_INCLUDE_DIR vitaGL.h PATHS "${VITASDK}/arm-vita-eabi/include" NO_DEFAULT_PATH)
    find_library(VITAGL_LIB_vitaGL vitaGL PATHS "${VITASDK}/arm-vita-eabi/lib" NO_DEFAULT_PATH)
    if(NOT VITAGL_LIB_vitaGL OR NOT VITAGL_INCLUDE_DIR)
        message(FATAL_ERROR "GPU GL: no vitaGL. Run python3 scripts/setup_vitagl.py (pinned build "
                "into extern/vitagl), then configure again.")
    endif()
    message(WARNING "GPU GL: using the VitaSDK's vitaGL (${VITAGL_LIB_vitaGL}): not the pinned "
            "version and unknown build flags. Run python3 scripts/setup_vitagl.py to build the pinned one.")
    list(APPEND VITAGL_LIBRARIES "${VITAGL_LIB_vitaGL}")
    # Helper libraries of an unknown vitaGL version: linked when installed.
    foreach(lib vitashark mathneon SceShaccCgExt taihen_stub)
        find_library(VITAGL_LIB_${lib} ${lib} PATHS "${VITASDK}/arm-vita-eabi/lib" NO_DEFAULT_PATH)
        if(VITAGL_LIB_${lib})
            list(APPEND VITAGL_LIBRARIES "${VITAGL_LIB_${lib}}")
        else()
            message(STATUS "GPU GL: optional lib${lib}.a not found (skipped)")
        endif()
    endforeach()
endif()
# Link order matters for static archives: vitaGL first, its dependencies after
# (vitashark -> SceShaccCgExt -> taihen / SceShaccCg, as in vitaGL's samples).
set(VITAGL_LINK_LIBS ${VITAGL_LIBRARIES}
    SceShaccCg_stub SceKernelDmacMgr_stub SceKernelModulemgr_stub
    SceGxm_stub SceDisplay_stub SceSysmodule_stub SceCommonDialog_stub
    SceCtrl_stub SceAppMgr_stub SceIofilemgr_stub SceProcessmgr_stub ScePower_stub
    $<$<BOOL:${DAYTONA_VITA_LINK}>:SceNet_stub> $<$<BOOL:${DAYTONA_VITA_LINK}>:SceNetCtl_stub> SceTouch_stub
    SceLibKernel_stub pthread m)
# The ImGui menu (render/imgui_backend.h, through vitaGL).
set(IMGUI_DIR "${DAYTONA_ROOT}/extern/imgui")
add_library(vita_imgui STATIC "${IMGUI_DIR}/imgui.cpp" "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp" "${IMGUI_DIR}/imgui_widgets.cpp")
target_include_directories(vita_imgui PUBLIC "${IMGUI_DIR}")
target_compile_definitions(vita_imgui PRIVATE IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
target_link_libraries(vita_runtime PUBLIC vita_imgui)
set(VITA_IMGUI_LICENSE FILE "${DAYTONA_ROOT}/extern/imgui/LICENSE.txt" "licenses/imgui.txt")

# The build's date in the menu's title line (frontend/launcher.cpp): build_date.h,
# rewritten on every build, changed only when the date does.
add_custom_target(vita_build_date
    COMMAND "${CMAKE_COMMAND}" -DOUT=${CMAKE_BINARY_DIR}/build_date.h -P "${CMAKE_CURRENT_SOURCE_DIR}/build_date.cmake"
    BYPRODUCTS "${CMAKE_BINARY_DIR}/build_date.h" VERBATIM)

if(DAYTONA_VITA_COMPILE_CHECK)
    add_library(daytona_vita_frontend OBJECT ${VITA_FRONTEND_SOURCES})
    target_include_directories(daytona_vita_frontend PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${VITAGL_INCLUDE_DIR}" "${CMAKE_BINARY_DIR}")
    add_dependencies(daytona_vita_frontend vita_build_date)
    target_link_libraries(daytona_vita_frontend PRIVATE vita_runtime ${VITA_SDL_TARGET})
    message(STATUS "ROM-free COMPILE CHECK only: no linked game or installable VPK will be produced")
else()
    file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${DAYTONA_GEN_ROOT}/${DAYTONA_VITA_ROMSET}/*.cpp")
    list(APPEND GEN_SOURCES "${DAYTONA_GEN_ROOT}/${DAYTONA_VITA_ROMSET}_tgp/tgp_gen.cpp" "${DAYTONA_GEN_ROOT}/${DAYTONA_VITA_ROMSET}_snd/snd_gen.cpp")
    # The recompiled SCSP DSP program (m2dsprecomp), when the set has one.
    if(EXISTS "${DAYTONA_GEN_ROOT}/${DAYTONA_VITA_ROMSET}_dsp/dsp_gen.cpp")
        list(APPEND GEN_SOURCES "${DAYTONA_GEN_ROOT}/${DAYTONA_VITA_ROMSET}_dsp/dsp_gen.cpp")
    endif()
    add_executable(daytona_vita ${VITA_FRONTEND_SOURCES} ${GEN_SOURCES})
    target_include_directories(daytona_vita PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${VITAGL_INCLUDE_DIR}" "${CMAKE_BINARY_DIR}")
    add_dependencies(daytona_vita vita_build_date)
    target_link_libraries(daytona_vita PRIVATE vita_runtime ${VITA_SDL_TARGET} ${VITAGL_LINK_LIBS})
    target_link_options(daytona_vita PRIVATE -Wl,--gc-sections)
    # vita-elf-create appends its SCE data (~4.3 KB) after the code segment, inside the
    # gap before the data segment (64 KB aligned): when the code ends less than that
    # before it, it fails ("segment 1 overlaps"). build_vita.py then relinks with this
    # read-only padding, sized to move the data segment one alignment step further.
    set(DAYTONA_VITA_TEXT_PAD 0 CACHE STRING "Bytes of read-only padding in the code segment (build_vita.py sets it)")
    if(DAYTONA_VITA_TEXT_PAD GREATER 0)
        file(WRITE "${CMAKE_BINARY_DIR}/text_pad.cpp"
             "extern const unsigned char daytona_vita_text_pad[${DAYTONA_VITA_TEXT_PAD}];\n"
             "const unsigned char daytona_vita_text_pad[${DAYTONA_VITA_TEXT_PAD}] = {1};\n")
        target_sources(daytona_vita PRIVATE "${CMAKE_BINARY_DIR}/text_pad.cpp")
        target_link_options(daytona_vita PRIVATE -Wl,--undefined=daytona_vita_text_pad)
    endif()
    set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wno-unused-label")
    vita_create_self(eboot.bin daytona_vita UNSAFE)
    set(VITA_OTHER_GAME)
    if(DAYTONA_VITA_REV_A_SELF)
        if(NOT EXISTS "${DAYTONA_VITA_REV_A_SELF}")
            message(FATAL_ERROR "No companion SELF at ${DAYTONA_VITA_REV_A_SELF}")
        endif()
        set(VITA_OTHER_GAME FILE "${DAYTONA_VITA_REV_A_SELF}" "${DAYTONA_VITA_COMPANION}.self")
    endif()
    # LiveArea images from the game's folder in platform/vita/logo_livearea, when present.
    set(VITA_LIVEAREA_DIR "${DAYTONA_VITA_LIVEAREA_DIR}")
    include("${CMAKE_CURRENT_SOURCE_DIR}/livearea.cmake")
    message(STATUS "Vita LiveArea images (${VITA_LIVEAREA_DIR}): ${VITA_LIVEAREA_SUMMARY}")
    vita_create_vpk(${DAYTONA_VITA_VPK} ${VITA_TITLEID} eboot.bin
        VERSION "01.24" NAME "${VITA_GAME_NAME}" # the game's home-screen name, the same for every build option
        ${VITA_LIVEAREA_FILES}
        FILE "${DAYTONA_ROOT}/THIRD_PARTY.md" "licenses/THIRD_PARTY.md"
        FILE "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY.md" "licenses/VITA.md"
        ${VITA_IMGUI_LICENSE} ${VITA_OTHER_GAME})
endif()
