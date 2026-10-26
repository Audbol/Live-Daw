#pragma once

#include "livedaw_engine.h"

#include <juce_core/juce_core.h>

namespace livedaw
{

/** The console model Sidecar shows (SETUP > GENERAL > CONSOLE): the
    full-size WING or the WING COMPACT. The values cross the C ABI
    (livedaw-session/model.rs); 2 is kept for the WING RACK. */
enum class ConsoleModel
{
    Full = 0,
    Compact = 1
};

/** The settings' and SETUP's name of a model: "FULL", "COMPACT". */
inline juce::String consoleModelName (ConsoleModel m)
{
    return m == ConsoleModel::Compact ? "COMPACT" : "FULL";
}

/** Unknown names (a later model, the RACK) read as the full-size console. */
inline ConsoleModel consoleModelFromName (const juce::String& name)
{
    return name.trim().equalsIgnoreCase ("COMPACT") ? ConsoleModel::Compact : ConsoleModel::Full;
}

/** A model's surface and local I/O: the full-size WING has 8 local MIDAS PRO
    preamps, 8 aux inputs and outputs and 2x2 GPIO; the COMPACT 24 preamps, no
    local aux I/O and 1x2 GPIO (WING remote protocols, "Wing, a family"). */
inline ld::ConsoleModelInfo consoleModelInfo (ConsoleModel m)
{
    ld::ConsoleModelInfo info {};
    ld::ld_console_model_info ((int32_t) m, &info);
    return info;
}

} // namespace livedaw
