cmake_minimum_required(VERSION 3.22)

# The version. Release builds pass the release tag (e.g. -DLIVEDAW_VERSION=v0.1.1-beta.2):
# its numeric part becomes the CMake/JUCE version (Windows version info, Info.plist), and
# the full string is what the app shows (about box, SETUP, log) as JUCE_APPLICATION_VERSION_STRING.
set(LIVEDAW_DEFAULT_VERSION 0.1.2)
set(LIVEDAW_VERSION "" CACHE STRING "Version to stamp, e.g. 0.1.1-beta.2 or v0.1.1-beta.2 (default: ${LIVEDAW_DEFAULT_VERSION})")
if(LIVEDAW_VERSION)
    string(REGEX REPLACE "^[vV]" "" LIVEDAW_VERSION_STRING "${LIVEDAW_VERSION}")
    if(NOT LIVEDAW_VERSION_STRING MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)([-+][0-9A-Za-z.+-]*)?$")
        message(FATAL_ERROR "LIVEDAW_VERSION '${LIVEDAW_VERSION}' is not MAJOR.MINOR.PATCH[-suffix]")
    endif()
    set(LIVEDAW_VERSION_NUMBER "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
else()
    set(LIVEDAW_VERSION_STRING ${LIVEDAW_DEFAULT_VERSION})
    set(LIVEDAW_VERSION_NUMBER ${LIVEDAW_DEFAULT_VERSION})
endif()

# std::atomic::wait (used by the realtime worker pool) needs macOS 11+. Set before
# project() so the toolchain picks it up.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

project(LiveDAW VERSION ${LIVEDAW_VERSION_NUMBER} LANGUAGES C CXX)
message(STATUS "Sidecar version ${LIVEDAW_VERSION_STRING}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    # Static runtime: the .exe runs on any Windows PC without the VC++ redistributable.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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
    src/engine/SandboxedPlugin.cpp
    src/sandbox/SandboxShared.cpp
    src/engine/ConsoleState.cpp
    src/engine/Layers.cpp
    src/engine/Show.cpp
    src/engine/AudioFileFormat.cpp
    src/engine/Recorder.cpp
    src/engine/UsbRecorder.cpp
    src/engine/VirtualAudioDevice.cpp
    src/engine/WingRemote.cpp)

# MIDI remote control and DAW remote control (MCU / HUI).
list(APPEND LIVEDAW_ENGINE_SOURCES
    src/engine/DawRemote.cpp
    src/engine/MidiRemote.cpp)

# The multitrack project model (no UI), shared by the app and the tests.
set(LIVEDAW_DAW_SOURCES
    src/daw/Project.cpp
    src/daw/TimeGrid.cpp
    src/daw/AudioFiles.cpp
    src/daw/Session.cpp)

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
        JUCE_USE_FLAC=1                 # imported into recording projects
        JUCE_USE_MP3AUDIOFORMAT=1
        JUCE_USE_OGGVORBIS=0
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:${target},JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="${LIVEDAW_VERSION_STRING}")

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
set(LIVEDAW_PLIST_EXTRAS "<plist version=\"1.0\"><dict>
<key>LSMinimumSystemVersion</key><string>${CMAKE_OSX_DEPLOYMENT_TARGET}</string>
<key>LSApplicationCategoryType</key><string>public.app-category.music</string>
</dict></plist>")

# The icon (resources/icon, made by packaging/make_icon.py): JUCE turns
# ICON_BIG/ICON_SMALL into the .icns (macOS) and the .exe icon (Windows).
juce_add_gui_app(LiveDAW
    PRODUCT_NAME "Sidecar"
    COMPANY_NAME "Sidecar"
    COMPANY_COPYRIGHT "Copyright (C) Sidecar contributors. Licensed under the GNU AGPLv3."
    BUNDLE_ID "org.sidecar.app"
    VERSION ${PROJECT_VERSION}
    ICON_BIG ${CMAKE_SOURCE_DIR}/resources/icon/sidecar-1024.png
    ICON_SMALL ${CMAKE_SOURCE_DIR}/resources/icon/sidecar-256.png
    PLIST_TO_MERGE "${LIVEDAW_PLIST_EXTRAS}"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Sidecar needs access to your audio inputs to mix them.")

target_sources(LiveDAW PRIVATE
    src/Main.cpp
    ${LIVEDAW_ENGINE_SOURCES}
    ${LIVEDAW_DAW_SOURCES}
    src/ui/LookAndFeel.cpp
    src/ui/Icons.cpp
    src/ui/Knobs.cpp
    src/ui/ModelParams.cpp
    src/ui/Faceplate.cpp
    src/ui/FaceplateDraw.cpp
    src/ui/FaceplateDesigns.cpp
    src/ui/MeterComponent.cpp
    src/ui/PluginMenu.cpp
    src/ui/FaderStrip.cpp
    src/ui/FaderSection.cpp
    src/ui/ResponseGraph.cpp
    src/ui/DynamicsGraph.cpp
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
    src/ui/daw/DawCommon.cpp
    src/ui/daw/DawTimeline.cpp
    src/ui/daw/DawTracks.cpp
    src/ui/daw/DawEditor.cpp
    src/ui/daw/RecorderPage.cpp
    src/ui/UsbRecorderPage.cpp
    src/ui/WingPopups.cpp
    src/ui/StartupNotice.cpp
    src/ui/CustomControlsPage.cpp
    src/ui/MonitorsPage.cpp
    src/ui/FourChannelSection.cpp
    src/ui/MainComponent.cpp)

# MIDI remote control and DAW remote control: the UI side.
target_sources(LiveDAW PRIVATE src/ui/MidiRemoteUi.cpp src/ui/DawSurface.cpp)

livedaw_configure_juce_target(LiveDAW)

# Barlow Condensed and Barlow Semi Condensed (SIL Open Font License,
# resources/fonts/OFL.txt): DIN-style faces like the WING's. The semi-condensed
# cut matches the width of the console's screen font; the surface print stays
# condensed.
juce_add_binary_data(LiveDAWResources SOURCES
    resources/fonts/BarlowCondensed-Medium.ttf
    resources/fonts/BarlowCondensed-SemiBold.ttf
    resources/fonts/BarlowSemiCondensed-Medium.ttf
    resources/fonts/BarlowSemiCondensed-SemiBold.ttf
    resources/icon/sidecar-256.png)   # the window icon on Linux
target_link_libraries(LiveDAW PRIVATE LiveDAWResources)

#-------------------------------------------------------------------------------
# Plugin sandbox helper (src/sandbox): Sidecar starts one SidecarPluginHost per
# sandboxed plugin. It ships next to the app's executable (Linux, Windows) or in
# Sidecar.app/Contents/Helpers (macOS); the build copies it there.
#-------------------------------------------------------------------------------
set(LIVEDAW_HELPER_PLIST_EXTRAS "<plist version=\"1.0\"><dict>
<key>LSMinimumSystemVersion</key><string>${CMAKE_OSX_DEPLOYMENT_TARGET}</string>
<key>LSUIElement</key><true/>
</dict></plist>")

juce_add_gui_app(SidecarPluginHost
    PRODUCT_NAME "SidecarPluginHost"
    COMPANY_NAME "Sidecar"
    COMPANY_COPYRIGHT "Copyright (C) Sidecar contributors. Licensed under the GNU AGPLv3."
    BUNDLE_ID "org.sidecar.pluginhost"
    VERSION ${PROJECT_VERSION}
    PLIST_TO_MERGE "${LIVEDAW_HELPER_PLIST_EXTRAS}")

target_sources(SidecarPluginHost PRIVATE
    src/sandbox/HostProcess.cpp
    src/sandbox/SandboxShared.cpp
    src/sandbox/TestPlugins.cpp)
target_include_directories(SidecarPluginHost PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(SidecarPluginHost PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=1
    JUCE_APPLICATION_NAME_STRING="SidecarPluginHost"
    JUCE_APPLICATION_VERSION_STRING="${LIVEDAW_VERSION_STRING}")
if(APPLE)
    target_compile_definitions(SidecarPluginHost PRIVATE JUCE_PLUGINHOST_AU=1)
endif()
if(LIVEDAW_VST2_SDK_DIR)
    target_compile_definitions(SidecarPluginHost PRIVATE JUCE_PLUGINHOST_VST=1)
endif()
target_link_libraries(SidecarPluginHost
    PRIVATE
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
        livedaw_warning_overrides)
if(APPLE)
    # AU plugin editors (AUGenericView); the app gets it through juce_audio_utils.
    target_link_libraries(SidecarPluginHost PRIVATE "-framework CoreAudioKit")
endif()

add_dependencies(LiveDAW SidecarPluginHost)
if(APPLE)
    add_custom_command(TARGET SidecarPluginHost POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:LiveDAW>/Helpers"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_BUNDLE_DIR:SidecarPluginHost>"
                "$<TARGET_BUNDLE_CONTENT_DIR:LiveDAW>/Helpers/SidecarPluginHost.app"
        VERBATIM)
else()
    add_custom_command(TARGET SidecarPluginHost POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:LiveDAW>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SidecarPluginHost>" "$<TARGET_FILE_DIR:LiveDAW>"
        VERBATIM)
endif()

#-------------------------------------------------------------------------------
# Tests
#-------------------------------------------------------------------------------
if(LIVEDAW_BUILD_TESTS)
    enable_testing()

    juce_add_console_app(LiveDAWTests PRODUCT_NAME "LiveDAW Tests")
    target_sources(LiveDAWTests PRIVATE
        tests/TestMain.cpp
        tests/EngineTests.cpp
        tests/ConsoleTests.cpp
        tests/ShowTests.cpp
        tests/StabilityTests.cpp
        tests/PluginHostingTests.cpp
        tests/LayerTests.cpp
        tests/DeviceTests.cpp
        tests/RecorderTests.cpp
        tests/UsbRecorderTests.cpp
        tests/DawTests.cpp
        tests/WingTests.cpp
        tests/SandboxTests.cpp
        src/sandbox/TestPlugins.cpp
        ${LIVEDAW_ENGINE_SOURCES}
        ${LIVEDAW_DAW_SOURCES})
    livedaw_configure_juce_target(LiveDAWTests)

    # The sandbox tests start the real helper (with its test plugins).
    add_dependencies(LiveDAWTests SidecarPluginHost)
    target_compile_definitions(LiveDAWTests PRIVATE LIVEDAW_TEST_PLUGIN_HOST="$<TARGET_FILE:SidecarPluginHost>")

    add_test(NAME LiveDAWTests COMMAND LiveDAWTests)

    # MIDI remote control and DAW remote control.
    target_sources(LiveDAWTests PRIVATE tests/MidiTests.cpp)
endif()
