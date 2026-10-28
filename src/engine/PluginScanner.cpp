#include "engine/PluginScanner.h"

#include <condition_variable>
#include <mutex>
#include <queue>

namespace livedaw
{

static constexpr const char* kScannerProcessId = "livedawpluginscanner";

//==============================================================================
class OutOfProcessScanner::Coordinator final : private juce::ChildProcessCoordinator
{
public:
    Coordinator()
    {
        launched = launchWorkerProcess (juce::File::getSpecialLocation (juce::File::currentExecutableFile),
                                        kScannerProcessId, 0, 0);
    }

    bool isLaunched() const noexcept { return launched; }

    enum class State
    {
        timeout,
        gotResult,
        connectionLost
    };

    struct Response
    {
        State state;
        std::unique_ptr<juce::XmlElement> xml;
    };

    Response getResponse()
    {
        std::unique_lock<std::mutex> lock { mutex };

        if (! condition.wait_for (lock, std::chrono::milliseconds { 50 }, [this] { return gotResult || connectionLost; }))
            return { State::timeout, nullptr };

        const auto state = connectionLost ? State::connectionLost : State::gotResult;
        gotResult = false;
        connectionLost = false;
        return { state, std::move (descriptions) };
    }

    using juce::ChildProcessCoordinator::sendMessageToWorker;

private:
    void handleMessageFromWorker (const juce::MemoryBlock& mb) override
    {
        const std::lock_guard<std::mutex> lock { mutex };
        descriptions = juce::parseXML (mb.toString());
        gotResult = true;
        condition.notify_one();
    }

    void handleConnectionLost() override
    {
        const std::lock_guard<std::mutex> lock { mutex };
        connectionLost = true;
        condition.notify_one();
    }

    std::mutex mutex;
    std::condition_variable condition;
    std::unique_ptr<juce::XmlElement> descriptions;
    bool gotResult = false, connectionLost = false, launched = false;
};

OutOfProcessScanner::OutOfProcessScanner (int timeoutMs) : scanTimeoutMs (timeoutMs) {}
OutOfProcessScanner::~OutOfProcessScanner() = default;

bool OutOfProcessScanner::findPluginTypesFor (juce::AudioPluginFormat& format,
                                              juce::OwnedArray<juce::PluginDescription>& result,
                                              const juce::String& fileOrIdentifier)
{
    if (coordinator == nullptr)
        coordinator = std::make_unique<Coordinator>();

    if (! coordinator->isLaunched())
    {
        // Couldn't start a scanner process (e.g. sandboxed); fall back to scanning in-process.
        coordinator.reset();
        format.findAllTypesForFile (result, fileOrIdentifier);
        return true;
    }

    juce::MemoryBlock block;
    juce::MemoryOutputStream stream { block, true };
    stream.writeString (format.getName());
    stream.writeString (fileOrIdentifier);

    if (! coordinator->sendMessageToWorker (block))
    {
        coordinator.reset();
        return false;
    }

    const auto deadline = juce::Time::getMillisecondCounterHiRes() + scanTimeoutMs;

    for (;;)
    {
        if (shouldExit())
            return true;

        auto response = coordinator->getResponse();

        if (response.state == Coordinator::State::timeout)
        {
            if (juce::Time::getMillisecondCounterHiRes() < deadline)
                continue;

            // The scanner hung on this plugin. Dropping the connection ends the
            // scanner (see Worker::handleConnectionLost); report it as failed.
            coordinator.reset();
            return false;
        }

        if (response.xml != nullptr)
        {
            for (const auto* item : response.xml->getChildIterator())
            {
                auto desc = std::make_unique<juce::PluginDescription>();
                if (desc->loadFromXml (*item))
                    result.add (std::move (desc));
            }
        }

        if (response.state == Coordinator::State::gotResult)
            return true;

        // The scanner died on this plugin: report it as failed and restart the scanner.
        coordinator.reset();
        return false;
    }
}

void OutOfProcessScanner::scanFinished()
{
    coordinator.reset();
}

//==============================================================================
namespace
{
    class Worker final : public PluginScannerWorker,
                         private juce::ChildProcessWorker,
                         private juce::AsyncUpdater
    {
    public:
        explicit Worker (const std::function<void (juce::AudioPluginFormatManager&)>& addFormats)
        {
            juce::addDefaultFormatsToManager (formatManager);
            if (addFormats)
                addFormats (formatManager);
        }

        using juce::ChildProcessWorker::initialiseFromCommandLine;

    private:
        void handleMessageFromCoordinator (const juce::MemoryBlock& mb) override
        {
            if (mb.isEmpty())
                return;

            const std::lock_guard<std::mutex> lock (mutex);

            if (const auto results = doScan (mb); ! results.isEmpty())
            {
                sendResults (results);
            }
            else
            {
                // Some formats must be scanned on the message thread.
                pending.emplace (mb);
                triggerAsyncUpdate();
            }
        }

        // Exit straight away rather than through the message loop: when a plugin hangs
        // the scan, the message thread never gets back to the loop to quit.
        void handleConnectionLost() override { juce::Process::terminate(); }

        void handleAsyncUpdate() override
        {
            for (;;)
            {
                const std::lock_guard<std::mutex> lock (mutex);

                if (pending.empty())
                    return;

                sendResults (doScan (pending.front()));
                pending.pop();
            }
        }

        juce::OwnedArray<juce::PluginDescription> doScan (const juce::MemoryBlock& block)
        {
            juce::MemoryInputStream stream { block, false };
            const auto formatName = stream.readString();
            const auto identifier = stream.readString();

            juce::PluginDescription pd;
            pd.fileOrIdentifier = identifier;
            pd.uniqueId = pd.deprecatedUid = 0;

            juce::OwnedArray<juce::PluginDescription> results;

            for (auto* format : formatManager.getFormats())
            {
                if (format->getName() != formatName)
                    continue;

                if (juce::MessageManager::getInstance()->isThisTheMessageThread()
                    || format->requiresUnblockedMessageThreadDuringCreation (pd))
                    format->findAllTypesForFile (results, identifier);
            }

            return results;
        }

        void sendResults (const juce::OwnedArray<juce::PluginDescription>& results)
        {
            juce::XmlElement xml ("LIST");
            for (auto* desc : results)
                xml.addChildElement (desc->createXml().release());

            const auto text = xml.toString();
            sendMessageToCoordinator ({ text.toRawUTF8(), text.getNumBytesAsUTF8() });
        }

        std::mutex mutex;
        std::queue<juce::MemoryBlock> pending;
        juce::AudioPluginFormatManager formatManager;
    };
} // namespace

std::unique_ptr<PluginScannerWorker> PluginScannerWorker::tryToStart (const juce::String& commandLine,
                                                                     const std::function<void (juce::AudioPluginFormatManager&)>& addFormats)
{
    auto worker = std::make_unique<Worker> (addFormats);

    if (worker->initialiseFromCommandLine (commandLine, kScannerProcessId))
        return worker;

    return nullptr;
}

} // namespace livedaw
