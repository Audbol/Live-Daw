#pragma once

#include <juce_data_structures/juce_data_structures.h>

namespace livedaw::ui
{

/** The control surface's finish: Behringer makes the full-size WING in
    silver and in black (WING BK). Only the surface view's hardware colours
    differ; the layout, the print and the touchscreen are the same. */
enum class SurfaceFinish
{
    Silver,
    Black
};

/** How the console looks (SETUP > GENERAL > CONSOLE), kept in the app
    settings rather than in the show: a preference of the user, not of the mix.
    The console MODEL (full-size, COMPACT, RACK) is planned to sit beside the
    finish here; until then the surface is the full-size WING's. */
struct ConsoleLook
{
    static constexpr const char* finishKey = "ui.surfaceFinish"; // "SILVER" (default) or "BLACK"

    SurfaceFinish finish = SurfaceFinish::Silver;

    static juce::String finishName (SurfaceFinish f) { return f == SurfaceFinish::Black ? "BLACK" : "SILVER"; }

    static ConsoleLook load (const juce::PropertiesFile& settings)
    {
        ConsoleLook look;
        look.finish = settings.getValue (finishKey) == finishName (SurfaceFinish::Black) ? SurfaceFinish::Black : SurfaceFinish::Silver;
        return look;
    }

    void save (juce::PropertiesFile& settings) const { settings.setValue (finishKey, finishName (finish)); }
};

} // namespace livedaw::ui
