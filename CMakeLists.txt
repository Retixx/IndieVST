cmake_minimum_required(VERSION 3.22)

project(Forge
    VERSION 0.1.0
    DESCRIPTION "AI instrument generator VST3"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(FORGE_BUILD_PLUGIN "Build the JUCE VST3 / Standalone plugin" ON)
option(FORGE_BUILD_TESTS  "Build the headless core test suite"       ON)
option(FORGE_ASAN         "Enable AddressSanitizer (dev builds)"     OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /MP /bigobj)
    # Flush-to-zero / denormals-are-zero friendly, but never fast-math:
    # the DSP relies on well-defined NaN/Inf behaviour for the safety scan.
    add_compile_options(/fp:precise)
else()
    add_compile_options(-Wall -Wextra -Wshadow -Wno-unused-parameter)
endif()

if(FORGE_ASAN AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

include(FetchContent)

# ---------------------------------------------------------------------------
# forge_core : the entire DSP engine, IR, validator and prompt builder.
#
# This target has NO dependency on JUCE. That is deliberate:
#   * it can be compiled and unit-tested headlessly on any machine / CI box,
#   * the real-time code has no framework between it and the samples,
#   * it can later be reused by the Phase 3 "player" export shell (SPEC 14.3).
# JUCE is only used for the plugin wrapper, the GUI and HTTP.
# ---------------------------------------------------------------------------
add_library(forge_core STATIC
    src/core/dsp/Registry.cpp
    src/core/dsp/Modules.cpp
    src/core/dsp/ModulesFx.cpp
    src/core/dsp/AssetBaker.cpp
    src/core/dsp/GraphInstance.cpp
    src/core/dsp/GraphBuilder.cpp
    src/core/dsp/VoiceManager.cpp
    src/core/ir/Ir.cpp
    src/core/ir/IrValidator.cpp
    src/core/ir/IrSafety.cpp
    src/core/ir/IrRepair.cpp
    src/core/llm/PromptBuilder.cpp
    src/core/llm/CannedLibrary.cpp
    src/core/llm/ResponseExtract.cpp
)

target_include_directories(forge_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_compile_definitions(forge_core PUBLIC
    JSON_USE_IMPLICIT_CONVERSIONS=0   # force explicit .get<T>() - fewer silent surprises
    JSON_NOEXCEPTION=0                # we parse untrusted input with try/catch
)

set_target_properties(forge_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# The JUCE plugin
# ---------------------------------------------------------------------------
if(FORGE_BUILD_PLUGIN)
    # Where the built .vst3 gets installed after a successful build.
    #
    # JUCE's default on Windows is C:\Program Files\Common Files\VST3, which
    # needs an elevated shell. Building a plugin as Administrator on every
    # iteration is both annoying and a bad habit, so we default to the
    # per-user VST3 location instead - it is part of the VST3 spec, and it
    # needs no special permissions.
    #
    # Override with -DFORGE_VST3_COPY_DIR="C:/Program Files/Common Files/VST3"
    # (from an elevated shell) if you would rather install system-wide.
    set(FORGE_VST3_COPY_DIR "" CACHE PATH
        "Install location for the built VST3. Empty = JUCE default.")
    if(WIN32 AND NOT FORGE_VST3_COPY_DIR AND DEFINED ENV{LOCALAPPDATA})
        set(FORGE_VST3_COPY_DIR "$ENV{LOCALAPPDATA}/Programs/Common/VST3")
    endif()

    set(FORGE_COPY_ARGS)
    if(FORGE_VST3_COPY_DIR)
        list(APPEND FORGE_COPY_ARGS VST3_COPY_DIR "${FORGE_VST3_COPY_DIR}")
        message(STATUS "Forge: VST3 will be installed to ${FORGE_VST3_COPY_DIR}")
    endif()

    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.4
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(JUCE)

    juce_add_plugin(Forge
        COMPANY_NAME              "IndieVST"
        COMPANY_WEBSITE           "https://github.com/Retixx/IndieVST"
        BUNDLE_ID                 "com.indievst.forge"
        PLUGIN_MANUFACTURER_CODE  Ivst
        PLUGIN_CODE               Frg1
        FORMATS                   VST3 Standalone
        PRODUCT_NAME              "Forge"
        DESCRIPTION               "AI instrument generator"
        IS_SYNTH                  TRUE
        NEEDS_MIDI_INPUT          TRUE
        NEEDS_MIDI_OUTPUT         FALSE
        IS_MIDI_EFFECT            FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE
        COPY_PLUGIN_AFTER_BUILD   TRUE
        VST3_CATEGORIES           "Instrument" "Synth"
        ${FORGE_COPY_ARGS}
    )

    target_sources(Forge PRIVATE
        src/plugin/PluginProcessor.cpp
        src/plugin/ParameterPool.cpp
        src/plugin/GraphPublisher.cpp
        src/plugin/InstrumentLibrary.cpp
        src/plugin/ForgeConfig.cpp
        src/llm/HttpLlmProvider.cpp
        src/llm/GenerationSession.cpp
        src/ui/Fonts.cpp
        src/ui/PluginEditor.cpp
        src/ui/ChatView.cpp
        src/ui/InstrumentView.cpp
        src/ui/ControlFactory.cpp
        src/ui/GraphView.cpp
        src/ui/ForgeLookAndFeel.cpp
    )

    target_compile_definitions(Forge PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        # JUCE_DISPLAY_SPLASH_SCREEN is not set: JUCE 8 removed the splash
        # screen entirely and warns if you pass the flag.
        JUCE_REPORT_APP_USAGE=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_MODAL_LOOPS_PERMITTED=0
    )

    target_link_libraries(Forge
        PRIVATE
            forge_core
            # juce_audio_utils pulls in juce_audio_devices, which the Standalone
            # wrapper needs. juce_dsp is deliberately NOT linked: every filter,
            # oscillator and effect in Forge is hand-written in forge_core, and
            # linking a module we do not use only lengthens the build.
            juce::juce_audio_utils
            juce::juce_audio_processors
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags
    )

    # Inter, subset to Latin and instanced to three static weights, embedded so
    # the UI looks identical on every machine and needs nothing installed.
    juce_add_binary_data(ForgeResources SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/InterRegular.ttf"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/InterMedium.ttf"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/InterSemiBold.ttf"
    )
    set_target_properties(ForgeResources PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(Forge PRIVATE ForgeResources)

    # NOTE: the offline fallback instruments are compiled into forge_core as
    # string literals (src/core/llm/CannedLibrary.cpp) rather than bundled as
    # binary resources. They double as the few-shot examples in the LLM prompt,
    # so having exactly one copy of them - which the test suite validates on
    # every build - matters more than the convenience of loose files.
    # resources/canned/*.json is a human-readable mirror, not a build input.
endif()

# ---------------------------------------------------------------------------
# Headless tests - no JUCE required, runs anywhere
# ---------------------------------------------------------------------------
if(FORGE_BUILD_TESTS)
    enable_testing()
    add_executable(forge_tests
        tests/main.cpp
        tests/test_validator.cpp
        tests/test_repair.cpp
        tests/test_dsp.cpp
        tests/test_graph.cpp
        tests/fuzz_ir.cpp
    )
    target_link_libraries(forge_tests PRIVATE forge_core)
    add_test(NAME forge_tests COMMAND forge_tests)
endif()
