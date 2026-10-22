#include "engine/PluginScanner.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <iostream>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init; // engine uses the message manager / timers

    // The out-of-process plugin scanner relaunches this executable as its worker.
    {
        juce::StringArray args;
        for (int i = 1; i < argc; ++i)
            args.add (argv[i]);

        if (auto worker = livedaw::PluginScannerWorker::tryToStart (args.joinIntoString (" ")))
        {
            juce::MessageManager::getInstance()->runDispatchLoop();
            return 0;
        }
    }

    // JUCE's runner logs through juce::Logger, which on Windows goes to the
    // debugger only; print to stdout so a CI log shows what failed.
    struct StdoutRunner final : juce::UnitTestRunner
    {
        void logMessage (const juce::String& message) override { std::cout << message << std::endl; }
    } runner;
    runner.setAssertOnFailure (false);

    if (argc > 1)
        runner.runTestsInCategory (argv[1]);
    else
        runner.runTestsInCategory ("LiveDAW");

    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
        if (auto* r = runner.getResult (i))
            failures += r->failures;

    std::cout << (failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED: " + std::to_string (failures)) << std::endl;
    return failures == 0 ? 0 : 1;
}
