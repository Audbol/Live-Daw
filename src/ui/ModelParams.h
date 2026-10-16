#pragma once

#include "ui/Knobs.h"

#include <memory>
#include <vector>

namespace livedaw::ui
{

/** A model parameter's caption and value text as the console prints them.
    Mostly the model table's own (models::Param), with the few places where a
    unit's panel says something else: Even 88 Comp RATIO "LIM" and RELEASE
    "AUTO" at the end of their travel, Even 88 Gate HYST "EXP" at 0, PIA 2250
    THR in dB (+10 .. -20), SOUL EQ Q from 2.5 (narrow) to 0.3 (wide), LTA100
    GAIN (an output gain). */
juce::String consoleLabel (const models::Param&);
juce::String consoleText (const models::Param&, float value);

/** bindModelParam with the console's caption and value text. */
KnobBinding bindConsoleParam (std::atomic<float>& value, const models::Param&);

/** One model parameter as a control: a knob (continuous, value lists,
    integers), a toggle, or a choice box. */
class ModelParamControl final : public juce::Component, private juce::Timer
{
public:
    ModelParamControl (std::atomic<float>& value, const models::Param&);

    void paint (juce::Graphics&) override;
    void resized() override;

    KnobBinding binding() const { return bindModelParam (value, param); }

private:
    void timerCallback() override;

    std::atomic<float>& value;
    models::Param param;
    std::unique_ptr<DisplayKnob> knob;
    std::unique_ptr<juce::TextButton> toggle;
    std::unique_ptr<juce::ComboBox> choice;
};

/** The parameters of the model currently loaded in a block (gate, dynamics,
    EQ, tool filter); rebuilds itself when the model changes. */
class ModelParamGrid final : public juce::Component, private juce::Timer
{
public:
    ModelParamGrid();

    /** `values` points at the block's parameter array (nullptr unbinds). */
    void bind (models::Table table, std::atomic<int32_t>* model, std::atomic<float>* values);

    /** Knob bindings for the first `n` parameters (for the knobs under the display). */
    std::vector<KnobBinding> bindings (int n) const;

    int getNumParams() const noexcept { return (int) controls.size(); }

    void resized() override;
    void paint (juce::Graphics&) override;

    std::function<void()> onModelChanged;

private:
    void timerCallback() override;
    void rebuild();

    models::Table table = models::Dynamics;
    std::atomic<int32_t>* model = nullptr;
    std::atomic<float>* values = nullptr;
    int builtModel = -1;
    std::vector<std::unique_ptr<ModelParamControl>> controls;
};

/** Model chooser for a block, grouped by category. Choosing a model calls
    `onSelect` (which loads the model's defaults through the engine). */
class ModelSelector final : public juce::ComboBox, private juce::Timer
{
public:
    ModelSelector();

    /** A section of the menu: a heading and its models (tokens) in order. */
    struct Group
    {
        juce::String heading;
        juce::StringArray tokens;
    };

    /** `allowed` (tokens) restricts the list, e.g. the aux channels' dynamics models.
        With `groups`, the menu lists those sections in order (a model appears once),
        then the remaining models under MORE. */
    void bind (models::Table, std::atomic<int32_t>* model, std::function<void (int)> onSelect,
               const juce::StringArray& allowed = {}, const std::vector<Group>& groups = {});

    /** The console's gate-slot and compressor-slot model lists. */
    static std::vector<Group> gateSlotGroups();
    static std::vector<Group> dynamicsSlotGroups();

private:
    void timerCallback() override;
    std::atomic<int32_t>* model = nullptr;
    std::function<void (int)> select;
};

} // namespace livedaw::ui
