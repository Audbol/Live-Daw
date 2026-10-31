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

    /** The first of `folders` the system knows. A special location comes back
        empty when Windows can't resolve it (Documents redirected to a share or
        to OneDrive and not there), and an empty parent would put the app's
        folders at the root of the drive (C:\Sidecar), which a standard user
        can't create (#12). */
    inline juce::File firstKnown (std::initializer_list<juce::File> folders)
    {
        for (const auto& f : folders)
            if (f != juce::File())
                return f;
        return juce::File::getSpecialLocation (juce::File::tempDirectory);
    }

    /** The user's Documents, or Documents in their home folder, or app data. */
    inline juce::File userDocuments()
    {
        const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
        return firstKnown ({ juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                             home != juce::File() ? home.getChildFile ("Documents") : juce::File(),
                             juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory) });
    }

    /** <app data>/Sidecar (settings, logs, safe-mode state). */
    inline juce::File appData()
    {
        return chooseFolder (firstKnown ({ juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory),
                                           juce::File::getSpecialLocation (juce::File::windowsLocalAppData) }),
                             kAppName, "LiveDAW");
    }

    /** <documents>/Sidecar (recordings, USB, library, screenshots). */
    inline juce::File documents()
    {
        return chooseFolder (userDocuments(), kAppName, "Live DAW");
    }
} // namespace paths

} // namespace livedaw
