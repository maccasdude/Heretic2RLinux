#
# Heretic2R - Linux port build system
#
# This builds Heretic2R on Linux as one executable plus six shared
# libraries that mirror the Windows DLL layout:
#
#   heretic2r              <- the launcher binary (runs the engine)
#   libquake2.so           <- the engine (corresponds to quake2.dll/QUAKE2_DLL)
#   libH2Common.so         <- shared math, console, resource-manager helpers
#   libplayer.so           <- player animation/control plugin
#   libgame.so             <- server-side game logic (gamex86.dll on Windows)
#   libclient_effects.so   <- client-side particles/effects plugin
#   libref_gl1.so          <- OpenGL 1.3 renderer (ref_gl1.dll on Windows)
#   libsnd_sdl3.so         <- SDL3 sound backend (snd_sdl3.dll on Windows)
#
# After building, install the .so files alongside the launcher binary in
# the install/run dir together with the H2 game data in ./base/.
#

cmake_minimum_required(VERSION 3.16)
project(Heretic2R LANGUAGES C CXX)

set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Output libraries to the binary directory so the launcher can dlopen them
# from the same folder via the rpath we set below.
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Use rpath = $ORIGIN so the executable finds its sibling .so files.
set(CMAKE_INSTALL_RPATH "$ORIGIN")
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)

# Hide symbols by default; the H2COMMON_API / Q2DLL_DECLSPEC macros mark the
# ones we want exported.
add_compile_options(
    -Wall
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-but-set-variable
    -Wno-sign-compare
    -Wno-missing-braces
    -Wno-pointer-sign
    -Wno-unknown-pragmas        # the source has lots of #pragma region from MSVC
    -fvisibility=hidden
    -fno-strict-aliasing        # Quake-style code aliases through structs
    -O2                          # release-grade optimization; -O0 default causes physics jitter
)

# Dependencies ----------------------------------------------------------------

# SDL3 is required for window/input/audio.
find_package(SDL3 QUIET)
if(NOT SDL3_FOUND)
    # Allow building against a system pkg-config SDL3.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
    set(SDL3_TARGET PkgConfig::SDL3)
else()
    set(SDL3_TARGET SDL3::SDL3)
endif()

find_package(OpenGL REQUIRED)
find_library(MATH_LIB m)
find_library(DL_LIB  dl)

# Common include paths --------------------------------------------------------

set(H2R_INC_ROOT ${CMAKE_SOURCE_DIR})

include_directories(
    ${H2R_INC_ROOT}/include
    ${H2R_INC_ROOT}/src/qcommon
    ${H2R_INC_ROOT}/src/game/src
    ${H2R_INC_ROOT}/src/Player/src
    ${H2R_INC_ROOT}/src/quake2/src
)

# ----------------------------------------------------------------------------
# Compat helper - linked into everything that touches MSVC-isms.
# Header-only except for _findfirst implementation.
# ----------------------------------------------------------------------------

add_library(h2r_compat OBJECT
    ${H2R_INC_ROOT}/include/compat/win_compat.c
)

# ----------------------------------------------------------------------------
# H2Common.so
# ----------------------------------------------------------------------------

