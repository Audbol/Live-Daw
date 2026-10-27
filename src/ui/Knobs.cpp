#include "ui/Knobs.h"
#include "ui/ModelParams.h"

namespace livedaw::ui
{

namespace fader
{
    // (position, dB) breakpoints, linear in dB between them.
    static constexpr float points[][2] = { { 0.0f, -90.0f }, { 0.0625f, -60.0f }, { 0.25f, -30.0f },
                                           { 0.5f, -10.0f }, { 0.75f, 0.0f },     { 1.0f, 10.0f } };
    static constexpr int numPoints = (int) (sizeof (points) / sizeof (points[0]));

    float posToDb (float pos) noexcept
    {
        pos = juce::jlimit (0.0f, 1.0f, pos);
        if (pos <= 0.002f)
            return kMinDb;
        for (int i = 1; i < numPoints; ++i)
            if (pos <= points[i][0])
            {
                const float t = (pos - points[i - 1][0]) / (points[i][0] - points[i - 1][0]);
                return points[i - 1][1] + t * (points[i][1] - points[i - 1][1]);
            }
        return kMaxDb;
    }

    float dbToPos (float db) noexcept
    {
        if (db <= points[0][1])
            return db <= kMinDb ? 0.0f : 0.002f;
        for (int i = 1; i < numPoints; ++i)
            if (db <= points[i][1])
            {
                const float t = (db - points[i - 1][1]) / (points[i][1] - points[i - 1][1]);
                return points[i - 1][0] + t * (points[i][0] - points[i - 1][0]);
            }
        return 1.0f;
    }

    juce::String text (float db)
    {
        if (db <= -89.9f)
            return juce::String::fromUTF8 ("-\xe2\x88\x9e");
        return (db > 0.05f ? "+" : "") + juce::String (db, 1);
    }

    juce::NormalisableRange<double> range()
    {
        return { (double) kMinDb, (double) kMaxDb,
                 [] (double, double, double p) { return (double) posToDb ((float) p); },
                 [] (double, double, double v) { return (double) dbToPos ((float) v); },
                 [] (double, double, double v) { return v <= -89.9 ? (double) kMinDb : juce::jlimit ((double) -90, (double) kMaxDb, v); } };
    }
} // namespace fader

juce::String formatHz (float hz)
{
    if (hz >= 10000.0f)
        return juce::String (hz / 1000.0f, 1) + "k";
    if (hz >= 1000.0f)
        return juce::String (hz / 1000.0f, 2) + "k";
    if (hz >= 100.0f)
        return juce::String (juce::roundToInt (hz));
    return juce::String (hz, 1);
}

KnobBinding bindLinear (const juce::String& label, std::atomic<float>& v, float min, float max,
                        const juce::String& suffix, int decimals, float defaultValue)
{
    KnobBinding b;
    b.label = label;
    b.get = [&v, min, max] { return juce::jlimit (0.0f, 1.0f, (v.load() - min) / (max - min)); };
    b.set = [&v, min, max] (float p) { v.store (min + juce::jlimit (0.0f, 1.0f, p) * (max - min)); };
    b.text = [&v, suffix, decimals] { return juce::String (v.load(), decimals) + suffix; };
    b.reset = [&v, defaultValue] { v.store (defaultValue); };
    b.bipolar = min < 0.0f && max > 0.0f;
    return b;
}

KnobBinding bindLog (const juce::String& label, std::atomic<float>& v, float min, float max,
                     const juce::String& suffix, float defaultValue)
{
    KnobBinding b;
    const float lmin = std::log (min), lmax = std::log (max);
    b.label = label;
    b.get = [&v, lmin, lmax] { return juce::jlimit (0.0f, 1.0f, (std::log (juce::jmax (1.0e-6f, v.load())) - lmin) / (lmax - lmin)); };
    b.set = [&v, lmin, lmax] (float p) { v.store (std::exp (lmin + juce::jlimit (0.0f, 1.0f, p) * (lmax - lmin))); };
    b.text = [&v, suffix] { return juce::String (v.load(), v.load() < 10.0f ? 2 : 0) + suffix; };
    b.reset = [&v, defaultValue] { v.store (defaultValue); };
    return b;
}

KnobBinding bindFreq (const juce::String& label, std::atomic<float>& v, float min, float max, float defaultValue)
{
    auto b = bindLog (label, v, min, max, {}, defaultValue);
    b.text = [&v] { return formatHz (v.load()); };
    return b;
}

KnobBinding bindFader (const juce::String& label, std::atomic<float>& v)
{
    KnobBinding b;
    b.label = label;
    b.get = [&v] { return fader::dbToPos (v.load()); };
    b.set = [&v] (float p) { v.store (fader::posToDb (p)); };
    b.text = [&v] { return fader::text (v.load()); };
    b.reset = [&v] { v.store (0.0f); };
    return b;
}

KnobBinding bindModelParam (std::atomic<float>& v, const models::Param& p)
{
    KnobBinding b;
    b.label = p.label;
    b.get = [&v, p] { return p.toNormalised (v.load()); };
    b.set = [&v, p] (float pos) { v.store (p.fromNormalised (pos)); };
    b.text = [&v, p] { return consoleText (p, v.load()); }; // a list RATIO reads "3:1" everywhere
    b.reset = [&v, p] { v.store (p.defaultValue); };
    b.bipolar = p.min < 0.0f && p.max > 0.0f && p.isContinuous();
    if (p.isStepped() && p.steps > 1)
        b.steps = (float) p.steps;
    return b;
}

KnobBinding bindChoice (const juce::String& label, std::atomic<int32_t>& v, const juce::StringArray& items, int defaultIndex)
{
    KnobBinding b;
    const int n = juce::jmax (1, items.size());
    b.label = label;
    b.get = [&v, n] { return n > 1 ? (float) juce::jlimit (0, n - 1, (int) v.load()) / (float) (n - 1) : 0.0f; };
    b.set = [&v, n] (float p) { v.store (juce::roundToInt (juce::jlimit (0.0f, 1.0f, p) * (float) (n - 1))); };
    b.text = [&v, items] { return items[juce::jlimit (0, items.size() - 1, (int) v.load())]; };
    b.reset = [&v, defaultIndex] { v.store (defaultIndex); };
    b.steps = (float) n;
    return b;
}

KnobBinding bindPan (const juce::String& label, std::atomic<float>& v)
{
    auto b = bindLinear (label, v, -100.0f, 100.0f, {}, 0, 0.0f);
    // The WING shows pan as a signed number: -100 (left) .. 0 .. 100 (right).
    b.text = [&v] { return juce::String (juce::roundToInt (v.load())); };
    return b;
}

//==============================================================================
DisplayKnob::DisplayKnob()
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    slider.setRange (0.0, 1.0, 0.0);
    slider.setMouseDragSensitivity (220);
    slider.setVelocityBasedMode (false);
    slider.onValueChange = [this]
    {
        if (binding.isValid() && binding.set)
        {
            auto p = (float) slider.getValue();
            if (binding.steps > 1.0f)
                p = std::round (p * (binding.steps - 1.0f)) / (binding.steps - 1.0f);
            binding.set (p);
            timerCallback();
        }
    };
    slider.onDragEnd = [this] { timerCallback(); };
    slider.onDragStart = [this] { if (onTouch) onTouch(); };
    slider.setDoubleClickReturnValue (false, 0.0);
    slider.addMouseListener (this, false);
    addAndMakeVisible (slider);
    startTimerHz (20);
}

