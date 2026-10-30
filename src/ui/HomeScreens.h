#pragma once

#include "ui/HomeWidgets.h"
#include "ui/Knobs.h"
#include "ui/ModelParams.h"
#include "ui/UiContext.h"

#include <memory>
#include <type_traits>
#include <vector>

namespace livedaw::ui
{

/** One of the six knobs under the display: what it changes and how the
    parameter bar shows it. */
struct HomeKnob
{
    KnobBinding binding;
    KnobStyle style {};
};

/** Base of the HOME screens of a strip: helpers to create bound controls and
    to lay out and draw WING panels. */
class HomeScreen : public juce::Component
{
public:
    HomeScreen (UiContext& c, Strip& s) : ctx (c), strip (s) {}

    /** The six knobs under the display for this screen (empty slots allowed). */
    virtual std::vector<HomeKnob> knobs() { return {}; }
    std::function<void()> knobsChanged;

    /** The screen has a SETTINGS key in the status bar (OVERVIEW, FILTER, EQ). */
    virtual bool hasSettings() const { return false; }

    /** The status bar shows none of the HOME keys (PREV CH ... COPY), as on the
        3.1 GATE / COMP KEY FILTER and XOVER pages. */
    virtual bool hidesStatusKeys() const { return false; }
    std::function<void()> statusKeysChanged;

    /** Opens the Feed Configuration of this bus, main or matrix (set by the page). */
    std::function<void()> openFeedConfig;

    void paint (juce::Graphics&) override;

protected:
    template <typename T, typename... Args>
    T& make (Args&&... args)
    {
        auto* c = new T (std::forward<Args> (args)...);
        // The HOME screens' fields have the 3.1 console's frame, key and type sizes.
        if constexpr (std::is_same_v<T, home::ChoiceField>)
            c->consoleMetrics = true;
        owned.add (c);
        addAndMakeVisible (c);
        return *c;
    }

    juce::Label& label (const juce::String& text, float size = 15.0f, juce::Colour colour = Colours::textDim);
    DisplayKnob& knob (KnobBinding);
    BoolButton& toggle (const juce::String& text, std::atomic<bool>& p, juce::Colour on = Colours::yellow);
    IntCombo& combo (const juce::StringArray& items, std::atomic<int32_t>* p, const juce::String& noneText = {});
    home::IconKey& key (const juce::String& text, std::optional<Icon> icon = std::nullopt, juce::Colour on = Colours::yellow);
    /** A drop-down field for an atomic choice (0..n-1; -1 shown as `noneText`)
        that opens the WING's on-screen list (W10). */
    home::ChoiceField& field (const juce::String& caption, const juce::StringArray& items, std::atomic<int32_t>& value,
                              const juce::String& noneText = {});

    /** A rectangle given in WING screen pixels (1280 x 800, firmware 3.1)
        in this screen's coordinates (the HOME page starts at y 80). */
    juce::Rectangle<int> wingRect (float x, float y, float w, float h) const
    {
        const float k = getWingScreenHeight() / 800.0f;
        const int x0 = juce::roundToInt (x * k) - getX(), y0 = juce::roundToInt ((y - 80.0f) * k) - getY();
        return { x0, y0, juce::roundToInt ((x + w) * k) - getX() - x0, juce::roundToInt ((y + h - 80.0f) * k) - getY() - y0 };
    }
    /** The area to give box() for a panel drawn at these WING pixels (box()
        insets its area by half the 8 px gap). */
    juce::Rectangle<int> panelAt (float x, float y, float w, float h) const { return wingRect (x - 4.0f, y - 4.0f, w + 8.0f, h + 8.0f); }

    /** Adds a panel (drawn in paint) and returns its content area. */
    juce::Rectangle<int> box (juce::Rectangle<int> area, const juce::String& title, bool chevron = false);
    void clearBoxes() { boxes.clear(); }

    /** The LEVEL panel that stays at the right on OVERVIEW, ICON/COLOR, NAME
        and TAGS: add it in the constructor, lay it out first in resized()
        (where the overview has it; the rest of `area` is returned). */
    home::LevelPanel& addLevelPanel();
    juce::Rectangle<int> layoutLevelPanel (juce::Rectangle<int> area);
    home::LevelPanel* levelPanel = nullptr;

    void notifyKnobs()
    {
        if (knobsChanged)
            knobsChanged();
    }

    struct Panel
    {
        juce::Rectangle<int> area;
        juce::String title;
        bool chevron;
    };

