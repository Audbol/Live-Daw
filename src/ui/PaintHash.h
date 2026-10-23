#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace livedaw::ui
{

/** A fingerprint of what a component (and its children) would draw: its
    paint runs into a graphics context that rasterises nothing and only hashes
    every drawing call with its fill, font, transform and clip. Pages that poll
    their state on a timer compare it with the last one and repaint only when
    it differs, so an unchanged page costs a paint walk instead of a redraw. */
juce::uint64 paintHash (juce::Component& c);

} // namespace livedaw::ui
