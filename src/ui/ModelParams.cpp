#include "ui/ModelParams.h"
#include "ui/DynamicsGraph.h"
#include "ui/Faceplate.h"
#include "ui/WingPopups.h"

namespace livedaw::ui
{

namespace
{
    juce::String tokenOf (const models::Param& p) { return models::model (static_cast<models::Table> (p.table), p.model).token; }

    bool atEnd (const models::Param& p, float v, bool top)
    {
        const float end = top ? p.max : p.min;
        return std::abs (v - end) <= 1.0e-3f * std::abs (p.max - p.min);
    }
} // namespace

juce::String consoleLabel (const models::Param& p)
{
    if (p.table == models::Dynamics && p.id == "ingain" && tokenOf (p) == "L100")
        return "OUT GAIN";
    return p.label;
}

juce::String consoleText (const models::Param& p, float v)
{
    const bool dyn = p.table == models::Dynamics;
    const bool eq = p.table == models::ChannelEq || p.table == models::BusEq;
    // The TILT EQ reads to a hundredth of a dB ("0.00 dB", 3.1).
    if (p.table == models::FilterTools && p.id == "tilt")
        return juce::String (v, 2) + " dB";
    // A RATIO picked from a list reads "3:1" (an expander's "1:3"), "2.5:1" or
    // "1.43:1": the list's value without trailing zeros, as the console's
    // parameter bar shows the WING COMPRESSOR's 3.0 and the WING GATE's 1:3
    // (3.1 screenshots); the list's special entries (B560's negative ratios
    // and INF) keep their own text.
    if (p.kind == models::ParamKind::Values && p.id.endsWith ("ratio"))
        if (const auto shown = p.display (v); shown.containsOnly ("0123456789.") && shown.getFloatValue() > 0.0f)
        {
            const float r = shown.getFloatValue();
            const bool hundredths = std::abs (r * 10.0f - std::round (r * 10.0f)) > 1.0e-3f;
            const bool tenths = std::abs (r - std::round (r)) > 1.0e-3f;
            const auto text = juce::String (r, hundredths ? 2 : (tenths ? 1 : 0));
            return tokenOf (p) == "EXP" ? "1:" + text : text + ":1";
        }
    if (! dyn && ! eq)
        return p.display (v);
    const auto token = tokenOf (p);
    if (dyn && token == "E88C" && p.id == "ratio" && atEnd (p, v, true))
        return "LIM";
    if (dyn && token == "E88C" && p.id == "rel" && atEnd (p, v, true))
        return "AUTO";
    if (dyn && token == "E88" && p.id == "hyst" && atEnd (p, v, false))
        return "EXP";
    if (dyn && token == "2250" && p.id == "thr" && p.unit.isEmpty())
    {
        // The knob's 0..10 run from +10 dB down to -20 dB.
        const float db = 10.0f - 30.0f * (v - p.min) / juce::jmax (1.0e-6f, p.max - p.min);
        return (db > 0.05f ? "+" : "") + juce::String (db, 1) + " dB";
    }
    // Percentages in whole steps read without decimals (the WING GATE's ACCENT: "0 %").
    if (p.unit == "%" && p.isContinuous() && p.steps > 1)
        if (const float step = (p.max - p.min) / (float) (p.steps - 1); step >= 1.0f && std::abs (step - std::round (step)) < 1.0e-3f)
            return juce::String (juce::roundToInt (v)) + " %";
    if (eq && token == "SOUL" && (p.id == "lmq" || p.id == "hmq"))
    {
        // Narrow (2.5) to wide (0.3) clockwise.
        const float t = juce::jlimit (0.0f, 1.0f, (v - p.min) / juce::jmax (1.0e-6f, p.max - p.min));
        return juce::String (2.5f * std::pow (0.3f / 2.5f, t), 2);
    }
    return p.display (v);
}

KnobBinding bindConsoleParam (std::atomic<float>& v, const models::Param& p)
{
    auto b = bindModelParam (v, p);
    b.label = consoleLabel (p);
    b.text = [&v, p] { return consoleText (p, v.load()); };
    return b;
}

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
        toggle = std::make_unique<juce::TextButton> (consoleLabel (param));
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
        knob->setBinding (bindConsoleParam (value, param));
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
    g.drawFittedText (choice != nullptr ? consoleLabel (param) : juce::String(), getLocalBounds().removeFromTop (14), juce::Justification::centred, 1);
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
        out.push_back (bindConsoleParam (values[i], spec.params[i]));
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

std::vector<ModelSelector::Row> ModelSelector::rows (models::Table t, const juce::StringArray& allowed, const std::vector<Group>& groups)
{
    std::vector<Row> out;
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
                    out.push_back ({ group.heading, -1 });
                    heading = true;
                }
                out.push_back ({ all[(size_t) i].name.toUpperCase(), i });
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
                out.push_back ({ "MORE", -1 });
                heading = true;
            }
            out.push_back ({ spec.name.toUpperCase(), spec.index });
        }
        return out;
    }

    int lastCategory = -1;
    for (auto& spec : all)
    {
        if (! ok (spec))
            continue;
        if ((int) spec.category != lastCategory)
        {
            if (const juce::String heading = categoryName (spec.category); heading.isNotEmpty())
                out.push_back ({ heading, -1 });
            lastCategory = (int) spec.category;
        }
        out.push_back ({ spec.name.toUpperCase(), spec.index });
    }
    return out;
}

