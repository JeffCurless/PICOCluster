cmake_minimum_required(VERSION 3.18)   # 3.18: set_source_files_properties(TARGET_DIRECTORY)

# ===========================================================================
# picoCluster — cluster environment built on picoOS (git submodule).
#
# Supported target (for now): Raspberry Pi Pico W with a Pimoroni Display Pack.
#
#   ./build            configure + build every variant into builds/
#   ./clean            empty builds/ and remove the images in kits/
#
# Or by hand:
#   cmake -B builds/picow_D  -DPICO_BOARD=picow
#   cmake -B builds/picow_D2 -DPICO_BOARD=picow -DPICOOS_DISPLAY_PACK2=ON
#   make -j$(nproc) -C builds/picow_D
#
# The .uf2 is built in builds/<variant>/picoOS/src/ and copied to kits/.
#
# We call add_subdirectory(picoOS/src) directly rather than
# add_subdirectory(picoOS): picoOS's own root runs project() and
# pico_sdk_init(), which must only run once.  See
# picoOS/docs/project-submodule.md.
# ===========================================================================

# ---------------------------------------------------------------------------
# Board selection — must run before pico_sdk_import.cmake reads PICO_BOARD.
#
# Only pico_w is supported today.  To add Pico 2 W later, add "pico2_w"
# (and its "pico2w" alias) to PICOCLUSTER_SUPPORTED_BOARDS.
# ---------------------------------------------------------------------------
set(PICOCLUSTER_SUPPORTED_BOARDS pico_w)

if(NOT DEFINED PICO_BOARD OR PICO_BOARD STREQUAL "")
    set(PICO_BOARD "pico_w" CACHE STRING "Target board" FORCE)
endif()

# Friendly aliases (mirrors picoOS/CMakeLists.txt).
if(PICO_BOARD STREQUAL "picow")
    set(PICO_BOARD "pico_w" CACHE STRING "Target board" FORCE)
elseif(PICO_BOARD STREQUAL "pico2w")
    set(PICO_BOARD "pico2_w" CACHE STRING "Target board" FORCE)
endif()

if(NOT PICO_BOARD IN_LIST PICOCLUSTER_SUPPORTED_BOARDS)
    message(FATAL_ERROR
        "picoCluster: unsupported board '${PICO_BOARD}'. "
        "Supported: ${PICOCLUSTER_SUPPORTED_BOARDS} (use -DPICO_BOARD=picow).")
endif()

# compile_commands.json for clangd (.clangd points at builds/picow_D).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Use the SDK import helper shipped with picoOS rather than a copy.
include(${CMAKE_CURRENT_SOURCE_DIR}/picoOS/pico_sdk_import.cmake)

project(picocluster C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# ---------------------------------------------------------------------------
# picoCluster version (independent of the picoOS version)
# ---------------------------------------------------------------------------
set(CLUSTER_VER_MAJOR 0)
set(CLUSTER_VER_MINOR 0)
set(CLUSTER_VER_EDIT  1)
set(PICOCLUSTER_VERSION "${CLUSTER_VER_MAJOR}.${CLUSTER_VER_MINOR}.${CLUSTER_VER_EDIT}")
message(STATUS "picoCluster version   : ${PICOCLUSTER_VERSION}")

# Largest cluster task argument / result in bytes.  Part of the build id, so
# every node must use the same value.  More head room on Pico 2 W later.
set(CLUSTER_MAX_PAYLOAD 512)

# ---------------------------------------------------------------------------
# picoOS feature configuration
#
# Normal (non-cache) variables set here take precedence over picoOS's
# option() declarations (policy CMP0077) and over any -D cache value,
# so the display cannot be turned off by accident.
#
# The Display Pack model is still selectable:
#   -DPICOOS_DISPLAY_PACK2=ON   Display Pack 2 (320x240)
#   (default)                   Display Pack   (240x135)
# ---------------------------------------------------------------------------
set(PICOOS_DISPLAY_ENABLE ON)

# picoOS demo apps (producer/consumer/sensor/pi/scantest/cray-one) are
# excluded; picoCluster supplies its own app table.
set(PICOOS_INCLUDE_DEMO_APPS OFF)

# Absolute paths are required — picoOS/src resolves relative paths against
# its own directory.
set(PICOCLUSTER_LAYER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/cluster/proto.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cluster/net.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cluster/node.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cluster/exec.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cluster/rpc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cluster/clusterd.c
)
set(PICOOS_APP_SOURCES
    ${PICOCLUSTER_LAYER_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/app_table.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/cluster_tasks.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/cpi.c
)
# The project root makes "cluster/..." includes resolve; apps/ for app headers.
set(PICOOS_APP_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/apps
)

add_subdirectory(picoOS/src)

# Hold project sources to the same warning set (and -Werror) as picoOS.
# PICOOS_WARNINGS is exported to this scope by picoOS/src/CMakeLists.txt.
# The picoCluster version is visible to project sources as
# CLUSTER_VER_MAJOR / CLUSTER_VER_MINOR / CLUSTER_VER_EDIT (same names as above).
#
# Source-file properties are directory-scoped: TARGET_DIRECTORY picoos is
# required so they apply where the picoos target is defined (picoOS/src).
set_source_files_properties(${PICOOS_APP_SOURCES}
    TARGET_DIRECTORY picoos
    PROPERTIES
    COMPILE_OPTIONS "${PICOOS_WARNINGS}"
    COMPILE_DEFINITIONS
        "CLUSTER_VER_MAJOR=${CLUSTER_VER_MAJOR};CLUSTER_VER_MINOR=${CLUSTER_VER_MINOR};CLUSTER_VER_EDIT=${CLUSTER_VER_EDIT};CLUSTER_MAX_PAYLOAD=${CLUSTER_MAX_PAYLOAD}u")

# ---------------------------------------------------------------------------
# Output naming — replaces picoOS's own name (e.g. picowos_D-v0.4.1).
#
#   Display Pack   → picocluster_D-v<ver>
#   Display Pack 2 → picocluster_D2-v<ver>
#
# Note: when Pico 2 W support is added the name will need a board tag so the
# two boards' images can coexist in kits/.
# ---------------------------------------------------------------------------
if(PICOOS_DISPLAY_PACK2)
    set(_disp_suffix "_D2")
else()
    set(_disp_suffix "_D")
endif()
set(PICOCLUSTER_OUTPUT_NAME "picocluster${_disp_suffix}-v${PICOCLUSTER_VERSION}")
set_target_properties(picoos PROPERTIES OUTPUT_NAME "${PICOCLUSTER_OUTPUT_NAME}")
message(STATUS "picoCluster output    : ${PICOCLUSTER_OUTPUT_NAME}")

# ---------------------------------------------------------------------------
# kits/ — every build copies its .uf2 into <project root>/kits/.
# "make clean" in a build directory also removes that build's kit image.
# ---------------------------------------------------------------------------
set(PICOCLUSTER_KITS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/kits)
add_custom_target(kits ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PICOCLUSTER_KITS_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE_DIR:picoos>/${PICOCLUSTER_OUTPUT_NAME}.uf2
            ${PICOCLUSTER_KITS_DIR}/
    COMMENT "Copying ${PICOCLUSTER_OUTPUT_NAME}.uf2 to kits/"
    VERBATIM)
add_dependencies(kits picoos)
set_property(TARGET kits PROPERTY ADDITIONAL_CLEAN_FILES
    ${PICOCLUSTER_KITS_DIR}/${PICOCLUSTER_OUTPUT_NAME}.uf2)
