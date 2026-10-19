#pragma once

#include "ui/HomeWidgets.h"
#include "ui/Knobs.h"
#include "ui/UiContext.h"

#include <memory>
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

    /** Opens the Feed Configuration of this bus, main or matrix (set by the page). */
    std::function<void()> openFeedConfig;

    void paint (juce::Graphics&) override;

protected:
    template <typename T, typename... Args>
    T& make (Args&&... args)
    {
        auto* c = new T (std::forward<Args> (args)...);
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

    /** Adds a panel (drawn in paint) and returns its content area. */
    juce::Rectangle<int> box (juce::Rectangle<int> area, const juce::String& title, bool chevron = false);
    void clearBoxes() { boxes.clear(); }

    /** The LEVEL panel that stays at the right on OVERVIEW, ICON/COLOR, NAME
        and TAGS: add it in the constructor, lay it out first in resized()
        (it takes 22 % of the width; the rest is returned). */
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
/** BUS n / MAIN n / MATRIX n FEED CONFIGURATION. */
std::unique_ptr<home::OverlayScreen> makeFeedConfigScreen (UiContext&, Strip&);
/** RTA SETTINGS (OVERVIEW, FILTER) or EQ/RTA SETTINGS (EQ). */
std::unique_ptr<home::OverlayScreen> makeSettingsScreen (UiContext&, Strip&, bool eqSettings);

} // namespace livedaw::ui
