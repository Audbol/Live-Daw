#pragma once

#include "engine/Models.h"
#include "ui/LookAndFeel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
#include <vector>

namespace livedaw::ui
{

/** How a model's front panel looks: the console draws each processing model and
    FX device as the hardware unit it recreates (a cream LARC remote for the
    480L reverbs, a grey SSL channel, a black 1176 with its VU meter...). */
struct FaceplateStyle
{
    enum class Knob
    {
        Chrome,  // knurled silver
        Black,   // black with a white line
        Skirt,   // black skirt with a coloured cap (SSL, Neve)
        Pedal,   // large black pedal knob
        White,   // white / cream
        Cap      // coloured knob (cap colour)
    };
    enum class Meter
    {
        None,
        Vu,  // needle meter showing gain reduction
        Led  // LED column
    };
    enum class Layout
    {
        Knobs,   // rows of knobs, switches and selectors
        Faders   // vertical faders (graphic EQs, LARC remotes, EMT 250)
    };

    juce::Colour panel { 0xff2b2b2b }, text { 0xffe8e8e8 }, accent { 0xfff5e14a }, cap { 0xff4a90d9 };
    Knob knob = Knob::Black;
    Meter meter = Meter::None;
    Layout layout = Layout::Knobs;
    juce::String title;       // printed on the panel
    float brushed = 0.0f;     // brushed-metal streaks (0..1)
    bool rackEars = true;
    bool wood = false;        // wooden end cheeks
    bool display = false;     // a display strip showing the first values (digital units)
    juce::Colour lcd { 0xff0d1a10 }, lcdText { 0xff7cff9a };
};

/** The style of a model of a table ("HALL" in the FX table, "LA" in the dynamics table...). */
FaceplateStyle faceplateStyle (models::Table, const juce::String& token);

/** A model's controls on its front panel. Knobs drag vertically (shift for fine
    steps), the mouse wheel steps, double-click resets; switches and selectors
    click. Bound to the model's parameter atomics; repaints when they change. */
class Faceplate final : public juce::Component, private juce::Timer
{
public:
    Faceplate();

    /** `values` must stay valid while bound (the engine's parameter block). */
    void bind (models::Table, int model, std::atomic<float>* values, std::function<float()> meter = {});
    void unbind();

    /** A control was touched (index into the model's parameters). */
    std::function<void (int param)> onTouched;
    int getLastTouched() const noexcept { return lastTouched; }

    /** Draws only the panel (no controls): for small thumbnails. */
    static void paintThumbnail (juce::Graphics&, juce::Rectangle<float>, models::Table, int model, const juce::String& caption);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    enum class Kind
    {
        Knob,
        Fader,
        Switch,
        Selector
    };

    struct Control
    {
        int param = 0;
        Kind kind = Kind::Knob;
        juce::Rectangle<float> area;
    };

    void timerCallback() override;
    void layoutControls();
    int controlAt (juce::Point<float>) const;
    float value (int param) const;
    void setValue (int param, float v);
    void touch (int param);

    void paintControl (juce::Graphics&, const Control&) const;
    void paintKnob (juce::Graphics&, juce::Rectangle<float>, float pos, bool bipolar) const;

    models::Table table = models::Fx;
    int modelIndex = -1;
    const models::Model* spec = nullptr;
    std::atomic<float>* values = nullptr;
    std::function<float()> meterFn;
    FaceplateStyle style;

    std::vector<Control> controls;
    juce::Rectangle<float> panelArea, displayArea, meterArea, titleArea;
    std::vector<float> shown;
    float shownMeter = 0.0f;
    int dragging = -1, lastTouched = -1;
    float dragStartPos = 0.0f;
};

} // namespace livedaw::ui
