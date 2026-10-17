#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

namespace livedaw
{

/** An audio device type with no hardware behind it: "Virtual (no hardware)".

    Its one device runs the audio callback from its own thread at the chosen
    sample rate and block size, keeping time against the system clock (it
    resynchronises after a long stall instead of catching up), with
    kNumChannels silent inputs and kNumChannels discarded outputs. It lets the
    application run, meter and roll the transport on machines without a sound
    card (CI, containers, a laptop on the road).

    Register it after the platform's types (AudioDeviceManager::addAudioDeviceType
    once getAvailableDeviceTypes() has created them), so it never wins over real
    hardware by default: it has to be chosen in SETUP > AUDIO or in the saved
    device settings. */
class VirtualAudioIODeviceType final : public juce::AudioIODeviceType
{
public:
    static constexpr const char* kTypeName = "Virtual (no hardware)";
    static constexpr const char* kDeviceName = "Virtual 64 x 64";
    static constexpr int kNumChannels = 64;

    VirtualAudioIODeviceType();

    void scanForDevices() override {}
    juce::StringArray getDeviceNames (bool wantInputNames = false) const override;
    int getDefaultDeviceIndex (bool forInput) const override;
    int getIndexOfDevice (juce::AudioIODevice* device, bool asInput) const override;
    bool hasSeparateInputsAndOutputs() const override { return false; }
    juce::AudioIODevice* createDevice (const juce::String& outputDeviceName, const juce::String& inputDeviceName) override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VirtualAudioIODeviceType)
};

} // namespace livedaw
