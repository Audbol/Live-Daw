#pragma once

#include "engine/ConsoleModel.h"

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
    The MODEL is the full-size WING or the WING COMPACT (the RACK may follow);
    the COMPACT comes in black only, so it shows the black finish whatever the
    finish setting says. */
struct ConsoleLook
{
    static constexpr const char* finishKey = "ui.surfaceFinish"; // "SILVER" (default) or "BLACK"
    static constexpr const char* modelKey = "ui.consoleModel";   // "FULL" (default) or "COMPACT"

    SurfaceFinish finish = SurfaceFinish::Silver;
    ConsoleModel model = ConsoleModel::Full;

    static juce::String finishName (SurfaceFinish f) { return f == SurfaceFinish::Black ? "BLACK" : "SILVER"; }

    /** The finish the surface shows: the COMPACT's is black. */
    SurfaceFinish shownFinish() const noexcept { return model == ConsoleModel::Compact ? SurfaceFinish::Black : finish; }

    static ConsoleLook load (const juce::PropertiesFile& settings)
    {
        ConsoleLook look;
        look.finish = settings.getValue (finishKey) == finishName (SurfaceFinish::Black) ? SurfaceFinish::Black : SurfaceFinish::Silver;
        look.model = consoleModelFromName (settings.getValue (modelKey));
        return look;
    }

    void save (juce::PropertiesFile& settings) const
    {
        settings.setValue (finishKey, finishName (finish));
        settings.setValue (modelKey, consoleModelName (model));
    }
};

} // namespace livedaw::ui
