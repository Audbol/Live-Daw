#include "engine/VirtualAudioDevice.h"

#include <chrono>
#include <thread>

namespace livedaw
{

namespace
{
juce::StringArray channelNames (const char* prefix)
{
    juce::StringArray names;
    for (int i = 0; i < VirtualAudioIODeviceType::kNumChannels; ++i)
        names.add (juce::String (prefix) + " " + juce::String (i + 1));
    return names;
}

/** The device: a thread that calls the callback once per block, on time. */
class VirtualAudioIODevice final : public juce::AudioIODevice,
                                   private juce::Thread
{
public:
    VirtualAudioIODevice()
        : juce::AudioIODevice (VirtualAudioIODeviceType::kDeviceName, VirtualAudioIODeviceType::kTypeName),
          juce::Thread ("LiveDAW virtual audio device")
    {
    }

    ~VirtualAudioIODevice() override { close(); }

    juce::StringArray getOutputChannelNames() override { return channelNames ("Out"); }
    juce::StringArray getInputChannelNames() override { return channelNames ("In"); }
    juce::Array<double> getAvailableSampleRates() override { return { 44100.0, 48000.0, 88200.0, 96000.0 }; }
    juce::Array<int> getAvailableBufferSizes() override { return { 32, 64, 128, 256, 512, 1024, 2048 }; }
    int getDefaultBufferSize() override { return 256; }

    juce::String open (const juce::BigInteger& inputChannels, const juce::BigInteger& outputChannels,
                       double newSampleRate, int newBufferSize) override
    {
        close();
        const auto clip = [] (juce::BigInteger channels)
        {
            channels.setRange (VirtualAudioIODeviceType::kNumChannels,
                               juce::jmax (0, channels.getHighestBit() + 1 - VirtualAudioIODeviceType::kNumChannels), false);
            return channels;
        };
        activeInputs = clip (inputChannels);
        activeOutputs = clip (outputChannels);
        sampleRate = getAvailableSampleRates().contains (newSampleRate) ? newSampleRate : 48000.0;
        bufferSize = newBufferSize > 0 ? juce::jlimit (16, 8192, newBufferSize) : getDefaultBufferSize();
        opened = true;
        return {};
    }

    void close() override
    {
        stop();
        opened = false;
    }

    bool isOpen() override { return opened; }

    void start (juce::AudioIODeviceCallback* newCallback) override
    {
        if (! opened || newCallback == nullptr)
            return;
        stop();
        newCallback->audioDeviceAboutToStart (this);
        {
            const juce::ScopedLock sl (callbackLock);
            callback = newCallback;
        }
        const auto options = juce::Thread::RealtimeOptions {}.withApproximateAudioProcessingTime (bufferSize, sampleRate);
        if (! startRealtimeThread (options))
            startThread (juce::Thread::Priority::highest);
    }

    void stop() override
    {
        juce::AudioIODeviceCallback* previous = nullptr;
        {
            const juce::ScopedLock sl (callbackLock); // waits for a callback in progress
            previous = std::exchange (callback, nullptr);
        }
        stopThread (4000);
        if (previous != nullptr)
            previous->audioDeviceStopped();
    }

    bool isPlaying() override
    {
        const juce::ScopedLock sl (callbackLock);
        return callback != nullptr && isThreadRunning();
    }

    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return bufferSize; }
    double getCurrentSampleRate() override { return sampleRate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return activeOutputs; }
    juce::BigInteger getActiveInputChannels() const override { return activeInputs; }
    int getOutputLatencyInSamples() override { return bufferSize; }
    int getInputLatencyInSamples() override { return bufferSize; }
    int getXRunCount() const noexcept override { return xruns.load(); }

private:
    void run() override
    {
        using Clock = std::chrono::steady_clock;
        const int numIns = activeInputs.countNumberOfSetBits();
        const int numOuts = activeOutputs.countNumberOfSetBits();
        juce::AudioBuffer<float> ins (juce::jmax (1, numIns), bufferSize);
        juce::AudioBuffer<float> outs (juce::jmax (1, numOuts), bufferSize);
        const auto block = std::chrono::duration<double> (bufferSize / sampleRate);

        auto origin = Clock::now();
        int64_t blocks = 0;
        uint64_t hostTimeNs = 0;
        juce::AudioIODeviceCallbackContext context;
        context.hostTimeNs = &hostTimeNs;

        while (! threadShouldExit())
        {
            const auto due = origin + std::chrono::duration_cast<Clock::duration> (block * (double) blocks);
            const auto now = Clock::now();
            if (due > now)
            {
                std::this_thread::sleep_until (due);
            }
            else if (now - due > std::chrono::milliseconds (500))
            {
                // Stalled (debugger, suspend): start counting again rather than racing.
                origin = now;
                blocks = 0;
                ++xruns;
            }

            hostTimeNs = (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds> (Clock::now().time_since_epoch()).count();
            {
                const juce::ScopedLock sl (callbackLock);
                if (callback != nullptr)
                {
                    ins.clear(); // silent inputs, whatever the callback did to them
                    callback->audioDeviceIOCallbackWithContext (ins.getArrayOfReadPointers(), numIns,
                                                                outs.getArrayOfWritePointers(), numOuts,
                                                                bufferSize, context);
                }
            }
            ++blocks;
        }
    }

    juce::CriticalSection callbackLock;
    juce::AudioIODeviceCallback* callback = nullptr;
    juce::BigInteger activeInputs, activeOutputs;
    double sampleRate = 48000.0;
    int bufferSize = 256;
    bool opened = false;
    std::atomic<int> xruns { 0 };
};
} // namespace

VirtualAudioIODeviceType::VirtualAudioIODeviceType() : juce::AudioIODeviceType (kTypeName) {}

juce::StringArray VirtualAudioIODeviceType::getDeviceNames (bool) const
{
    return { kDeviceName };
}

int VirtualAudioIODeviceType::getDefaultDeviceIndex (bool) const
{
    return 0;
}

int VirtualAudioIODeviceType::getIndexOfDevice (juce::AudioIODevice* device, bool) const
{
    return device != nullptr && device->getName() == kDeviceName ? 0 : -1;
}

juce::AudioIODevice* VirtualAudioIODeviceType::createDevice (const juce::String& outputDeviceName,
                                                              const juce::String& inputDeviceName)
{
    const auto name = outputDeviceName.isNotEmpty() ? outputDeviceName : inputDeviceName;
    if (name.isNotEmpty() && name != kDeviceName)
        return nullptr;
    return new VirtualAudioIODevice();
}

} // namespace livedaw
