#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace livedaw
{

/** Scans plugins in a child process (the app relaunched with a special command
    line), so a plugin that crashes while being scanned takes down only the
    scanner process. The crashing plugin is reported as failed and the scan
    carries on with a fresh scanner process. A plugin that hangs while being
    scanned is treated the same way once scanTimeoutMs has passed: JUCE's own
    ping timeout can't fire while the message thread waits here. */
class OutOfProcessScanner final : public juce::KnownPluginList::CustomScanner
{
public:
    static constexpr int kDefaultScanTimeoutMs = 60000;

    explicit OutOfProcessScanner (int scanTimeoutMs = kDefaultScanTimeoutMs);
    ~OutOfProcessScanner() override;

    bool findPluginTypesFor (juce::AudioPluginFormat&, juce::OwnedArray<juce::PluginDescription>& result,
                             const juce::String& fileOrIdentifier) override;
    void scanFinished() override;

private:
    class Coordinator;
    std::unique_ptr<Coordinator> coordinator;
    int scanTimeoutMs;
};

/** The child-process side. Call tryToStart() first thing in initialise(): if the
    app was launched as a scanner it returns an object to keep alive (and the app
    should do nothing else); otherwise it returns nullptr. addFormats, if given,
    adds formats beyond the default ones (the tests add their test format). */
class PluginScannerWorker
{
public:
    virtual ~PluginScannerWorker() = default;
    static std::unique_ptr<PluginScannerWorker> tryToStart (const juce::String& commandLine,
                                                            const std::function<void (juce::AudioPluginFormatManager&)>& addFormats = {});
};

} // namespace livedaw
