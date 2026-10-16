#pragma once

#include "ui/Knobs.h"
#include "ui/MeterComponent.h"
#include "ui/UiContext.h"

namespace livedaw::ui
{

/** A block key of the channel strip section: click opens the block's screen,
    right-click switches the block on or off (lit when on). */
class BlockKey final : public juce::TextButton, private juce::Timer
{
public:
    explicit BlockKey (const juce::String& keyText);

    std::function<bool()> isOn;
    std::function<void()> onToggle, onOpen;

    void mouseUp (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
};

/** The WING's channel strip section (top left): the selected strip's main
    parameters on dedicated knobs (gain, low cut, gate, dynamics, insert, pan)
    and the EQ section (four band knobs, GAIN/WIDTH/FREQ, SHIFT, MIX). */
class ChannelStripSection final : public juce::Component, private juce::Timer
{
public:
    explicit ChannelStripSection (UiContext&);

    /** The selected strip changed. */
    void refresh();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void bindEq();

    UiContext& ctx;
    Strip* strip = nullptr;
    MeterComponent inMeter, outMeter;

    static constexpr int kNumBlocks = 6;
    DisplayKnob blockKnobs[kNumBlocks];
    std::unique_ptr<BlockKey> blockKeys[kNumBlocks];

    DisplayKnob eqKnobs[4], eqMix;
    juce::TextButton gainButton { "GAIN" }, widthButton { "WIDTH" }, freqButton { "FREQ" }, shiftButton { "SHIFT" };
    std::unique_ptr<BlockKey> eqKey;
    int eqElement = 0; // 0 gain, 1 width (Q), 2 freq
    bool eqShift = false;
    int eqModel = -1;

    juce::Rectangle<int> lcdArea, blockArea, eqArea;
    juce::String shownName;
};

/** Top right: the main/solo meter, monitor level with DIM, PFL/AFL and CLR SOLO,
    show control (scene GO/back) and the mute group keys. */
class MonitorSection final : public juce::Component, private juce::Timer
{
public:
    explicit MonitorSection (UiContext&);

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Scenes changed. */
    void refresh();

private:
    void timerCallback() override;

    UiContext& ctx;
    MeterComponent meter;
    DisplayKnob level;
    juce::TextButton dimButton { "DIM" }, aflButton { "AFL" }, clearSoloButton { "CLR SOLO" };
    juce::TextButton prevButton { "<" }, goButton { "GO >" }, scenesButton { "SCENES" };
    juce::OwnedArray<juce::TextButton> muteGroupButtons;
    juce::Rectangle<int> meterArea, sceneArea, mgArea, monitorArea, sceneDisplay;
    float dimRestoreDb = 0.0f;
    bool dimmed = false;
    juce::String sceneText;
};

} // namespace livedaw::ui
