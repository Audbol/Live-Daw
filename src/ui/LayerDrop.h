#pragma once

#include "engine/Layers.h"

#include <juce_graphics/juce_graphics.h>

#include <vector>

namespace livedaw::layerdrop
{

/** Where a drop at `pos` lands on VIEW > CONFIG's slots (`slots`: the page's
    slot rectangles, left to right; `first`: the layer index of the first).
    Near the boundary between two slots (the outer quarter of a slot, the gap
    between two) it inserts there; on the rest of a slot's body it fills or
    replaces that slot. The zones scale with the slots, so they work on a
    small screen and under a finger. */
inline layerslots::DropTarget targetAt (const std::vector<juce::Rectangle<int>>& slots, int first, juce::Point<int> pos)
{
    for (size_t i = 0; i < slots.size(); ++i)
    {
        const auto r = slots[i].expanded (3, 6);
        if (! r.contains (pos))
            continue;
        const int edge = juce::jmax (6, slots[i].getWidth() / 4);
        const int index = first + (int) i;
        if (pos.x < slots[i].getX() + edge)
            return { index, true };
        if (pos.x >= slots[i].getRight() - edge)
            return { index + 1, true };
        return { index, false };
    }
    // In the gap between two slots (between the groups of four).
    for (size_t i = 0; i + 1 < slots.size(); ++i)
    {
        const auto& a = slots[i];
        const auto& b = slots[i + 1];
        if (pos.x >= a.getRight() && pos.x < b.getX() && pos.y >= a.getY() - 6 && pos.y < a.getBottom() + 6)
            return { first + (int) i + 1, true };
    }
    return {};
}

/** The insertion marker for an insert before page slot `local` (0 to
    slots.size()): a bar centred on the boundary, a little taller than the slots. */
inline juce::Rectangle<int> markerAt (const std::vector<juce::Rectangle<int>>& slots, int local, int thickness)
{
    if (slots.empty() || local < 0 || local > (int) slots.size())
        return {};
    int x;
    if (local == 0)
        x = slots.front().getX() - 3;
    else if (local == (int) slots.size())
        x = slots.back().getRight() + 3;
    else
        x = (slots[(size_t) local - 1].getRight() + slots[(size_t) local].getX()) / 2;
    const auto& ref = slots[(size_t) juce::jmin (local, (int) slots.size() - 1)];
    const int extra = juce::jmax (4, ref.getHeight() / 8);
    return { x - thickness / 2, ref.getY() - extra, thickness, ref.getHeight() + 2 * extra };
}

} // namespace livedaw::layerdrop
