#include <juce_audio_devices/juce_audio_devices.h>

/** Checks that the platform's pro audio driver types are compiled in. CI runs
    this on Windows, macOS and Linux runners. */
class DeviceTests final : public juce::UnitTest
{
public:
    DeviceTests() : juce::UnitTest ("Devices", "LiveDAW") {}

    void runTest() override
    {
        beginTest ("Pro audio driver types are available");

        juce::AudioDeviceManager manager;
        juce::OwnedArray<juce::AudioIODeviceType> types;
        manager.createAudioDeviceTypes (types);

        juce::StringArray names;
        for (auto* t : types)
            names.add (t->getTypeName());
        logMessage ("Driver types: " + names.joinIntoString (", "));

       #if JUCE_WINDOWS
        expect (names.contains ("ASIO"), "ASIO support is not compiled in");
        expect (names.contains ("Windows Audio"), "WASAPI support is missing");
       #elif JUCE_MAC
        expect (names.contains ("CoreAudio"), "CoreAudio support is missing");
       #elif JUCE_LINUX
        expect (names.contains ("ALSA"), "ALSA support is missing");
        expect (names.contains ("JACK"), "JACK support is missing");
       #endif
    }
};

static DeviceTests deviceTests;
