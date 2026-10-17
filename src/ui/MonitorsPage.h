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

/** MONITORS (manual chapter 7, fig. 72), opened by SETUP's MONITORS key and the
    MONITORING/TALKBACK VIEW key:
      - the toolbar: TALK A / TALK B (the talk keys, with their SWITCH MODE),
        PHONES / SPEAKERS / PH+SPK (the solo bus and the path DIM, MUTE, MONO
        and LR SWAP act on), DIM, MUTE, MONO, LR SWAP;
      - TALK A / TALK B: SWITCH MODE, the talkback channel, AUTO DIM (BUS DIM,
        MON DIM), USE SEND LEVELS and the TALK TO BUS / MATRIX / MAIN grids;
      - PHONES / SPEAKERS (monitor paths B and A): SOURCE, SRC LEVEL, SRC MIX,
        DIM, PFL DIM, the outputs and PFL / AFL;
      - LEVEL: the shown path's meter and level (set by PHONES / MONITOR LEVEL).
    DIRECT IN, BAND SOLO, the EQ, INVERT, DELAY and the limiter aren't in the
    engine and are drawn disabled. */
class MonitorsPage final : public DisplayPage, private juce::Timer
{
public:
    explicit MonitorsPage (UiContext&);
    ~MonitorsPage() override;

    std::vector<KnobBinding> getKnobs() override;
    StatusTitle getStatusTitle() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

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
    void paintLevel (juce::Graphics&);
    juce::String outputName (int port) const;
    juce::String talkChannelName() const;
    void chooseOutput (bool right);
    void chooseSource();
    void chooseTalkChannel();
    void showTalkTab (int tab);
    void showPathTab (int tab);

    // The path shown: tab 0 = PHONES (path B), 1 = SPEAKERS (path A).
    bool showsPhones() const noexcept { return pathTab == 0; }
    ld::MonitorSourceParams& pathSource() const noexcept;
    std::atomic<float>& pathLevel() const noexcept;
    std::atomic<float>& pathDim() const noexcept;
    std::atomic<int32_t>& pathOutput (bool right) const noexcept;
    ld::TalkPathParams& talkPath() const noexcept;

    UiContext& ctx;
    int talkTab = 0, pathTab = 0, pressedTalk = -1;
    std::vector<ToolKey> tools;
    juce::Rectangle<int> toolbar, talkTabs, talkArea, pathTabs, pathHeader, pathArea, levelHeader, levelArea;
    juce::Rectangle<int> switchBox, autoDimBox, sendLevelsRow, busGrid, mtxGrid, mainGrid, talkChannelField, sourceField, directField, eqKey,
        distSlider, invertKey, delayKey, outputBox, leftField, rightField, soloBox, meterArea, faderArea;
    std::vector<juce::Rectangle<int>> busTiles, mtxTiles, mainTiles;
    KnobField srcLevel, srcMix, dim, pflDim, bandSolo, busDim, monDim, limiter;
    std::unique_ptr<ChoiceKeys> soloKeys, switchMode;
    std::unique_ptr<OnKey> sendLevels;
    juce::Component anchor;
    std::array<float, 2> shownPeak {}, phonesPeak {};
};

} // namespace livedaw::ui
