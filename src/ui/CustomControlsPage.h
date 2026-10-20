#pragma once

#include "ui/DisplayPage.h"
#include "ui/ScreenWidgets.h"

#include <array>

/** Custom controls (the WING's CUSTOM CONTROLS section and CC EDIT): the
    assignments saved with the show, what each function does, and the CC EDIT
    screen. */
namespace livedaw::ui::cc
{

/** The upper part's 16 layers (UP / DOWN), then the lower part's USER layer,
    the 4-Channel section's U1-U3 and the lower part's DAW 1-4 layers of DAW
    mode (stored with the show, see livedaw-session/custom.rs). */
constexpr int kNumLayers = 16, kUserLayer = 16, kFirstFourChannelLayer = 17, kNumStoredLayers = 24, kNumControls = 4;
constexpr int kFirstDawLayer = 20, kNumDawLayers = 4;

/** The lower part's layers (USER, DAW 1-4) have keys only. */
inline bool isLowerLayer (int layer) noexcept { return layer == kUserLayer || layer >= kFirstDawLayer; }
/** "LAYER 3", "USER", "DAW 2". */
juce::String layerName (int layer);

/** The encoder of a control, or one of the two keys under it. */
enum class Slot
{
    Encoder,
    Upper,
    Lower
};

struct Function
{
    int category = 0, target = 0, option = 0;

    bool isAssigned() const noexcept { return category != 0; }
    bool operator== (const Function& o) const noexcept { return category == o.category && target == o.target && option == o.option; }
};

struct Control
{
    Function encoder, upper, lower;
    juce::String name;
    int colour = 0; // WING colour 1-18, 0 = none

    Function& slot (Slot s) { return s == Slot::Encoder ? encoder : s == Slot::Upper ? upper : lower; }
    const Function& slot (Slot s) const { return s == Slot::Encoder ? encoder : s == Slot::Upper ? upper : lower; }
};

/** The show's assignments, cached: reload() after a show was loaded; every
    change is written back to the show at once. */
class Assignments
{
public:
    explicit Assignments (Show&);

    void reload();
    const Control& get (int layer, int index) const;
    void set (int layer, int index, const Control&);
    /** CLR ALL. */
    void clearLayer (int layer);
    /** Bumped by every change, so the surface knows to redraw. */
    int getRevision() const noexcept { return revision; }

    /** The upper part's layer (UP / DOWN; CC EDIT edits it too). */
    int currentLayer = 0;

private:
    void save();

    Show& show;
    std::array<std::array<Control, kNumControls>, kNumStoredLayers> layers {};
    int revision = 0;
};

// The function categories (Drew Brashler's sheet of the 3.x functions; the
// ones that need features this console doesn't have are listed disabled).
enum EncoderCategory
{
    EncNone,
    EncFader,
    EncPan,
    EncDcaFader,
    EncSelBusSend,
    EncFixedBusSend,
    EncFxParameter,
    EncMonitoring,
    EncDawVpot,
    EncMidiCc,
    kNumEncoderCategories
};

enum ButtonCategory
{
    BtnNone,
    BtnMute,
    BtnSolo,
    BtnDcaMute,
    BtnMuteGroup,
    BtnSendsOnFader,
    BtnChannelPage,
    BtnGlobalPage,
    BtnFaderPage,
    BtnShowControl,
    BtnSceneRecall,
    BtnOther,
    BtnFaderSpill,
    BtnDawButtons,
    BtnMidi,
    BtnUsbPlayer,
    BtnSdRecorder,
    BtnGpio,
    BtnCcBank, // 3.0.6: the option is the upper part's layer (CC bank) to show
    kNumButtonCategories
};

int numCategories (Slot);
juce::String categoryName (Slot, int category);
/** False for the functions this console can't do yet (drawn disabled). */
bool categoryAvailable (Slot, int category);
/** The FUNCTION list's row colours (bus sends dark red, FX parameters magenta, ...). */
juce::Colour categoryColour (Slot, int category);

/** A strip as a function target: kind * 100000 + id. */
int encodeStrip (const state::StripRef&) noexcept;
state::StripRef decodeStrip (int target) noexcept;

/** The knob binding of an encoder function (invalid when it does nothing). */
KnobBinding bindEncoder (UiContext&, const Function&);
/** A key press. */
void press (UiContext&, const Function&);
/** A key released (the push functions: DAW BUTTONS, MIDI CC / NOTE PUSH). */
void release (UiContext&, const Function&);
/** A key's LED as SurfaceKey::Led (0 off, 1 lit, 2 blinking: DAW LEDs blink). */
int ledState (UiContext&, const Function&);
/** Whether a key's LED is lit. */
bool isLit (UiContext&, const Function&);
/** "CH1 FADER", "SCN GO", "TALK A"... */
juce::String describe (UiContext&, Slot, const Function&);

} // namespace livedaw::ui::cc

namespace livedaw::ui
{

/** CUSTOM CONTROLS (CC EDIT, [D1]): opened by pressing SETUP twice, SETUP's
    CC EDIT key and the CC section's VIEW key. The ‹ › keys pick the layer; the
    LAYER panel maps its four encoders and eight keys (touch one to edit it);
    the FUNCTION list (rows coloured by category) and one or two lists for
    the function's target and option assign it; SETTINGS has COLOR and
    CLR ALL, CONTROL has NAME (a name starting with '*' shows the value, a
    word written "|word" is shown inverted on the LCD). */
class CustomControlsPage final : public DisplayPage, private juce::Timer
{
public:
    CustomControlsPage (UiContext&, cc::Assignments&, int layer = -1);

    bool usesKnobs() const override { return false; }
    StatusTitle getStatusTitle() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    /** A list column: its header, rows and the ▲ ▼ keys under it. */
    struct ListColumn
    {
        juce::String title;
        juce::StringArray rows;
        std::vector<juce::Colour> bars;     // a colour bar at the row's left (channels)
        std::vector<juce::Colour> fills;    // the row's background (FUNCTION)
        std::vector<bool> enabled;
        int selected = -1, scroll = 0;
        juce::Rectangle<int> header, body, upKey, downKey;
        std::vector<juce::Rectangle<int>> rowAreas;
    };

    void timerCallback() override;
    void rebuildLists();
    /** Scrolls each list to its selected row (touching a control). */
    void revealSelection();
    void layoutList (ListColumn&, juce::Rectangle<int> area);
    void paintList (juce::Graphics&, ListColumn&, bool focused);
    void paintLayer (juce::Graphics&);
    void choose (int list, int row);
    void setLayer (int);
    cc::Control control() const;
    cc::Function function() const;
    void store (const cc::Function&);
    /** The targets (list 1) and options (list 2) of the selected function. */
    std::vector<int> targetCodes, optionCodes;
    bool targetIsOption = false; // the first sub-list holds the options (no targets)

    UiContext& ctx;
    cc::Assignments& assignments;
    int layer = 0, index = 0;
    cc::Slot slot = cc::Slot::Encoder;
    int focusedList = 0;
    ListColumn lists[3];
    juce::Rectangle<int> header, prevKey, nextKey, userKey, gpioKey, title, layerHeader, layerBody, settingsHeader, controlHeader, colourKey, clearKey, nameKey,
        colourBox, clearBox, nameBox;
    struct Element
    {
        juce::Rectangle<int> area;
        int index;
        cc::Slot slot;
    };
    std::vector<Element> elements;
    std::vector<juce::Rectangle<int>> colourBars;
    juce::Component anchor;
    int shownRevision = -1;
};

} // namespace livedaw::ui
