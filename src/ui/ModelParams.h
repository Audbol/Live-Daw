#pragma once

#include "ui/Knobs.h"

#include <memory>
#include <vector>

namespace livedaw::ui
{

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

    /** `allowed` (tokens) restricts the list, e.g. the aux channels' dynamics models. */
    void bind (models::Table, std::atomic<int32_t>* model, std::function<void (int)> onSelect,
               const juce::StringArray& allowed = {});

private:
    void timerCallback() override;
    std::atomic<int32_t>* model = nullptr;
    std::function<void (int)> select;
};

} // namespace livedaw::ui
