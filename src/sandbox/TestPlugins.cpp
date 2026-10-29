#include "sandbox/TestPlugins.h"

#include <csignal>
#include <thread>

namespace livedaw::sandbox
{

namespace
{
    const char* kindName (TestPluginFormat::Kind k)
    {
        switch (k)
        {
            case TestPluginFormat::Kind::crash: return "crash";
            case TestPluginFormat::Kind::hang:  return "hang";
            case TestPluginFormat::Kind::gain:  break;
        }
        return "gain";
    }

    class TestPlugin final : public juce::AudioPluginInstance
    {
    public:
        explicit TestPlugin (TestPluginFormat::Kind k)
            : juce::AudioPluginInstance (BusesProperties()
                                             .withInput ("In", juce::AudioChannelSet::stereo())
                                             .withOutput ("Out", juce::AudioChannelSet::stereo())),
              kind (k)
        {
            addHostedParameter (std::make_unique<Param> ("gain", "Gain", 0.5f));     // 0..2 linear
            addHostedParameter (std::make_unique<Param> ("tone", "Tone", 0.25f));    // one-pole coefficient
            addHostedParameter (std::make_unique<Param> ("fault", "Fault", 0.0f));   // > 0.5 misbehaves
        }

        void fillInPluginDescription (juce::PluginDescription& d) const override { d = TestPluginFormat::describe (kind); }
        const juce::String getName() const override { return TestPluginFormat::describe (kind).name; }

        void prepareToPlay (double, int) override { std::fill (std::begin (state), std::end (state), 0.0f); }
        void releaseResources() override {}

        using juce::AudioPluginInstance::processBlock;

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            const auto& params = getParameters();
            if (params[2]->getValue() > 0.5f)
            {
                if (kind == TestPluginFormat::Kind::crash)
                    std::raise (SIGSEGV);
                if (kind == TestPluginFormat::Kind::hang)
                    for (;;)
                        std::this_thread::sleep_for (std::chrono::milliseconds (50));
            }

            const float g = 2.0f * params[0]->getValue();
            const float a = juce::jlimit (0.0f, 0.99f, params[1]->getValue());
            for (int ch = 0; ch < juce::jmin (2, buffer.getNumChannels()); ++ch)
            {
                auto* d = buffer.getWritePointer (ch);
                float z = state[ch];
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                {
                    z = d[i] + a * (z - d[i]);
                    d[i] = g * z;
                }
                state[ch] = z;
            }
        }

        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor (*this); }
        bool hasEditor() const override { return true; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const juce::String getProgramName (int) override { return "Default"; }
        void changeProgramName (int, const juce::String&) override {}

        void getStateInformation (juce::MemoryBlock& data) override
        {
            juce::MemoryOutputStream out (data, false);
            for (auto* p : getParameters())
                out.writeFloat (p->getValue());
        }

        void setStateInformation (const void* data, int size) override
        {
            juce::MemoryInputStream in (data, (size_t) size, false);
            for (auto* p : getParameters())
                if (in.getNumBytesRemaining() >= 4)
                    p->setValueNotifyingHost (in.readFloat());
        }

    private:
        struct Param final : public juce::AudioPluginInstance::HostedParameter
        {
            Param (juce::String i, juce::String n, float d) : id (std::move (i)), name (std::move (n)), defaultValue (d), value (d) {}

            juce::String getParameterID() const override { return id; }
            float getValue() const override { return value.load(); }
            void setValue (float v) override { value = v; }
            float getDefaultValue() const override { return defaultValue; }
            juce::String getName (int maxLength) const override { return name.substring (0, maxLength); }
            juce::String getLabel() const override { return {}; }
            juce::String getText (float v, int) const override { return juce::String (v, 3); }
            float getValueForText (const juce::String& text) const override { return text.getFloatValue(); }

            juce::String id, name;
            float defaultValue;
            std::atomic<float> value;
        };

        TestPluginFormat::Kind kind;
        float state[2] {};
    };
} // namespace

juce::PluginDescription TestPluginFormat::describe (Kind kind)
{
    juce::PluginDescription d;
    d.name = juce::String ("Sidecar Test ") + kindName (kind);
    d.descriptiveName = d.name;
    d.pluginFormatName = kFormatName;
    d.category = "Effect";
    d.manufacturerName = "Sidecar Tests";
    d.version = "1.0";
    d.fileOrIdentifier = juce::String ("sidecar-test:") + kindName (kind);
    d.uniqueId = d.deprecatedUid = d.fileOrIdentifier.hashCode();
    d.numInputChannels = d.numOutputChannels = 2;
    return d;
}

std::unique_ptr<juce::AudioPluginInstance> TestPluginFormat::create (Kind kind)
{
    return std::make_unique<TestPlugin> (kind);
}

bool TestPluginFormat::isEnabledByEnvironment()
{
    return juce::SystemStats::getEnvironmentVariable (kEnvironmentVariable, {}) == "1";
}

void TestPluginFormat::findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results, const juce::String& id)
{
    if (id == kHangingScanId)
        for (;;)
            juce::Thread::sleep (1000);

    for (auto kind : { Kind::gain, Kind::crash, Kind::hang })
        if (describe (kind).fileOrIdentifier == id)
            results.add (new juce::PluginDescription (describe (kind)));
}

void TestPluginFormat::createPluginInstance (const juce::PluginDescription& desc, double, int, PluginCreationCallback callback)
{
    for (auto kind : { Kind::gain, Kind::crash, Kind::hang })
        if (describe (kind).fileOrIdentifier == desc.fileOrIdentifier)
            return callback (create (kind), {});
    callback (nullptr, "unknown test plugin");
}

} // namespace livedaw::sandbox
