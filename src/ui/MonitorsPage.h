#pragma once

#include "ui/DisplayPage.h"
#include "ui/ScreenWidgets.h"

#include <memory>

namespace livedaw::ui
{

/** A WING knob box: a ring at the left, a small caption over a big value, on a
    dark well. Drag up / down or turn the wheel; double-click resets. Drawn
    dim (and inert) without a binding. */
class KnobField final : public juce::Component, private juce::Timer
{
public:
    explicit KnobField (KnobBinding b = {}, juce::Colour ringColour = Dark::text);

    void setBinding (KnobBinding);
    const KnobBinding& getBinding() const noexcept { return binding; }
    /** The label to show while unbound (the console's control that isn't here yet). */
    juce::String placeholder, placeholderValue;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void timerCallback() override;

    KnobBinding binding;
    juce::Colour ring;
    float dragStart = 0.0f;
    juce::String shown;
};

class MonitorEqView;

/** MONITORS (manual chapter 7, figs. 72-73), opened by SETUP's MONITORS key and
    the MONITORING/TALKBACK VIEW key:
      - the toolbar: TALK A / TALK B (the talk keys, with their SWITCH MODE),
        PHONES / SPEAKERS / PH+SPK (the solo bus and the path DIM, MUTE, MONO
        and LR SWAP act on), DIM, MUTE, MONO, LR SWAP;
      - TALK A / TALK B: SWITCH MODE, the talkback channel, AUTO DIM (BUS DIM,
        MON DIM), USE SEND LEVELS and the TALK TO BUS / MATRIX / MAIN grids;
      - PHONES / SPEAKERS SETTINGS (each path its own): SOURCE, SRC LEVEL, SRC
        MIX, DIM, PFL DIM, BAND SOLO, DIRECT IN (its level on the parameter
        bar), the EQ key (opening MONITORS EQ, fig. 73: 8 bands, PAN, WIDTH),
        the DIST slider with INVERT and DELAY;
      - LEVEL: LIMITER, the path's meter (with the limiter's gain reduction)
        and its level (set by PHONES / MONITOR LEVEL).
    The paths' outputs are patched on ROUTING > OUTPUTS (source group MONITOR)
    and PFL / AFL chosen on SETUP > AUDIO, as on the console. */
class MonitorsPage final : public DisplayPage, private juce::Timer
{
public:
    explicit MonitorsPage (UiContext&);
    ~MonitorsPage() override;

    std::vector<KnobBinding> getKnobs() override;
    std::vector<KnobStyle> getKnobStyles() override;
    StatusTitle getStatusTitle() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    /** MONITORS EQ (fig. 73) of the path shown, over the page. */
    void showEq (bool show);
    bool isEqShown() const noexcept;

private:
    struct ToolKey
    {
        juce::String text;
        Icon icon;
        juce::Colour colour;
        std::function<bool()> lit;
        std::function<void()> press;
        juce::Rectangle<int> area;
        int talkPath = -1; // TALK A / TALK B: press and release go to the engine
    };

    void timerCallback() override;
    void paintToolbar (juce::Graphics&);
    void paintTalk (juce::Graphics&);
    void paintPath (juce::Graphics&);
    void paintEqKey (juce::Graphics&);
    void paintDistSlider (juce::Graphics&);
    void paintLevel (juce::Graphics&);
    juce::String talkChannelName() const;
    void chooseSource();
    void chooseDirectIn();
    void chooseTalkChannel();
    void showTalkTab (int tab);
    void showPathTab (int tab);
    void setDistanceFromY (int y);
    juce::String eqStateKey() const;

    // The path shown: tab 0 = PHONES, 1 = SPEAKERS (as the solo bus choices A, B).
    bool showsPhones() const noexcept { return pathTab == 0; }
    ld::MonitorSourceParams& pathSource() const noexcept;
    ld::MonitorDspParams& pathDsp() const noexcept;
    std::atomic<float>& pathLevel() const noexcept;
    std::atomic<float>& pathDim() const noexcept;
    ld::TalkPathParams& talkPath() const noexcept;

    UiContext& ctx;
    int talkTab = 0, pathTab = 0, pressedTalk = -1;
    bool draggingDist = false;
    std::vector<ToolKey> tools;
    juce::Rectangle<int> toolbar, talkTabs, talkArea, pathTabs, pathHeader, pathArea, levelHeader, levelArea, talkTabKeys[2], pathTabKeys[2];
    juce::Rectangle<int> switchHeader, switchBox, autoDimHeader, autoDimBox, sourceBox, directBox, sendLevelsRow, busGrid, mtxGrid, mainGrid, talkChannelField, sourceField, directField,
        eqKey, dimBox, distBox, distSlider, invertKey, delayKey, meterArea, faderArea;
    std::vector<juce::Rectangle<int>> busTiles, mtxTiles, mainTiles;
    KnobField srcLevel, srcMix, dim, pflDim, bandSolo, busDim, monDim, limiter;
    std::unique_ptr<ChoiceKeys> switchMode;
    std::unique_ptr<OnKey> sendLevels;
    std::unique_ptr<MonitorEqView> eqView;
    juce::Component anchor;
    std::array<float, 2> shownPeak {}, phonesPeak {};
    float limiterGr = 0.0f, shownGr = 0.0f;
    juce::String shownEq;
    std::vector<double> eqKeyFreqs;
};

} // namespace livedaw::ui
