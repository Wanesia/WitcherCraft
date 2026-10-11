cmake_minimum_required(VERSION 3.21)
project(WitcherCraftNative LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "WitcherCraft native must be built for x64 (use -A x64).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: no dependency on a particular VC++ redist being installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(WCRAFT_CORE_SOURCES
    src/log.cpp
    src/pe.cpp
    src/pattern.cpp
    src/hook.cpp
    src/red_engine.cpp
)

# The release version, from the Minecraft mod (minecraft/gradle.properties: version=...).
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/../minecraft/gradle.properties WCRAFT_VERSION_LINE REGEX "^version=")
string(REGEX REPLACE "^version=[ ]*([^ ]+).*" "\\1" WCRAFT_VERSION "${WCRAFT_VERSION_LINE}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../minecraft/gradle.properties)
set(WCRAFT_COMMON_DEFS WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS WCRAFT_VERSION="${WCRAFT_VERSION}")
set(WCRAFT_COMMON_OPTS /W4 /permissive- /utf-8 /EHsc /Zi)

# ---------------------------------------------------------------------------
# dinput8.dll proxy (the plugin itself). Same binary also works as an .asi.
# ---------------------------------------------------------------------------
set(WCRAFT_PROTOCOL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../protocol)

add_library(witchercraft_native SHARED
    ${WCRAFT_CORE_SOURCES}
    src/config.cpp
    src/skycraft_host.cpp
    src/blocks.cpp
    src/buildmode.cpp
    src/control.cpp
    src/camsync.cpp
    src/combat.cpp
    src/natives_blocks.cpp
    src/natives_combat.cpp
    src/natives_visual.cpp
    src/natives_control.cpp
    src/natives_collision.cpp
    src/collision.cpp
    src/collision_physx.cpp
    src/log_channel.cpp
    src/natives.cpp
    src/launcher.cpp
    src/overlay_dx12.cpp
    src/proxy_dinput8.cpp
    src/dllmain.cpp
    src/dinput8.def
)
target_include_directories(witchercraft_native PRIVATE src ${WCRAFT_PROTOCOL_DIR})
target_compile_definitions(witchercraft_native PRIVATE ${WCRAFT_COMMON_DEFS})
target_compile_options(witchercraft_native PRIVATE ${WCRAFT_COMMON_OPTS})
# /PDBALTPATH: the DLL names its symbols file "dinput8.pdb" only, not the build machine's full path.
target_link_options(witchercraft_native PRIVATE /DEBUG /OPT:REF /OPT:ICF /PDBALTPATH:dinput8.pdb)
set_target_properties(witchercraft_native PROPERTIES OUTPUT_NAME "dinput8")
add_custom_command(TARGET witchercraft_native POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:witchercraft_native>
            $<TARGET_FILE_DIR:witchercraft_native>/witchercraft.asi
    COMMENT "Also emitting witchercraft.asi (for setups that already use an ASI loader)")

# ---------------------------------------------------------------------------
# Offline verifier: maps witcher3.exe as an image (no execution) and runs the
# exact same signature resolver the plugin uses at runtime.
# ---------------------------------------------------------------------------
add_executable(wcraft_scan
    ${WCRAFT_CORE_SOURCES}
    tools/wcraft_scan.cpp
)
target_include_directories(wcraft_scan PRIVATE src)
target_compile_definitions(wcraft_scan PRIVATE ${WCRAFT_COMMON_DEFS})
target_compile_options(wcraft_scan PRIVATE ${WCRAFT_COMMON_OPTS})

# ---------------------------------------------------------------------------
# Offline link test: a stand-in for the game that hosts Local\SkyCraft_v1 with the
# very same host code as the DLL (no game, no hooks). See tools/fake_host.cpp.
# ---------------------------------------------------------------------------
add_executable(fake_host
    src/log.cpp
    src/skycraft_host.cpp
    src/collision.cpp
    src/blocks.cpp
    src/buildmode.cpp
    src/control.cpp
    src/camsync.cpp
    src/combat.cpp
    tools/fake_host.cpp
    tools/fake_host_walls.cpp
    tools/fake_host_features.cpp
    tools/fake_host_tnt.cpp
    tools/fake_host_mobs.cpp
    tools/fake_host_build.cpp
    src/pe.cpp
    src/pattern.cpp
    src/hook.cpp
    src/red_engine.cpp
    src/collision_physx.cpp
    tools/col_scan.cpp
)
target_include_directories(fake_host PRIVATE src ${WCRAFT_PROTOCOL_DIR})
target_compile_definitions(fake_host PRIVATE ${WCRAFT_COMMON_DEFS} WCRAFT_IN_FAKE_HOST)
target_compile_options(fake_host PRIVATE ${WCRAFT_COMMON_OPTS})

# ---------------------------------------------------------------------------
# Offline overlay test: its own D3D12 window/swapchain + the host + the very same
# overlay code as the DLL (hooks included). See tools/overlay_test.cpp.
# (The DLL itself loads d3d12/dxgi/d3dcompiler dynamically: no new imports.)
# ---------------------------------------------------------------------------
add_executable(overlay_test
    src/log.cpp
    src/config.cpp
    src/skycraft_host.cpp
    src/collision.cpp
    src/blocks.cpp
    src/buildmode.cpp
    src/control.cpp
    src/camsync.cpp
    src/combat.cpp
    src/overlay_dx12.cpp
    tools/overlay_test.cpp
)
target_include_directories(overlay_test PRIVATE src ${WCRAFT_PROTOCOL_DIR})
target_compile_definitions(overlay_test PRIVATE ${WCRAFT_COMMON_DEFS})
target_compile_options(overlay_test PRIVATE ${WCRAFT_COMMON_OPTS})
target_link_libraries(overlay_test PRIVATE d3d12 dxgi)

# ---------------------------------------------------------------------------
# M5a offline check: maps witcher3.exe + PhysX3Common_x64.dll as images (nothing executes) and
# runs the W3 collision (PhysX) resolver of collision_physx.cpp.
# ---------------------------------------------------------------------------
add_executable(col_scan
    ${WCRAFT_CORE_SOURCES}
    src/collision.cpp
    src/collision_physx.cpp
    tools/col_scan.cpp
)
target_include_directories(col_scan PRIVATE src ${WCRAFT_PROTOCOL_DIR})
target_compile_definitions(col_scan PRIVATE ${WCRAFT_COMMON_DEFS} WCRAFT_OFFLINE_TOOL)
target_compile_options(col_scan PRIVATE ${WCRAFT_COMMON_OPTS})

# ---------------------------------------------------------------------------
# Offline test of the bundled-Minecraft launcher (src/launcher.cpp): unpacks a release's
# WitcherCraft-Minecraft.zip into a scratch %LOCALAPPDATA% and starts Prism, without the game.
# ---------------------------------------------------------------------------
add_executable(launcher_test
    ${WCRAFT_CORE_SOURCES}
    src/config.cpp
    src/launcher.cpp
    tools/launcher_test.cpp
)
target_include_directories(launcher_test PRIVATE src ${WCRAFT_PROTOCOL_DIR})
target_compile_definitions(launcher_test PRIVATE ${WCRAFT_COMMON_DEFS} WCRAFT_OFFLINE_TOOL)
target_compile_options(launcher_test PRIVATE ${WCRAFT_COMMON_OPTS})