void ModelSelector::choose (juce::Component& anchor, models::Table t, int current, std::function<void (int)> onSelect,
                            const juce::StringArray& allowed, const std::vector<Group>& groups)
{
    const auto list = rows (t, allowed, groups);
    juce::StringArray items;
    popups::ListOptions options;
    int selected = -1;
    for (int i = 0; i < (int) list.size(); ++i)
    {
        const auto& row = list[(size_t) i];
        items.add (row.model < 0 ? "- " + row.text + " -" : row.text);
        if (row.model < 0)
            options.disabled.push_back (i);
        if (row.model == current)
            selected = i;
    }
    options.columns = items.size() > 24 ? 2 : 1;
    popups::chooseFrom (anchor, items, selected, [list, onSelect] (int i)
    {
        if (i >= 0 && i < (int) list.size() && list[(size_t) i].model >= 0 && onSelect)
            onSelect (list[(size_t) i].model);
    }, options);
}

void ModelSelector::chooseDynamics (juce::Component& anchor, const DynSlotParams& slot, std::function<void (int)> onSelect,
                                    const juce::StringArray& allowed)
{
    // The 3.1 model list, row by row (gates, special processors | compressors).
    static const std::vector<juce::StringArray> layout {
        { "GATE", "DUCK", "E88", "D241G", "9000G" },
        { "DEQ", "DEQ2", "DS902", "WAVE", "RIDE", "WARM" },
        { "COMP", "EXP", "E88C", "ONEC", "9000C" },
        { "B160", "B560", "RED3", "LMT", "SBUS", "2250" },
        { "76LA", "LA", "F670", "L100", "BLISS", "NSTR" },
        { "PSE", "CMB", "ECL33", "D241C" },
    };
    std::vector<std::vector<popups::Tile>> rows;
    juce::StringArray placed;
    for (auto& tokens : layout)
    {
        auto& row = rows.emplace_back();
        for (auto& token : tokens)
        {
            const int m = models::findModel (models::Dynamics, token);
            placed.add (token);
            if (m < 0 || (allowed.size() > 0 && ! allowed.contains (token)))
                row.push_back ({});
            else
                row.push_back ({ m, models::model (models::Dynamics, m).name.toUpperCase() });
        }
    }
    // Anything the layout doesn't know yet goes in a last row.
    std::vector<popups::Tile> extra;
    for (auto& spec : models::table (models::Dynamics))
        if (! placed.contains (spec.token) && (allowed.isEmpty() || allowed.contains (spec.token)))
            extra.push_back ({ spec.index, spec.name.toUpperCase() });
    if (! extra.empty())
        rows.push_back (extra);
    // Every tile is a fixed picture of its model at the factory values, the
    // loaded model's too: the list doesn't follow the strip's settings.
    popups::chooseTile (anchor, rows, { 2 }, slot.model.load(),
                        [] (juce::Graphics& g, juce::Rectangle<float> r, int m) { paintDynamicsModelTile (g, r, m); },
                        std::move (onSelect));
}

std::vector<float> ModelSelector::modelTileValues (int m)
{
    std::vector<float> v ((size_t) ld::kMaxDynParams, 0.0f);
    const auto& spec = models::model (models::Dynamics, m);
    for (size_t i = 0; i < spec.params.size() && i < v.size(); ++i)
        v[i] = spec.params[i].defaultValue;
    return v;
}

