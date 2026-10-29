cmake_minimum_required(VERSION 3.22)

project(LABX11 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static MSVC runtime: the plugin needs no VC++ redistributable on other machines.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(LABX11_JUCE_PATH "F:/_DEV/tools/JUCE" CACHE PATH "Path to a JUCE 9 checkout")
if(NOT EXISTS "${LABX11_JUCE_PATH}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE not found at ${LABX11_JUCE_PATH}. Configure with -DLABX11_JUCE_PATH=<path to JUCE 9>.")
endif()
add_subdirectory("${LABX11_JUCE_PATH}" JUCE)

# Header-only libraries, as in LAB X-16: no ABI, no link order, no JUCE flag mismatch.
add_library(lab_core INTERFACE)
target_include_directories(lab_core INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
add_library(lab_ui INTERFACE)
target_include_directories(lab_ui INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

set(LABX11_DEFINITIONS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
set(LABX11_PRODUCT "LAB X-11")
set(LABX11_SLUG "x11")
set(LABX11_CODE "Lb11")

# The config header the base and the plugin read (LAB X-16's per-effect LabX16Config.h, renamed). Force-included,
# so plugins/x11/LabX11.cpp reads LABX11_PRODUCT and friends without an include of its own.
set(LABX11_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/config")
file(WRITE "${LABX11_CONFIG_DIR}/LabX11Config.h.in"
    "#pragma once\n#define LABX11_PRODUCT \"@LABX11_PRODUCT@\"\n#define LABX11_SLUG \"@LABX11_SLUG@\"\n#define LABX11_VERSION \"@PROJECT_VERSION@\"\n#define LABX11_CODE \"@LABX11_CODE@\"\n")
configure_file("${LABX11_CONFIG_DIR}/LabX11Config.h.in" "${LABX11_CONFIG_DIR}/LabX11Config.h" @ONLY)
set(LABX11_FORCE_INCLUDE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /FILabX11Config.h>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-include LabX11Config.h>)

juce_add_plugin(LabX11
    PRODUCT_NAME "LAB X-11"
    VERSION ${PROJECT_VERSION}
    COMPANY_NAME "OPERATION FAIRWAY, LLC"
    COMPANY_WEBSITE "https://github.com/crystalball-dev/lab-x-11"
    BUNDLE_ID com.operationfairway.labx11
    PLUGIN_MANUFACTURER_CODE Opfw
    PLUGIN_CODE Lb11
    FORMATS VST3 Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Dynamics)

set(LABX11_SOURCES
    "${CMAKE_SOURCE_DIR}/plugins/x11/LabX11.cpp"
    "${CMAKE_SOURCE_DIR}/core/host/ZoneBusPlatform.cpp")

target_sources(LabX11 PRIVATE ${LABX11_SOURCES})
target_include_directories(LabX11 PRIVATE "${LABX11_CONFIG_DIR}")
target_compile_definitions(LabX11 PUBLIC ${LABX11_DEFINITIONS})
target_compile_options(LabX11 PRIVATE ${LABX11_FORCE_INCLUDE})
target_link_libraries(LabX11
    PRIVATE
        lab_core
        lab_ui
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# The offline render / test harness (test/run_tests.py drives it).
# JUCE_MODAL_LOOPS_PERMITTED: --png-wait runs the message loop before the snapshot.
juce_add_console_app(LabX11Render PRODUCT_NAME "LabX11Render")
target_sources(LabX11Render PRIVATE "${CMAKE_SOURCE_DIR}/test/harness_main.cpp" ${LABX11_SOURCES})
target_include_directories(LabX11Render PRIVATE "${LABX11_CONFIG_DIR}")
target_compile_definitions(LabX11Render PRIVATE ${LABX11_DEFINITIONS} LABX11_HARNESS=1 JUCE_MODAL_LOOPS_PERMITTED=1)
target_compile_options(LabX11Render PRIVATE ${LABX11_FORCE_INCLUDE})
target_link_libraries(LabX11Render
    PRIVATE
        lab_core
        lab_ui
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