void DisplayKnob::setHardware (bool h)
{
    hardware = h;
    slider.getProperties().set ("hardware", h);
    resized();
    repaint();
}

void DisplayKnob::setCap (Cap cap)
{
    slider.getProperties().set ("cap", (int) cap);
    repaint();
}

void DisplayKnob::mouseEnter (const juce::MouseEvent&)
{
    if (onTouch && slider.isEnabled())
        onTouch();
}

void DisplayKnob::setBinding (KnobBinding b)
{
    binding = std::move (b);
    slider.setEnabled (binding.isValid());
    slider.getProperties().set ("bipolar", binding.bipolar);
    valueText = {};
    timerCallback();
    repaint();
}

void DisplayKnob::timerCallback()
{
    if (! binding.isValid())
        return;

    if (! slider.isMouseButtonDown())
    {
        const double v = binding.get();
        if (std::abs (v - slider.getValue()) > 1.0e-5)
            slider.setValue (v, juce::dontSendNotification);
    }

    const auto t = binding.text ? binding.text() : juce::String();
    if (t != valueText)
    {
        valueText = t;
        repaint();
    }
}

void DisplayKnob::paint (juce::Graphics& g)
{
    if (hardware)
        return;
    // The WING's knob box: the caption in grey above, the value in white below.
    auto b = getLocalBounds();
    const float captionSize = compact ? 14.0f : 16.0f;
    const float screenH = getWingScreenHeight();
    g.setColour (binding.isValid() ? Colours::caption : Colours::textDim.withAlpha (0.3f));
    g.setFont (wingFontPx (juce::jmin (TypeScale::caption * screenH, captionSize * 0.62f)));
    g.drawFittedText (binding.isValid() ? binding.label.toUpperCase() : juce::String(), b.removeFromTop ((int) captionSize + 2),
                      juce::Justification::centred, 1, 0.8f);
    g.setColour (Colours::text);
    g.setFont (wingFontPx (juce::jmin (TypeScale::value * screenH, (compact ? 15.0f : 18.0f) * 0.62f)));
    g.drawFittedText (valueText, b.removeFromBottom ((int) captionSize + 4), juce::Justification::centred, 1, 0.8f);
}

void DisplayKnob::resized()
{
    auto b = getLocalBounds();
    if (hardware)
    {
        const int size = juce::jmin (b.getWidth(), b.getHeight());
        slider.setBounds (b.withSizeKeepingCentre (size, size));
        return;
    }
    const int caption = compact ? 16 : 18;
    b.removeFromTop (caption);
    b.removeFromBottom (caption + 2);
    const int size = juce::jmin (b.getWidth(), b.getHeight());
    slider.setBounds (b.withSizeKeepingCentre (size, size));
}

