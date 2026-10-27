cmake_minimum_required(VERSION 3.22)
project(openWallLimiter VERSION 1.0.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/utf-8)
endif()
option(OWL_BUILD_PLUGIN "Build native audio plugins and host tests" ON)
include(CTest)
add_library(wall_core STATIC Source/Core/Limiter.cpp)
target_include_directories(wall_core PUBLIC Source/Core)
set_target_properties(wall_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(BUILD_TESTING)
  add_executable(wall_core_tests Tests/LimiterTests.cpp)
  target_link_libraries(wall_core_tests PRIVATE wall_core)
  add_test(NAME limiter COMMAND wall_core_tests)
endif()
if(OWL_BUILD_PLUGIN)
  include(FetchContent)
  FetchContent_Declare(juce GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.9 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(juce)
  juce_add_binary_data(WallAssets SOURCES assets/fortress.png)
  set_target_properties(WallAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)
  set(OWL_FORMATS VST3 Standalone)
  if(APPLE)
    list(APPEND OWL_FORMATS AU)
  endif()
  juce_add_plugin(openWallLimiter
    COMPANY_NAME "Alan Marcero"
    COMPANY_WEBSITE "https://github.com/alanmarcero/openWallLimiter"
    COPYRIGHT "Copyright (c) 2026 Alan Marcero"
    BUNDLE_ID "com.alanmarcero.openwalllimiter"
    PRODUCT_NAME "openWallLimiter"
    PLUGIN_MANUFACTURER_CODE Amrc PLUGIN_CODE Owli
    FORMATS ${OWL_FORMATS} VST3_CATEGORIES Fx Dynamics Mastering
    IS_SYNTH FALSE NEEDS_MIDI_INPUT FALSE NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)
  target_sources(openWallLimiter PRIVATE
    Source/Plugin/PluginProcessor.cpp Source/Plugin/PluginEditor.cpp Source/Plugin/Parameters.cpp
    Source/Filters/BiquadFilter.cpp Source/Filters/ButterworthCoeffs.cpp Source/Filters/MultiPoleFilter.cpp
    Source/Metering/LoudnessMeter.cpp)
  target_include_directories(openWallLimiter PUBLIC Source/Plugin Source/Filters Source/Metering)
  target_compile_definitions(openWallLimiter PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
  target_link_libraries(openWallLimiter PRIVATE wall_core WallAssets juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
  if(BUILD_TESTING)
    add_executable(wall_plugin_tests Tests/PluginTests.cpp)
    target_link_libraries(wall_plugin_tests PRIVATE openWallLimiter wall_core WallAssets juce::juce_audio_utils juce::juce_dsp)
    target_compile_definitions(wall_plugin_tests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    add_test(NAME plugin COMMAND wall_plugin_tests "${CMAKE_BINARY_DIR}/screenshots")
  endif()
endif()
