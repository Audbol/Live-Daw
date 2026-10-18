#pragma once

#include "ui/FaderStrip.h"

namespace livedaw::ui
{

/** One of the WING's three fader sections: the layer column (VIEW, the seven
    layer keys, the shift keys ◄4 / 4► and any further user layers; SOF FLIP on
    the center section) and the fader strips. A layer longer than the section
    is scrolled with the shift keys, 4 faders at a time (8 or 12 per
    SETUP > SURFACE); holding them jumps to the first / last layer. While the
    section spills (a DCA, bus, FX slot, automix group or a strip's sends) it
    shows the spill and no layer key is lit; a layer key ends the spill. */
class FaderSection final : public juce::Component
{
public:
    static constexpr int kMinStripWidth = 54;
    static constexpr int kBankColumnWidth = 56;

    FaderSection (UiContext&, LayerSet::Section);

    /** Rebuilds the layer keys and re-resolves the faders (structure/layer changes). */
    void refresh();
    /** Re-resolves the faders only (selection / SOF changes). */
    void refreshFaders();
    /** Selects the layer key holding `fader` and scrolls it into view. */
    void reveal (const state::FaderRef&);

    /** Number of strips shown right now. */
    int getNumStrips() const noexcept { return (int) strips.size(); }
    /** The strip on fader `index` (nullptr for a DCA, an empty position or past the end). */
    Strip* getStrip (int index) const { return juce::isPositiveAndBelow (index, strips.size()) ? strips[index]->getStrip() : nullptr; }
    /** Width needed for `n` strips of width `stripWidth`. */
    static int widthFor (int n, int stripWidth) { return kBankColumnWidth + n * stripWidth; }

    /** For Auto width: how many strips fit `width`. */
    static int fitStrips (int width) { return juce::jmax (1, (width - kBankColumnWidth) / kMinStripWidth); }

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

private:
    void selectLayer (const juce::String& id);
    void shift (int direction);
    void jumpToEnd (int direction);
    int shiftStep() const;
    bool isKeyLit (const Layer&) const;

    UiContext& ctx;
    LayerSet::Section section;
    std::vector<Layer> keys; // the layer keys, then user layers 3+
    int numFixedKeys = 0;    // the layer keys
    juce::String userIds[2]; // the user layers on USER 1 / USER 2
    Layer layer;
    int offset = 0; // first fader shown

    ViewKey viewKey;
    juce::OwnedArray<SurfaceKey> layerKeys;
    SurfaceKey shiftLeft, shiftRight, sofKey { "SOF\nFLIP" };
    juce::OwnedArray<FaderStrip> strips;
    juce::Rectangle<int> column;
};

} // namespace livedaw::ui
