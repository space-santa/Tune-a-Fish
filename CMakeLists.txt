# SPDX-License-Identifier: AGPL-3.0-or-later

cmake_minimum_required(VERSION 3.22)

# Must be set before project(). Xcode 27 no longer supports targets below macOS 12.
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")

project(TuneAFish VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(TUNEAFISH_BUILD_PLUGIN "Build the plugin and the command-line tool (downloads JUCE)" ON)
option(TUNEAFISH_COPY_PLUGIN "Install the plugin into the system plug-in folders after building" ON)

# The pitch-correction engine. Plain C++ with no JUCE, shared by the plugin, the
# command-line tool and the tests.
add_library(tuneafish_dsp STATIC
    Source/dsp/PitchDetector.cpp
    Source/dsp/PitchCorrector.cpp)

target_include_directories(tuneafish_dsp PUBLIC Source)
set_target_properties(tuneafish_dsp PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(tuneafish_dsp PRIVATE /W4)
else()
    target_compile_options(tuneafish_dsp PRIVATE -Wall -Wextra -Wshadow)
endif()

enable_testing()
add_executable(tuneafish_tests Tests/Tests.cpp)
target_link_libraries(tuneafish_tests PRIVATE tuneafish_dsp)
add_test(NAME tuneafish_tests COMMAND tuneafish_tests)

if(NOT TUNEAFISH_BUILD_PLUGIN)
    return()
endif()

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        9.0.5
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(TuneAFish
    COMPANY_NAME             "Tune-a-Fish"
    BUNDLE_ID                com.tuneafish.tuneafish
    PLUGIN_MANUFACTURER_CODE Tnfs
    PLUGIN_CODE              Tafh
    PRODUCT_NAME             "Tune-a-Fish"
    DESCRIPTION              "Vocal pitch correction"

    # An audio effect that also listens to MIDI, for the MIDI target mode. In Live, route a
    # MIDI track's output to this plugin on the vocal track.
    IS_SYNTH          FALSE
    NEEDS_MIDI_INPUT  TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT    FALSE
    VST3_CATEGORIES   Fx Pitch Shift

    FORMATS VST3
    COPY_PLUGIN_AFTER_BUILD ${TUNEAFISH_COPY_PLUGIN})

target_sources(TuneAFish
    PRIVATE
        Source/PluginEditor.cpp
        Source/PluginProcessor.cpp
        Source/PitchView.cpp)

target_include_directories(TuneAFish PRIVATE Source)

target_compile_definitions(TuneAFish
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(TuneAFish
    PRIVATE
        tuneafish_dsp
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Command-line tool: tune an audio file, or print its pitch.
juce_add_console_app(TuneAFishCli PRODUCT_NAME "tune-a-fish")

target_sources(TuneAFishCli PRIVATE Tools/TuneFile.cpp)
set_target_properties(TuneAFishCli PROPERTIES OUTPUT_NAME tune-a-fish)

target_compile_definitions(TuneAFishCli
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)

target_link_libraries(TuneAFishCli
    PRIVATE
        tuneafish_dsp
        juce::juce_audio_formats
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
