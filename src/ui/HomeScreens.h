#pragma once

#include "ui/Knobs.h"
#include "ui/UiContext.h"

#include <memory>
#include <vector>

namespace livedaw::ui
{

/** Base of the HOME screens of a strip: helpers to create bound controls and
    to lay out and draw WING panels. */
class HomeScreen : public juce::Component
{
public:
    HomeScreen (UiContext& c, Strip& s) : ctx (c), strip (s) {}

    /** The six knobs under the display for this screen. */
    virtual std::vector<KnobBinding> knobs() { return {}; }
    std::function<void()> knobsChanged;

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

    /** Adds a panel (drawn in paint) and returns its content area. */
    juce::Rectangle<int> box (juce::Rectangle<int> area, const juce::String& title, bool chevron = false);
    void clearBoxes() { boxes.clear(); }

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

/** "CH 1 Kick", ... for every channel (sidechain sources, matrix direct inputs). */
juce::StringArray channelNames (MixerEngine&);

/** The threshold-like parameter of a dynamics slot ("thr", else the first). */
KnobBinding thresholdBinding (DynSlotParams&, const juce::String& label);

/** A row of keys editing the bits of an atomic mask (DCAs, mute groups). */
class MaskButtons final : public juce::Component, private juce::Timer
{
public:
    MaskButtons (std::atomic<uint32_t>& mask, int count, const juce::String& prefix, juce::Colour on);
    void resized() override;

private:
    void timerCallback() override;
    std::atomic<uint32_t>& mask;
    juce::OwnedArray<juce::TextButton> buttons;
};

// The block screens.
std::unique_ptr<HomeScreen> makeInputScreen (UiContext&, Strip&);
std::unique_ptr<HomeScreen> makeFilterScreen (UiContext&, Strip&);
std::unique_ptr<HomeScreen> makeDynamicsScreen (UiContext&, Strip&, bool gateSlot);
std::unique_ptr<HomeScreen> makeEqScreen (UiContext&, Strip&);
std::unique_ptr<HomeScreen> makeInsertScreen (UiContext&, Strip&, bool postInsert);
std::unique_ptr<HomeScreen> makeMainScreen (UiContext&, Strip&);
std::unique_ptr<HomeScreen> makeSendsScreen (UiContext&, Strip&);

} // namespace livedaw::ui