void ModelSelector::paintDynamicsModelTile (juce::Graphics& g, juce::Rectangle<float> r, int m)
{
    if (hasTransferGraph (m))
    {
        const auto v = modelTileValues (m);
        paintDynamicsTile (g, r, m, v.data(), true); // orange whether the slot is on or not [3.1]
    }
    else
        Faceplate::paintThumbnail (g, r, models::Dynamics, m, {});
}

void ModelSelector::chooseEq (juce::Component& anchor, models::Table t, int current, std::function<void (int)> onSelect,
                              const juce::StringArray& allowed)
{
    // The 3.1 list: the WING EQ alone in the first row, the analog models three to a row.
    std::vector<std::vector<popups::Tile>> rows;
    for (auto& spec : models::table (t))
    {
        if (allowed.size() > 0 && ! allowed.contains (spec.token))
            continue;
        if (rows.empty() || rows.size() == 1 || rows.back().size() == 3)
            rows.emplace_back();
        rows.back().push_back ({ spec.index, spec.name.toUpperCase() });
    }
    popups::chooseTile (anchor, rows, {}, current,
                        [t] (juce::Graphics& g, juce::Rectangle<float> r, int m)
                        {
                            if (models::model (t, m).token == "STD")
                                paintWingEqTile (g, r);
                            else
                                Faceplate::paintThumbnail (g, r, t, m, {});
                        },
                        std::move (onSelect));
}

void ModelSelector::paintWingEqTile (juce::Graphics& g, juce::Rectangle<float> r)
{
    // A dark grid with an amber curve through six band rings, as the 3.1 model list draws it.
    g.setColour (juce::Colour (0xff000400));
    g.fillRect (r);
    g.setColour (juce::Colour (0xff283038));
    for (int i = 1; i < 8; ++i)
        g.fillRect (r.getX() + r.getWidth() * (float) i / 8.0f, r.getY(), 1.0f, r.getHeight());
    g.fillRect (r.getX(), r.getCentreY(), r.getWidth(), 1.0f);
    // Bands as (position 0..1, gain -1..1, width).
    static const float bands[][3] = { { 0.08f, 0.25f, 0.18f }, { 0.28f, 0.7f, 0.1f }, { 0.48f, -0.25f, 0.1f },
                                      { 0.65f, 0.15f, 0.08f }, { 0.8f, -0.3f, 0.08f }, { 0.94f, 0.35f, 0.12f } };
    auto gainAt = [] (float x)
    {
        float sum = 0.0f;
        for (auto& b : bands)
            sum += b[1] * std::exp (-std::pow ((x - b[0]) / b[2], 2.0f));
        return sum;
    };
    auto pointAt = [&r] (float x, float gain) { return juce::Point<float> (r.getX() + x * r.getWidth(), r.getCentreY() - gain * r.getHeight() * 0.32f); };
    juce::Path curve, fill;
    for (int i = 0; i <= 64; ++i)
    {
        const float x = (float) i / 64.0f;
        const auto pt = pointAt (x, gainAt (x));
        if (i == 0)
            curve.startNewSubPath (pt);
        else
            curve.lineTo (pt);
    }
    fill = curve;
    fill.lineTo (r.getRight(), r.getCentreY());
    fill.lineTo (r.getX(), r.getCentreY());
    fill.closeSubPath();
    g.setColour (juce::Colour (0xff38280f));
    g.fillPath (fill);
    g.setColour (juce::Colour (0xfff0a830));
    g.strokePath (curve, juce::PathStrokeType (juce::jmax (1.0f, r.getHeight() * 0.03f)));
    const float d = juce::jmax (3.0f, r.getHeight() * 0.11f);
    g.setColour (juce::Colour (0xffc0c0c0));
    for (auto& b : bands)
        g.drawEllipse (juce::Rectangle<float> (d, d).withCentre (pointAt (b[0], gainAt (b[0]))), juce::jmax (1.0f, d * 0.15f));
}

void ModelSelector::bind (models::Table t, std::atomic<int32_t>* m, std::function<void (int)> onSelect,
                          const juce::StringArray& allowed, const std::vector<Group>& groups)
{
    model = m;
    select = std::move (onSelect);
    clear (juce::dontSendNotification);
    for (auto& row : rows (t, allowed, groups))
    {
        if (row.model < 0)
            addSectionHeading (row.text);
        else
            addItem (row.text, row.model + 1);
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
