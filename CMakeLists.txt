cmake_minimum_required(VERSION 3.25)

project(TFDEngine_Rebuild VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23 CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_REQUIRED ON CACHE BOOL "" FORCE)
set(CMAKE_CXX_EXTENSIONS OFF CACHE BOOL "" FORCE)

# Guard: CommonLibSSE-NG requires C++20+
if (CMAKE_CXX_STANDARD LESS 20)
  message(FATAL_ERROR "TFDEngine requires C++20 or later. Remove any forced /std:c++17 from your build configuration.")
endif()

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(ENABLE_SKYRIM_SE "Build for Skyrim SE 1.5.x" ON)
option(ENABLE_SKYRIM_AE "Build for Skyrim AE 1.6.x" OFF)
option(ENABLE_SKYRIM_VR "Build for Skyrim VR" OFF)
option(TFDEnableSmf "Build SKSE Menu Framework integration" ON)

# ---- Dependencies (use installed packages / vcpkg) ----
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

# ---- CommonLibSSE-NG ----
add_subdirectory(extern/CommonLibSSE-NG)

# ---- TFDEngine DLL ----
add_library(TFDEngine SHARED
    src/main.cpp
    src/TFDSettings.cpp
    src/TFDLocation.cpp
    src/TFDActorScan.cpp
    src/TFDAntiAggro.cpp
    src/TFDDefeatMonitor.cpp
    src/TFDFactionMask.cpp
    src/TFDForceGreet.cpp
    src/TFDAggressionClamp.cpp
    src/TFDPreCombat.cpp
    src/TFDPreCombatGreet.cpp
    src/TFDPacify.cpp
    src/TFDPacifyHooks.cpp
    src/TFDTargetClassifier.cpp
    src/TFDInteractionRouter.cpp
    src/MenuFramework.cpp
    src/TFDCaptiveHostility.cpp
    src/TFDCaptiveDoorController.cpp
    src/TFDTeammateAliasSync.cpp
)

if(TFDEnableSmf)
  target_compile_definitions(TFDEngine PRIVATE TFDEnableSmf=1)
else()
  target_compile_definitions(TFDEngine PRIVATE TFDEnableSmf=0)
endif()

target_include_directories(TFDEngine PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(TFDEngine PRIVATE
  CommonLibSSE::CommonLibSSE
  fmt::fmt
  spdlog::spdlog
)

if(MSVC)
  target_compile_options(TFDEngine PRIVATE /W4 /permissive- /utf-8)
endif()

set_target_properties(TFDEngine PROPERTIES
  OUTPUT_NAME "TFDEngine"
)

option(TFDCopyToSksePlugins "Copy TFDEngine.dll to a SKSE/Plugins folder after build" OFF)
set(TFDSksePluginsDir "" CACHE PATH "Path to your Data/SKSE/Plugins (or MO2 mod folder)/SKSE/Plugins")

if(TFDCopyToSksePlugins AND TFDSksePluginsDir)
  add_custom_command(TARGET TFDEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${TFDSksePluginsDir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:TFDEngine>" "${TFDSksePluginsDir}"
  )
endif()