    UiContext& ctx;
    Strip& strip;
    juce::OwnedArray<juce::Component> owned;
    std::vector<Panel> boxes;
};

constexpr int kKeyHeight = 30;

/** Places components side by side, equally wide. */
void layoutRow (juce::Rectangle<int>, std::initializer_list<juce::Component*>, int gap = 4);

/** "CH 1 Kick", ... for every channel (key sources, matrix direct inputs). */
juce::StringArray channelNames (MixerEngine&);

/** The threshold-like parameter of a dynamics slot ("thr", else the first). */
KnobBinding thresholdBinding (DynSlotParams&, const juce::String& label);

/** A grid of keys editing the bits of an atomic mask (DCAs, mute groups). */
class MaskButtons final : public juce::Component, private juce::Timer
{
public:
    MaskButtons (std::atomic<uint32_t>& mask, int count, const juce::String& prefix, juce::Colour on, int columns = 8);
    void resized() override;

private:
    void timerCallback() override;
    std::atomic<uint32_t>& mask;
    juce::OwnedArray<juce::TextButton> buttons;
    int numColumns;
};

/** A routing-style source tile [3.1, D16] for the input `left` (`right` when
    stereo): the name strip, the icon, the signal ring, 48V / MUTE and the
    port. Used by INPUT and the overview's INPUT panel. */
void paintInputSourceTile (juce::Graphics&, juce::Rectangle<float> r, MixerEngine&, int left, int right, bool stereo, bool signal);

/** The input DELAY as a knob, in its unit over the WING's 0.1 .. 150 m (the
    INPUT screen and the channel strip section share it). */
KnobBinding inputDelayBinding (UiContext&, InputParams&);

/** The GAIN of the source on the strip's MAIN input (both sources of a pair),
    as the WING's preamp gain: -12 .. +60 dB in 0.5 dB steps. */
KnobBinding sourceGainBinding (UiContext&, Strip&);

/** The FILTER block's LC FREQ and HC FREQ knobs (FILTER SETUP, the EQ screen). */
inline KnobBinding lowCutFreqBinding (FilterParams& f)
{
    return home::wingFreq ("LC FREQ", f.lowCutFreq, 20.0f, 2000.0f, kLowCutDefaultHz);
}

inline KnobBinding highCutFreqBinding (FilterParams& f)
{
    return home::wingFreq ("HC FREQ", f.highCutFreq, 50.0f, 20000.0f, kHighCutDefaultHz);
}

/** The FILTER block on the WING EQ's knobs (channels, firmware 3.1.2): LC
    FREQ on the first while LO CUT is on, the TILT EQ's TILT on the second
    while the tool filter is the TILT EQ and on, HC FREQ on the sixth while HI
    CUT is on. A filter that is off leaves its knob to the EQ (or empty), as
    the key filters leave the knobs of the parameters they don't use. `k`
    holds the EQ's knobs and is padded to six. */
inline void addEqFilterKnobs (std::vector<HomeKnob>& k, FilterParams& f, juce::Component* target = nullptr)
{
    if (k.size() < 6)
        k.resize (6);
    if (f.lowCut.load())
        k[0] = { lowCutFreqBinding (f), home::freqStyle (target) };
    if (const int tool = f.toolModel.load(); f.tool.load() && home::toolFilterToken (tool) == "TILT")
        if (const auto& spec = models::model (models::FilterTools, tool); ! spec.params.empty())
            k[1] = { bindConsoleParam (f.toolParams[0], spec.params[0]), home::toolStyle (target, home::toolFilterColour (tool)) };
    if (f.highCut.load())
        k[5] = { highCutFreqBinding (f), home::freqStyle (target) };
}

// The block screens.
std::unique_ptr<HomeScreen> makeInputScreen (UiContext&, Strip&);
std::unique_ptr<HomeScreen> makeFilterScreen (UiContext&, Strip&);
std::unique_ptr<HomeScreen> makeDynamicsScreen (UiContext&, Strip&, bool gateSlot);
std::unique_ptr<HomeScreen> makeEqScreen (UiContext&, Strip&);
std::unique_ptr<HomeScreen> makeInsertScreen (UiContext&, Strip&, bool postInsert);
std::unique_ptr<HomeScreen> makeMainScreen (UiContext&, Strip&);
/** BUS SENDS (`matrixPage` false) or MATRIX SENDS. */
std::unique_ptr<HomeScreen> makeSendsScreen (UiContext&, Strip&, bool matrixPage);

// The overlay screens of Channel Home.
/** CHANNEL INIT or CHANNEL COPY, with the scope preselected for the slot `from`. */
std::unique_ptr<home::OverlayScreen> makeInitCopyScreen (UiContext&, Strip&, bool copy, HomeTab from);
/** INITIALIZE CONSOLE (SETUP): the strips, DCAs, mute groups and FX slots of
    `scope` back to a fresh console's (the strips within its sections, as
    CHANNEL INIT does). Returns the number of strips initialized. */
int initializeParts (UiContext&, const state::Scope&);
/** BUS n / MAIN n / MATRIX n FEED CONFIGURATION. */
std::unique_ptr<home::OverlayScreen> makeFeedConfigScreen (UiContext&, Strip&);
/** RTA SETTINGS (OVERVIEW, FILTER) or EQ/RTA SETTINGS (EQ). */
std::unique_ptr<home::OverlayScreen> makeSettingsScreen (UiContext&, Strip&, bool eqSettings);

} // namespace livedaw::ui
