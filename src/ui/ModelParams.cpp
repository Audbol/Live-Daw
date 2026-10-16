#include "ui/ModelParams.h"

namespace livedaw::ui
{

static const char* categoryName (models::Category c)
{
    switch (c)
    {
        case models::Category::Gate:       return "GATES";
        case models::Category::Compressor: return "COMPRESSORS";
        case models::Category::Special:    return "SPECIAL";
        case models::Category::Eq:         return "EQUALIZERS";
        case models::Category::Filter:     return "FILTERS";
        case models::Category::FxNone:     return "";
        case models::Category::FxExternal: return "EXTERNAL";
        case models::Category::FxPremium:  return "PREMIUM FX";
        case models::Category::FxStandard: return "STANDARD FX";
        case models::Category::FxChannel:  return "CHANNEL";
    }
    return "";
}

//==============================================================================
ModelParamControl::ModelParamControl (std::atomic<float>& v, const models::Param& p) : value (v), param (p)
{
    if (param.kind == models::ParamKind::Toggle)
    {
        toggle = std::make_unique<juce::TextButton> (param.label);
        toggle->setClickingTogglesState (true);
        toggle->setColour (juce::TextButton::buttonOnColourId, Colours::block);
        toggle->onClick = [this] { value.store (toggle->getToggleState() ? 1.0f : 0.0f); };
        addAndMakeVisible (*toggle);
    }
    else if (param.kind == models::ParamKind::Choice && param.choices.size() <= 12)
    {
        choice = std::make_unique<juce::ComboBox>();
        for (int i = 0; i < param.choices.size(); ++i)
            choice->addItem (param.choices[i], i + 1);
        choice->onChange = [this]
        {
            if (choice->getSelectedId() > 0)
                value.store ((float) (choice->getSelectedId() - 1));
        };
        addAndMakeVisible (*choice);
    }
    else
    {
        knob = std::make_unique<DisplayKnob>();
        knob->setCompact (true);
        knob->setBinding (bindModelParam (value, param));
        addAndMakeVisible (*knob);
    }

    timerCallback();
    startTimerHz (10);
}

void ModelParamControl::timerCallback()
{
    if (toggle != nullptr && toggle->getToggleState() != (value.load() >= 0.5f))
        toggle->setToggleState (value.load() >= 0.5f, juce::dontSendNotification);

    if (choice != nullptr && ! choice->isPopupActive())
    {
        const int id = juce::roundToInt (value.load()) + 1;
        if (choice->getSelectedId() != id)
            choice->setSelectedId (id, juce::dontSendNotification);
    }
}

void ModelParamControl::paint (juce::Graphics& g)
{
    if (knob != nullptr)
        return;

    g.setColour (Colours::accent);
    g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
    g.drawFittedText (choice != nullptr ? param.label : juce::String(), getLocalBounds().removeFromTop (14), juce::Justification::centred, 1);
}

void ModelParamControl::resized()
{
    auto b = getLocalBounds();
    if (knob != nullptr)
        knob->setBounds (b);
    if (toggle != nullptr)
        toggle->setBounds (b.withSizeKeepingCentre (b.getWidth() - 8, juce::jmin (28, b.getHeight())));
    if (choice != nullptr)
    {
        b.removeFromTop (16);
        choice->setBounds (b.removeFromTop (24).reduced (2, 0));
    }
}

//==============================================================================
ModelParamGrid::ModelParamGrid()
{
    startTimerHz (8);
}

void ModelParamGrid::bind (models::Table t, std::atomic<int32_t>* m, std::atomic<float>* v)
{
    table = t;
    model = m;
    values = v;
    builtModel = -2;
    rebuild();
}

void ModelParamGrid::timerCallback()
{
    if (model != nullptr && model->load() != builtModel)
    {
        rebuild();
        if (onModelChanged)
            onModelChanged();
    }
}

void ModelParamGrid::rebuild()
{
    controls.clear();
    builtModel = model != nullptr ? (int) model->load() : -1;

    if (model != nullptr && values != nullptr)
    {
        const auto& spec = models::model (table, builtModel);
        for (size_t i = 0; i < spec.params.size(); ++i)
        {
            auto& c = controls.emplace_back (std::make_unique<ModelParamControl> (values[i], spec.params[i]));
            addAndMakeVisible (*c);
        }
    }

    resized();
    repaint();
}

std::vector<KnobBinding> ModelParamGrid::bindings (int n) const
{
    std::vector<KnobBinding> out;
    if (model == nullptr || values == nullptr)
        return out;

    const auto& spec = models::model (table, (int) model->load());
    for (size_t i = 0; i < spec.params.size() && (int) out.size() < n; ++i)
        out.push_back (bindModelParam (values[i], spec.params[i]));
    return out;
}

void ModelParamGrid::paint (juce::Graphics& g)
{
    if (controls.empty())
    {
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText ("No parameters", getLocalBounds(), juce::Justification::centred);
    }
}

void ModelParamGrid::resized()
{
    if (controls.empty())
        return;

    auto b = getLocalBounds();
    const int n = (int) controls.size();
    const int cellW = 78, cellH = 82;
    const int columns = juce::jlimit (1, n, b.getWidth() / cellW);
    const int rows = (n + columns - 1) / columns;
    const int h = juce::jmin (cellH + 10, b.getHeight() / juce::jmax (1, rows));
    const int w = b.getWidth() / columns;

    for (int i = 0; i < n; ++i)
        controls[(size_t) i]->setBounds (b.getX() + (i % columns) * w, b.getY() + (i / columns) * h, w, h);
}

//==============================================================================
ModelSelector::ModelSelector()
{
    onChange = [this]
    {
        const int id = getSelectedId();
        if (id > 0 && model != nullptr && id - 1 != model->load() && select)
            select (id - 1);
    };
    startTimerHz (8);
}

void ModelSelector::bind (models::Table t, std::atomic<int32_t>* m, std::function<void (int)> onSelect,
                          const juce::StringArray& allowed, const std::vector<Group>& groups)
{
    model = m;
    select = std::move (onSelect);
    clear (juce::dontSendNotification);

    const auto& all = models::table (t);
    auto ok = [&allowed] (const models::Model& spec) { return allowed.isEmpty() || allowed.contains (spec.token); };

    if (! groups.empty())
    {
        juce::StringArray shown;
        for (auto& group : groups)
        {
            bool heading = false;
            for (auto& token : group.tokens)
            {
                const int i = models::findModel (t, token);
                if (i < 0 || shown.contains (token) || ! ok (all[(size_t) i]))
                    continue;
                if (! heading)
                {
                    addSectionHeading (group.heading);
                    heading = true;
                }
                addItem (all[(size_t) i].name.toUpperCase(), i + 1);
                shown.add (token);
            }
        }
        bool heading = false;
        for (auto& spec : all)
        {
            if (shown.contains (spec.token) || ! ok (spec))
                continue;
            if (! heading)
            {
                addSectionHeading ("MORE");
                heading = true;
            }
            addItem (spec.name.toUpperCase(), spec.index + 1);
        }
    }
    else
    {
        int lastCategory = -1;
        for (auto& spec : all)
        {
            if (! ok (spec))
                continue;
            if ((int) spec.category != lastCategory)
            {
                addSectionHeading (categoryName (spec.category));
                lastCategory = (int) spec.category;
            }
            addItem (spec.name.toUpperCase(), spec.index + 1);
        }
    }

    setEnabled (model != nullptr);
    timerCallback();
}

namespace
{
    // As the console's documentation lists the gate-slot and compressor-slot models.
    const juce::StringArray gateModels { "GATE", "DUCK", "9000G", "E88", "D241G", "DS902", "76LA", "LA", "PSE", "WAVE", "RIDE", "WARM", "DEQ" };
    const juce::StringArray compModels { "COMP", "EXP",  "B160", "B560", "D241C", "RED3", "9000C", "SBUS", "ECL33",
                                         "BLISS", "76LA", "LA",   "F670", "NSTR",  "2250", "L100",  "WAVE", "RIDE" };
} // namespace

std::vector<ModelSelector::Group> ModelSelector::gateSlotGroups()
{
    return { { "GATE MODELS", gateModels }, { "COMPRESSOR MODELS", compModels } };
}

std::vector<ModelSelector::Group> ModelSelector::dynamicsSlotGroups()
{
    return { { "COMPRESSOR MODELS", compModels }, { "GATE MODELS", gateModels } };
}

void ModelSelector::timerCallback()
{
    if (model != nullptr && ! isPopupActive() && getSelectedId() != model->load() + 1)
        setSelectedId (model->load() + 1, juce::dontSendNotification);
}

} // namespace livedaw::ui
