cmake_minimum_required(VERSION 3.22)

project(LiveDAW VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    # Static runtime: the .exe runs on any Windows PC without the VC++ redistributable.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(APPLE)
    # std::atomic::wait (used by the realtime worker pool) needs macOS 11+.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

#-------------------------------------------------------------------------------
# Options
#-------------------------------------------------------------------------------
option(LIVEDAW_BUILD_TESTS "Build the unit test runner" ON)
set(LIVEDAW_JUCE_DIR "" CACHE PATH "Path to a local JUCE checkout (otherwise JUCE is fetched)")
set(LIVEDAW_JUCE_TAG "8.0.15" CACHE STRING "JUCE git tag to fetch when LIVEDAW_JUCE_DIR is empty")
option(LIVEDAW_ASIO "Windows only: enable ASIO (uses the ASIO SDK bundled with JUCE, GPLv3)" ON)
set(LIVEDAW_ASIO_SDK_DIR "" CACHE PATH "Windows only, optional: use this ASIO SDK instead of JUCE's bundled copy")
set(LIVEDAW_VST2_SDK_DIR "" CACHE PATH "Optional: path to a legacy VST2 SDK (enables VST2 hosting)")
set(LIVEDAW_SANITIZE "" CACHE STRING "Build with sanitizers (GCC/Clang), e.g. 'address,undefined' or 'thread'")

if(LIVEDAW_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=${LIVEDAW_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=${LIVEDAW_SANITIZE})
endif()

#-------------------------------------------------------------------------------
# JUCE
#-------------------------------------------------------------------------------
if(LIVEDAW_JUCE_DIR)
    add_subdirectory(${LIVEDAW_JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        ${LIVEDAW_JUCE_TAG}
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

#-------------------------------------------------------------------------------
# Rust engine (rust/): DSP, mixing, routing and stability core, linked as a
# static library through a C ABI (rust/crates/livedaw-ffi/include/livedaw_engine.h)
#-------------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG        v0.6.1
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(Corrosion)

corrosion_import_crate(MANIFEST_PATH rust/Cargo.toml CRATES livedaw-ffi CRATE_TYPES staticlib)
target_include_directories(livedaw_ffi INTERFACE ${CMAKE_SOURCE_DIR}/rust/crates/livedaw-ffi/include)

if(MSVC)
    # Match the app's static C runtime (portable .exe).
    corrosion_add_target_rustflags(livedaw_ffi -Ctarget-feature=+crt-static)
endif()

#-------------------------------------------------------------------------------
# Shared engine sources + JUCE configuration (used by the app and the tests)
#-------------------------------------------------------------------------------
set(LIVEDAW_ENGINE_SOURCES
    src/engine/FxSlots.cpp
    src/engine/Models.cpp
    src/engine/Strip.cpp
    src/engine/MixerEngine.cpp
    src/engine/PluginHost.cpp
    src/engine/PluginScanner.cpp
    src/engine/ConsoleState.cpp
    src/engine/Layers.cpp
    src/engine/Show.cpp
    src/engine/Recorder.cpp)

# Linked after JUCE's recommended warnings so these flags come last on the command line.
add_library(livedaw_warning_overrides INTERFACE)
if(NOT MSVC)
    # Exact float comparisons are intentional (e.g. skipping unity/zero gain ramps).
    target_compile_options(livedaw_warning_overrides INTERFACE -Wno-float-equal)
endif()

function(livedaw_configure_juce_target target)
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/src)

    target_compile_definitions(${target} PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_LV2=1
        JUCE_USE_FLAC=0
        JUCE_USE_OGGVORBIS=0
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:${target},JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:${target},JUCE_VERSION>")

    if(APPLE)
        target_compile_definitions(${target} PRIVATE JUCE_PLUGINHOST_AU=1)
    endif()

    if(UNIX AND NOT APPLE)
        # ALSA is on by default in JUCE; JACK is enabled as a bonus for pro Linux setups.
        target_compile_definitions(${target} PRIVATE JUCE_ALSA=1 JUCE_JACK=1)
    endif()

    if(WIN32)
        target_compile_definitions(${target} PRIVATE JUCE_WASAPI=1 JUCE_DIRECTSOUND=1)

        if(LIVEDAW_ASIO)
            # JUCE 8.0.11+ ships the ASIO SDK headers (Steinberg relicensed them under GPLv3).
            target_compile_definitions(${target} PRIVATE JUCE_ASIO=1)

            if(LIVEDAW_ASIO_SDK_DIR)
                target_compile_definitions(${target} PRIVATE JUCE_ASIO_USE_EXTERNAL_SDK=1)
                target_include_directories(${target} PRIVATE
                    "${LIVEDAW_ASIO_SDK_DIR}/common"
                    "${LIVEDAW_ASIO_SDK_DIR}/host"
                    "${LIVEDAW_ASIO_SDK_DIR}/host/pc")
            endif()
        endif()
    endif()

    if(LIVEDAW_VST2_SDK_DIR)
        juce_set_vst2_sdk_path(${LIVEDAW_VST2_SDK_DIR})
        target_compile_definitions(${target} PRIVATE JUCE_PLUGINHOST_VST=1)
    endif()

    target_link_libraries(${target}
        PRIVATE
            livedaw_ffi
            juce::juce_audio_utils
            juce::juce_audio_devices
            juce::juce_audio_processors
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags
            livedaw_warning_overrides)
endfunction()

#-------------------------------------------------------------------------------
# Application
#-------------------------------------------------------------------------------
juce_add_gui_app(LiveDAW
    PRODUCT_NAME "Live DAW"
    COMPANY_NAME "LiveDAW"
    BUNDLE_ID "org.livedaw.app"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Live DAW needs access to your audio inputs to mix them.")

target_sources(LiveDAW PRIVATE
    src/Main.cpp
    ${LIVEDAW_ENGINE_SOURCES}
    src/ui/LookAndFeel.cpp
    src/ui/Icons.cpp
    src/ui/Knobs.cpp
    src/ui/ModelParams.cpp
    src/ui/Faceplate.cpp
    src/ui/MeterComponent.cpp
    src/ui/PluginMenu.cpp
    src/ui/FaderStrip.cpp
    src/ui/FaderSection.cpp
    src/ui/ResponseGraph.cpp
    src/ui/HomeScreens.cpp
    src/ui/HomePage.cpp
    src/ui/EffectsPage.cpp
    src/ui/MetersPage.cpp
    src/ui/RoutingPage.cpp
    src/ui/SetupPage.cpp
    src/ui/LibraryPage.cpp
    src/ui/UtilityPage.cpp
    src/ui/ShowPanel.cpp
    src/ui/ScopeWindow.cpp
    src/ui/ViewPages.cpp
    src/ui/MainDisplay.cpp
    src/ui/SurfaceSections.cpp
    src/ui/MainComponent.cpp)

livedaw_configure_juce_target(LiveDAW)

# Barlow Condensed (SIL Open Font License, resources/fonts/OFL.txt): a DIN-style
# condensed face like the WING's screen font.
juce_add_binary_data(LiveDAWResources SOURCES
    resources/fonts/BarlowCondensed-Medium.ttf
    resources/fonts/BarlowCondensed-SemiBold.ttf)
target_link_libraries(LiveDAW PRIVATE LiveDAWResources)

#-------------------------------------------------------------------------------
# Tests
#-------------------------------------------------------------------------------
if(LIVEDAW_BUILD_TESTS)
    enable_testing()

    juce_add_console_app(LiveDAWTests PRODUCT_NAME "LiveDAW Tests")
    target_sources(LiveDAWTests PRIVATE
        tests/TestMain.cpp
        tests/EngineTests.cpp
        tests/ShowTests.cpp
        tests/StabilityTests.cpp
        tests/PluginHostingTests.cpp
        tests/LayerTests.cpp
        tests/DeviceTests.cpp
        ${LIVEDAW_ENGINE_SOURCES})
    livedaw_configure_juce_target(LiveDAWTests)

    add_test(NAME LiveDAWTests COMMAND LiveDAWTests)
endif()
