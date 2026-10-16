#pragma once

#include <juce_graphics/juce_graphics.h>

namespace livedaw::ui
{

/** Vector icons in the style of the WING's screen. */
enum class Icon
{
    Gear,
    Tools,
    Wrench,
    Xlr,        // input channel
    Jack,       // aux channel
    Bus,        // mix bus
    Speakers,   // main
    Matrix,
    Headphones,
    Speaker,
    LockOpen,
    LockClosed,
    Plus,
    LowCut,
    HighCut,
    Tilt,
    Gate,
    Compressor,
    Insert,
    Eq,
    Folder,
    Check,
    Reset,
    Edit,
    Trash,
    Play,
    Stop,
    ChevronLeft,
    ChevronRight,
    Disk
};

/** Draws `icon` fitted into `area` (stroked or filled as the icon needs). */
void drawIcon (juce::Graphics&, Icon, juce::Rectangle<float> area, juce::Colour);

} // namespace livedaw::ui