file(GLOB H2COMMON_SRC ${H2R_INC_ROOT}/src/H2Common/src/*.c)
list(APPEND H2COMMON_SRC ${H2R_INC_ROOT}/src/game/src/q_Shared.c)

add_library(H2Common SHARED
    ${H2COMMON_SRC}
    $<TARGET_OBJECTS:h2r_compat>
)
target_compile_definitions(H2Common PRIVATE H2COMMON)
# H2R doesn't religiously mark every H2Common export with H2COMMON_API, so we
# need default visibility here for cross-.so symbols to resolve (most notably
# BoxOnPlaneSide, Q_strncpyz, etc).
target_compile_options(H2Common PRIVATE -fvisibility=default)
target_include_directories(H2Common PRIVATE
    ${H2R_INC_ROOT}/src/H2Common/src
    ${H2R_INC_ROOT}/src/Player/src
)
target_link_libraries(H2Common PRIVATE ${MATH_LIB} ${DL_LIB})

# ----------------------------------------------------------------------------
# libplayer.so
# ----------------------------------------------------------------------------

set(PLAYER_SRC
    ${H2R_INC_ROOT}/src/Player/src/p_actions.c
    ${H2R_INC_ROOT}/src/Player/src/p_animactor.c
    ${H2R_INC_ROOT}/src/Player/src/p_anims.c
    ${H2R_INC_ROOT}/src/Player/src/p_anim_branch.c
    ${H2R_INC_ROOT}/src/Player/src/p_anim_data.c
    ${H2R_INC_ROOT}/src/Player/src/p_chicken.c
    ${H2R_INC_ROOT}/src/Player/src/p_chicken_anim.c
    ${H2R_INC_ROOT}/src/Player/src/p_items.c
    ${H2R_INC_ROOT}/src/Player/src/p_main.c
    ${H2R_INC_ROOT}/src/Player/src/p_utility.c
    ${H2R_INC_ROOT}/src/Player/src/p_weapon.c
)

add_library(player SHARED
    ${PLAYER_SRC}
    $<TARGET_OBJECTS:h2r_compat>
)
target_compile_definitions(player PRIVATE PLAYER_DLL)
target_include_directories(player PRIVATE
    ${H2R_INC_ROOT}/src/Player/src
    ${H2R_INC_ROOT}/src/quake2/src/client
)
target_link_libraries(player PRIVATE H2Common ${MATH_LIB} ${DL_LIB})

# ----------------------------------------------------------------------------
# libgame.so  (gamex86 on Windows)
# ----------------------------------------------------------------------------

file(GLOB_RECURSE GAME_SRC_C   ${H2R_INC_ROOT}/src/game/src/*.c)
file(GLOB_RECURSE GAME_SRC_CXX ${H2R_INC_ROOT}/src/game/src/*.cpp)
# q_Shared.c is already pulled in via H2Common; remove from the list to
# avoid duplicate-symbol errors at link time.
list(REMOVE_ITEM GAME_SRC_C ${H2R_INC_ROOT}/src/game/src/q_Shared.c)
list(APPEND      GAME_SRC_C
    ${H2R_INC_ROOT}/src/qcommon/Message.c
    ${H2R_INC_ROOT}/src/qcommon/p_dll.c
    ${H2R_INC_ROOT}/src/qcommon/Skeletons.c
)

add_library(game SHARED
    ${GAME_SRC_C}
    ${GAME_SRC_CXX}
    $<TARGET_OBJECTS:h2r_compat>
)
target_compile_definitions(game PRIVATE GAME_DLL)
target_include_directories(game PRIVATE
    ${H2R_INC_ROOT}/src/game/src
    ${H2R_INC_ROOT}/src/Player/src
    ${H2R_INC_ROOT}/src/quake2/src
)
target_link_libraries(game PRIVATE H2Common ${MATH_LIB} ${DL_LIB})

# ----------------------------------------------------------------------------
# libclient_effects.so   ("Client Effects" on Windows)
# ----------------------------------------------------------------------------

file(GLOB_RECURSE CLFX_SRC ${H2R_INC_ROOT}/src/client\ effects/src/*.c)
list(APPEND       CLFX_SRC
    ${H2R_INC_ROOT}/src/qcommon/anorms.c
    ${H2R_INC_ROOT}/src/qcommon/Message.c
    ${H2R_INC_ROOT}/src/qcommon/netmsg_read.c
    ${H2R_INC_ROOT}/src/qcommon/turbsin.c
)

add_library(client_effects SHARED
    ${CLFX_SRC}
    $<TARGET_OBJECTS:h2r_compat>
)
target_include_directories(client_effects PRIVATE
    ${H2R_INC_ROOT}/src/client\ effects/src
    ${H2R_INC_ROOT}/src/quake2/src/client
    ${H2R_INC_ROOT}/src/quake2/src
)
target_link_libraries(client_effects PRIVATE H2Common ${MATH_LIB} ${DL_LIB})

# ----------------------------------------------------------------------------
# libref_gl1.so
# ----------------------------------------------------------------------------

file(GLOB_RECURSE REF_GL1_SRC ${H2R_INC_ROOT}/src/ref_gl1/src/*.c)
list(APPEND       REF_GL1_SRC
    ${H2R_INC_ROOT}/include/glad-GL1.3/glad.c
    ${H2R_INC_ROOT}/src/qcommon/anorms.c
    ${H2R_INC_ROOT}/src/qcommon/Reference.c
    ${H2R_INC_ROOT}/src/qcommon/Skeletons.c
    ${H2R_INC_ROOT}/src/qcommon/turbsin.c
)

add_library(ref_gl1 SHARED
    ${REF_GL1_SRC}
    $<TARGET_OBJECTS:h2r_compat>
)
target_include_directories(ref_gl1 PRIVATE
    ${H2R_INC_ROOT}/src/ref_gl1/src
    ${H2R_INC_ROOT}/src/quake2/src
    ${H2R_INC_ROOT}/include/glad-GL1.3
)
target_link_libraries(ref_gl1 PRIVATE
    H2Common
    ${SDL3_TARGET}
    OpenGL::GL
    ${MATH_LIB} ${DL_LIB}
)

# ----------------------------------------------------------------------------
# libsnd_sdl3.so
# ----------------------------------------------------------------------------

set(SND_SDL3_SRC
    ${H2R_INC_ROOT}/src/snd_sdl3/src/snd_LowpassFilter.c
    ${H2R_INC_ROOT}/src/snd_sdl3/src/snd_main.c
    ${H2R_INC_ROOT}/src/snd_sdl3/src/snd_ogg.c
    ${H2R_INC_ROOT}/src/snd_sdl3/src/snd_sdl3.c
    ${H2R_INC_ROOT}/src/snd_sdl3/src/snd_wav.c
)

add_library(snd_sdl3 SHARED
    ${SND_SDL3_SRC}
    $<TARGET_OBJECTS:h2r_compat>
)
target_include_directories(snd_sdl3 PRIVATE
    ${H2R_INC_ROOT}/src/snd_sdl3/src
    ${H2R_INC_ROOT}/src/quake2/src
    ${H2R_INC_ROOT}/src/quake2/src/client
)
target_link_libraries(snd_sdl3 PRIVATE H2Common ${SDL3_TARGET} ${MATH_LIB} ${DL_LIB})

# ----------------------------------------------------------------------------
# libquake2.so  (the engine itself)
# ----------------------------------------------------------------------------

file(GLOB_RECURSE QUAKE2_SRC
    ${H2R_INC_ROOT}/src/quake2/src/client/*.c
    ${H2R_INC_ROOT}/src/quake2/src/cs_shared/*.c
    ${H2R_INC_ROOT}/src/quake2/src/server/*.c
)

# Cross-platform DLL-glue files live in win32/dll_io/ but compile fine on
# Linux because we patched their headers.
list(APPEND QUAKE2_SRC
    ${H2R_INC_ROOT}/src/quake2/src/win32/dll_io/clfx_dll.c
    ${H2R_INC_ROOT}/src/quake2/src/win32/dll_io/snd_dll.c
    ${H2R_INC_ROOT}/src/quake2/src/win32/dll_io/vid_dll.c
    ${H2R_INC_ROOT}/src/quake2/src/win32/vid_Screenshot.c
)

# Platform layer: Linux .c, Windows .c, plus the qcommon shared bits.
list(APPEND QUAKE2_SRC
    ${H2R_INC_ROOT}/src/quake2/src/linux/sys_linux.c
    ${H2R_INC_ROOT}/src/quake2/src/linux/q_shlinux.c
    ${H2R_INC_ROOT}/src/quake2/src/linux/net_linux.c
    ${H2R_INC_ROOT}/include/libsmacker/smacker.c
    ${H2R_INC_ROOT}/src/qcommon/anorms.c
    ${H2R_INC_ROOT}/src/qcommon/netmsg_read.c
    ${H2R_INC_ROOT}/src/qcommon/netmsg_write.c
    ${H2R_INC_ROOT}/src/qcommon/p_dll.c
    ${H2R_INC_ROOT}/src/qcommon/Reference.c
    ${H2R_INC_ROOT}/src/qcommon/Skeletons.c
    ${H2R_INC_ROOT}/src/qcommon/turbsin.c
)

add_library(quake2 SHARED
    ${QUAKE2_SRC}
    $<TARGET_OBJECTS:h2r_compat>
)
target_compile_definitions(quake2 PRIVATE
    QUAKE2_DLL
    # MSVC-style build identifiers used in the version string.
    CPUSTRING="${CMAKE_SYSTEM_PROCESSOR}"
    BUILDSTRING="Linux"
)
target_include_directories(quake2 PRIVATE
    ${H2R_INC_ROOT}/src/quake2/src
    ${H2R_INC_ROOT}/src/quake2/src/client
    ${H2R_INC_ROOT}/src/quake2/src/server
    ${H2R_INC_ROOT}/src/H2Common/src
    ${H2R_INC_ROOT}/src/client\ effects/src
    ${H2R_INC_ROOT}/include/glad-GL1.3
)
target_link_libraries(quake2 PRIVATE
    H2Common
    ${SDL3_TARGET}
    OpenGL::GL
    ${MATH_LIB} ${DL_LIB}
    -Wl,--no-as-needed   # keep the H2Common runtime ctor alive
)

# ----------------------------------------------------------------------------
# Launcher executable
# ----------------------------------------------------------------------------

add_executable(heretic2r
    ${H2R_INC_ROOT}/src/launcher/src/main_linux.c
)
target_link_libraries(heretic2r PRIVATE quake2 H2Common)

# ----------------------------------------------------------------------------
# Helpful notes during build
# ----------------------------------------------------------------------------

message(STATUS "Heretic2R Linux configuration:")
message(STATUS "  SDL3 target:   ${SDL3_TARGET}")
message(STATUS "  Build type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "  Output:        ${CMAKE_BINARY_DIR}")
