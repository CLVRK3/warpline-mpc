# Warpline desktop build (beta): an Audio Unit and VST3 on macOS, a VST3 on Linux and Windows.
#
#   cmake -S desktop -B build/desktop -DCMAKE_BUILD_TYPE=Release [-DJUCE_DIR=<a JUCE 8 checkout>]
#   cmake --build build/desktop --config Release
#
# The desktop binaries combine Warpline (GPLv3) with JUCE (AGPLv3), which GPLv3 section 13 allows; they are
# distributed under the AGPLv3 (the source is this repository). The MPC plugin does not use JUCE and stays GPLv3.
#
# Two targets: warpline_engine is the unchanged MPC plugin (src/, vital/, shim/) as a library with the VST2 entry point,
# and Warpline (JUCE 8) is the AU/VST3 that loads it from its bundle's Contents/Resources/VST. Content/ and plugin.json go
# into Contents/Resources too, where the engine finds them (desktop/sdk/mpc_resources.h). See desktop/README.md.
# Part of Warpline. GPLv3 (see LICENSE).

cmake_minimum_required(VERSION 3.26)
project(WarplineDesktop VERSION 1.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
endif()

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# ----- JUCE 8 (AGPLv3; JUCE 7, which was GPLv3, no longer builds with current Xcode) -----
set(JUCE_DIR "" CACHE PATH "A JUCE 8 checkout (empty: download JUCE 8.0.15)")
if(JUCE_DIR)
  add_subdirectory(${JUCE_DIR} juce)
else()
  include(FetchContent)
  FetchContent_Declare(juce GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.15 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(juce)
endif()

# ----- the engine: the MPC plugin as a library -----
file(GLOB ENGINE_SOURCES ${ROOT}/src/*.cpp)
add_library(warpline_engine MODULE ${ENGINE_SOURCES})
set_target_properties(warpline_engine PROPERTIES OUTPUT_NAME "warpline" PREFIX "" CXX_VISIBILITY_PRESET hidden
                      VISIBILITY_INLINES_HIDDEN ON)
if(APPLE)
  set_target_properties(warpline_engine PROPERTIES SUFFIX ".dylib")
endif()
target_include_directories(warpline_engine PRIVATE
  ${ROOT}/src ${CMAKE_CURRENT_SOURCE_DIR}/sdk
  ${ROOT}/vital/synthesis ${ROOT}/vital/synthesis/effects ${ROOT}/vital/synthesis/filters
  ${ROOT}/vital/synthesis/framework ${ROOT}/vital/synthesis/lookups ${ROOT}/vital/synthesis/modulators
  ${ROOT}/vital/synthesis/modules ${ROOT}/vital/synthesis/producers ${ROOT}/vital/synthesis/synth_engine
  ${ROOT}/vital/synthesis/utilities ${ROOT}/vital/common ${ROOT}/vital/common/wavetable ${ROOT}/vital/third_party
  ${ROOT}/shim ${ROOT}/third_party/mutable)
if(MSVC)
  target_compile_options(warpline_engine PRIVATE /O2 /bigobj)
else()
  target_compile_options(warpline_engine PRIVATE -O2 -w)
endif()
if(APPLE)
  target_link_libraries(warpline_engine PRIVATE "-framework Accelerate")   # Vital's FFT on macOS
elseif(UNIX)
  target_link_libraries(warpline_engine PRIVATE pthread dl)
endif()

# ----- the plugin -----
set(FORMATS VST3)
if(APPLE)
  set(FORMATS AU VST3)
endif()
juce_add_plugin(Warpline
  COMPANY_NAME "CLVRKE"
  BUNDLE_ID "com.clvrke.warpline"
  PLUGIN_MANUFACTURER_CODE Clvr
  PLUGIN_CODE Wrpl
  IS_SYNTH TRUE
  NEEDS_MIDI_INPUT TRUE
  NEEDS_MIDI_OUTPUT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS FALSE
  COPY_PLUGIN_AFTER_BUILD FALSE
  AU_MAIN_TYPE kAudioUnitType_MusicDevice
  VST3_CATEGORIES Instrument Synth
  DESCRIPTION "Wavetable synth (Vital engine), the MPC plugin on the desktop"
  FORMATS ${FORMATS}
  PRODUCT_NAME "Warpline")
target_sources(Warpline PRIVATE WarplineDesktop.cpp)
target_include_directories(Warpline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
juce_generate_juce_header(Warpline)
target_compile_definitions(Warpline PUBLIC
  JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
target_link_libraries(Warpline PRIVATE juce::juce_audio_utils
  PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)
add_dependencies(Warpline warpline_engine)

# Put the engine, Content/ and plugin.json into each bundle's Contents/Resources (every build, so a changed engine
# also lands in the bundle when the plugin itself did not need relinking).
foreach(format ${FORMATS})
  set(target Warpline_${format})
  get_target_property(artefact ${target} JUCE_PLUGIN_ARTEFACT_FILE)
  add_custom_target(${target}_resources ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${artefact}/Contents/Resources/VST"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:warpline_engine>" "${artefact}/Contents/Resources/VST/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ROOT}/plugin.json" "${artefact}/Contents/Resources/plugin.json"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${ROOT}/Content" "${artefact}/Contents/Resources/Content"
    VERBATIM)
  add_dependencies(${target}_resources ${target} warpline_engine)
endforeach()