//==============================================================================
ParamSlot::ParamSlot()
{
    setRepaintsOnMouseActivity (true);
    startTimerHz (20);
}

void ParamSlot::setBinding (KnobBinding b)
{
    binding = std::move (b);
    valueText = {};
    timerCallback();
    repaint();
}

void ParamSlot::timerCallback()
{
    if (! binding.isValid())
        return;
    const float p = binding.get();
    const auto t = binding.text ? binding.text() : juce::String();
    if (std::abs (p - position) > 1.0e-5f || t != valueText)
    {
        position = p;
        valueText = t;
        repaint();
    }
}

void ParamSlot::setPosition (float p)
{
    if (! binding.isValid() || ! binding.set)
        return;
    p = juce::jlimit (0.0f, 1.0f, p);
    if (binding.steps > 1.0f)
        p = std::round (p * (binding.steps - 1.0f)) / (binding.steps - 1.0f);
    binding.set (p);
    timerCallback();
}

void ParamSlot::paint (juce::Graphics& g)
{
    if (! binding.isValid())
        return;

    auto b = getLocalBounds().reduced (6, 2);
    auto bar = b.removeFromBottom (8).toFloat();
    g.setColour (isMouseOver() ? Colours::gold : Colours::caption);
    g.setFont (wingFontPx (juce::jmin (TypeScale::paramLabel * getWingScreenHeight(), (float) b.getHeight() * 0.5f)));
    g.drawFittedText (binding.label.toUpperCase(), b, juce::Justification::centredLeft, 1, 0.8f);
    g.setColour (Colours::text);
    g.drawFittedText (valueText, b, juce::Justification::centredRight, 1, 0.8f);

    // Position bar: a grey line, the value filled blue (from the centre when
    // bipolar), a white thumb.
    const float y = bar.getCentreY();
    g.setColour (Colours::screenLine);
    g.fillRect (bar.getX(), y - 1.0f, bar.getWidth(), 2.0f);
    const float x = bar.getX() + position * bar.getWidth();
    const float from = binding.bipolar ? bar.getCentreX() : bar.getX();
    g.setColour (Colours::valueBlue);
    g.fillRect (juce::jmin (from, x), y - 2.5f, std::abs (x - from), 5.0f);
    g.setColour (Colours::text);
    g.fillRoundedRectangle (x - 2.0f, bar.getY(), 4.0f, bar.getHeight(), 1.0f);
}

void ParamSlot::mouseDown (const juce::MouseEvent&)
{
    dragStart = position;
}

void ParamSlot::mouseDrag (const juce::MouseEvent& e)
{
    const float sensitivity = e.mods.isShiftDown() ? 0.2f : 1.0f;
    setPosition (dragStart + sensitivity * (float) (e.getDistanceFromDragStartX() - e.getDistanceFromDragStartY()) / (float) juce::jmax (100, getWidth() * 2));
}

void ParamSlot::mouseDoubleClick (const juce::MouseEvent&)
{
    if (binding.reset)
        binding.reset();
    timerCallback();
}

void ParamSlot::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    const float step = binding.steps > 1.0f ? 1.0f / (binding.steps - 1.0f) : 0.01f;
    setPosition (position + (wheel.deltaY > 0 ? step : -step) * (binding.steps > 1.0f ? 1.0f : 2.0f));
}

//==============================================================================
BoolButton::BoolButton (const juce::String& buttonText, juce::Colour onColour) : juce::TextButton (buttonText)
{
    setClickingTogglesState (true);
    setColour (buttonOnColourId, onColour);
    onClick = [this]
    {
        if (param != nullptr)
            param->store (getToggleState());
    };
    startTimerHz (15);
}

void BoolButton::bind (std::atomic<bool>* p)
{
    param = p;
    setEnabled (p != nullptr);
    timerCallback();
}

void BoolButton::timerCallback()
{
    const bool v = param != nullptr && param->load();
    if (getToggleState() != v)
        setToggleState (v, juce::dontSendNotification);
}

//==============================================================================
IntCombo::IntCombo()
{
    onChange = [this]
    {
        const int id = getSelectedId();
        if (id <= 0)
            return;
        const int value = id - 2;
        if (onValueChosen)
            onValueChosen (value);
        else if (param != nullptr)
            param->store (value);
    };
    startTimerHz (10);
}

void IntCombo::setItems (const juce::StringArray& items, const juce::String& noneText)
{
    clear (juce::dontSendNotification);
    hasNone = noneText.isNotEmpty();
    if (hasNone)
        addItem (noneText, 1);
    for (int i = 0; i < items.size(); ++i)
        addItem (items[i], i + 2);
    refreshSelection();
}

void IntCombo::bind (std::atomic<int32_t>* p)
{
    param = p;
    setEnabled (p != nullptr);
    refreshSelection();
}

void IntCombo::refreshSelection()
{
    if (param == nullptr)
        return;
    const int id = param->load() + 2;
    if (getSelectedId() != id)
        setSelectedId (id, juce::dontSendNotification);
}

void IntCombo::timerCallback()
{
    if (! isPopupActive())
        refreshSelection();
}

} // namespace livedaw::ui
