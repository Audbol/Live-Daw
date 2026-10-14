#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace livedaw
{

/** Scans plugins in a child process (the app relaunched with a special command
    line), so a plugin that crashes while being scanned takes down only the
    scanner process. The crashing plugin is reported as failed and the scan
    carries on with a fresh scanner process. */
class OutOfProcessScanner final : public juce::KnownPluginList::CustomScanner
{
public:
    OutOfProcessScanner();
    ~OutOfProcessScanner() override;

    bool findPluginTypesFor (juce::AudioPluginFormat&, juce::OwnedArray<juce::PluginDescription>& result,
                             const juce::String& fileOrIdentifier) override;
    void scanFinished() override;

private:
    class Coordinator;
    std::unique_ptr<Coordinator> coordinator;
};

/** The child-process side. Call tryToStart() first thing in initialise(): if the
    app was launched as a scanner it returns an object to keep alive (and the app
    should do nothing else); otherwise it returns nullptr. */
class PluginScannerWorker
{
public:
    virtual ~PluginScannerWorker() = default;
    static std::unique_ptr<PluginScannerWorker> tryToStart (const juce::String& commandLine);
};

} // namespace livedaw
