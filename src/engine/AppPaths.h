#pragma once

#include <juce_core/juce_core.h>

namespace livedaw
{

/** The app's name as the user sees it. */
inline constexpr const char* kAppName = "Sidecar";

/** The app's folders. The app was called "Live DAW" before; an existing
    folder under the old name keeps being used (projects refer to their
    audio by path, so it isn't moved). */
namespace paths
{
    inline juce::File chooseFolder (const juce::File& parent, const char* name, const char* oldName)
    {
        const auto current = parent.getChildFile (name);
        const auto old = parent.getChildFile (oldName);
        return ! current.exists() && old.isDirectory() ? old : current;
    }

    /** <app data>/Sidecar (settings, logs, safe-mode state). */
    inline juce::File appData()
    {
        return chooseFolder (juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory), kAppName, "LiveDAW");
    }

    /** <documents>/Sidecar (recordings, USB, library, screenshots). */
    inline juce::File documents()
    {
        return chooseFolder (juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), kAppName, "Live DAW");
    }
} // namespace paths

} // namespace livedaw
