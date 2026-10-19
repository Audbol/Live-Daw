#pragma once

#include <juce_events/juce_events.h>

namespace livedaw
{

/** Timestamped log of system events (device restarts, plugin faults, recalls...).
    Message thread only. Listeners are notified via ChangeBroadcaster. */
class EventLog final : public juce::ChangeBroadcaster
{
public:
    enum class Level
    {
        Info,
        Warning,
        Error
    };

    struct Entry
    {
        juce::Time time;
        Level level;
        juce::String message;
    };

    void add (Level level, const juce::String& message)
    {
        entries.add ({ juce::Time::getCurrentTime(), level, message });
        if (entries.size() > kMaxEntries)
            entries.removeRange (0, entries.size() - kMaxEntries);

        juce::Logger::writeToLog ("[Sidecar] " + message);
        sendChangeMessage();
    }

    void info (const juce::String& m) { add (Level::Info, m); }
    void warning (const juce::String& m) { add (Level::Warning, m); }
    void error (const juce::String& m) { add (Level::Error, m); }

    const juce::Array<Entry>& getEntries() const noexcept { return entries; }

private:
    static constexpr int kMaxEntries = 500;
    juce::Array<Entry> entries;
};

} // namespace livedaw
