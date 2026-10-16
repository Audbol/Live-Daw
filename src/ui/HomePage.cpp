#include "ui/HomePage.h"
#include "dsp/DspUtils.h"
#include "ui/Icons.h"
#include "ui/MeterComponent.h"
#include "ui/ResponseGraph.h"

namespace livedaw::ui
{

namespace
{
    const char* blockTitle (HomeTab t)
    {
        switch (t)
        {
            case HomeTab::Home:       return "OVERVIEW";
            case HomeTab::Icon:       return "ICON/COLOR";
            case HomeTab::Name:       return "NAME";
            case HomeTab::Tags:       return "TAGS";
            case HomeTab::Input:      return "INPUT";
            case HomeTab::Filter:     return "FILTER";
            case HomeTab::Gate:       return "GATE";
            case HomeTab::Eq:         return "EQ";
            case HomeTab::Dynamics:   return "DYNAMICS";
            case HomeTab::PreInsert:  return "PRE FADER INSERT";
            case HomeTab::PostInsert: return "POST FADER INSERT";
            case HomeTab::Main:       return "MAIN";
            case HomeTab::Sends:      return "BUS SENDS";
        }
        return "";
    }

    /** A WING tab key: grey, the current one darker with yellow text and a yellow top line. */
    class WingTab final : public juce::TextButton
    {
    public:
        using juce::TextButton::TextButton;

        void paintButton (juce::Graphics& g, bool over, bool) override
        {
            auto r = getLocalBounds().toFloat();
            const bool front = getToggleState();
            g.setColour (front ? Colours::screenPanel : (over ? Colours::screenHeader.brighter (0.08f) : Colours::screenHeader));
            g.fillRect (r);
            if (front)
            {
                g.setColour (Colours::yellow);
                g.fillRect (r.withHeight (3.0f));
            }
            g.setColour (! isEnabled() ? Colours::textDim : (front ? Colours::yellow : Colours::text));
            g.setFont (juce::FontOptions (juce::jlimit (13.0f, 24.0f, r.getHeight() * 0.55f), juce::Font::bold));
            g.drawFittedText (getButtonText(), getLocalBounds().reduced (4, 0), juce::Justification::centred, 1);
        }
    };

    /** A screen key with an icon above or beside its text; lit in `onColour` when toggled. */
    class IconKey final : public juce::TextButton, private juce::Timer
    {
    public:
        IconKey (const juce::String& keyText, Icon i, juce::Colour on) : juce::TextButton (keyText), icon (i), onColour (on)
        {
            startTimerHz (8);
        }

        std::function<bool()> isOn;
        bool iconOnly = false, textOnly = false;

        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            const bool on = getToggleState();
            auto r = getLocalBounds().toFloat();
            g.setColour (on ? onColour : (down ? Colours::screenTile : (over ? Colours::screenHeader.brighter (0.1f) : Colours::screenHeader)));
            g.fillRoundedRectangle (r, 2.0f);
            const auto ink = on ? juce::Colours::black : Colours::text;
            if (textOnly)
            {
                g.setColour (ink);
                g.setFont (juce::FontOptions (juce::jlimit (12.0f, 22.0f, r.getHeight() * 0.48f), juce::Font::bold));
                g.drawFittedText (getButtonText(), r.toNearestInt().reduced (3, 0), juce::Justification::centred, 1);
                return;
            }
            if (iconOnly || getButtonText().isEmpty())
            {
                drawIcon (g, icon, r.reduced (r.getHeight() * 0.18f), ink);
                return;
            }
            auto iconBox = r.removeFromLeft (r.getHeight()).reduced (r.getHeight() * 0.18f);
            if (r.getWidth() < 40.0f)
                iconBox = r.reduced (4.0f);
            drawIcon (g, icon, iconBox, ink);
            g.setColour (ink);
            g.setFont (juce::FontOptions (juce::jlimit (12.0f, 20.0f, r.getHeight() * 0.42f), juce::Font::bold));
            g.drawFittedText (getButtonText(), r.toNearestInt().reduced (4, 0), juce::Justification::centredLeft, 1);
        }

    private:
        void timerCallback() override
        {
            if (isOn && getToggleState() != isOn())
                setToggleState (isOn(), juce::dontSendNotification);
        }

        Icon icon;
        juce::Colour onColour;
    };

    float ratioOf (const DynSlotParams& slot, bool gate)
    {
        const auto& spec = models::model (models::Dynamics, slot.model.load());
        const int i = spec.find ("ratio");
        if (i < 0)
            return gate ? 100.0f : 4.0f;
        const auto& p = spec.params[(size_t) i];
        const int index = juce::roundToInt (slot.values[i].load());
        const auto text = p.choices[juce::jlimit (0, juce::jmax (0, p.choices.size() - 1), index)];
        if (text.containsIgnoreCase ("gate"))
            return 100.0f;
        const float r = text.contains (":") ? text.fromFirstOccurrenceOf (":", false, false).getFloatValue() : text.getFloatValue();
        return r > 0.0f ? r : (gate ? 100.0f : 4.0f);
    }

    float thresholdOf (const DynSlotParams& slot)
    {
        const auto& spec = models::model (models::Dynamics, slot.model.load());
        const int i = spec.find ("thr");
        if (i < 0)
            return -20.0f;
        const auto& p = spec.params[(size_t) i];
        const float v = slot.values[i].load();
        return p.unit == "dB" ? v : -60.0f + 60.0f * p.toNormalised (v);
    }

    /** Transfer curve of a dynamics slot (input -60..0 dB against output). */
    class TransferGraph final : public juce::Component, private juce::Timer
    {
    public:
        TransferGraph (DynSlotParams& s, bool isGate, std::function<void()> open) : slot (s), gate (isGate), onOpen (std::move (open))
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            startTimerHz (6);
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (juce::Colours::black);
            g.fillRect (r);
            auto plot = r.reduced (4.0f);
            g.setColour (Colours::screenLine.withAlpha (0.6f));
            for (int i = 1; i < 4; ++i)
            {
                g.drawHorizontalLine (juce::roundToInt (plot.getY() + plot.getHeight() * (float) i / 4.0f), plot.getX(), plot.getRight());
                g.drawVerticalLine (juce::roundToInt (plot.getX() + plot.getWidth() * (float) i / 4.0f), plot.getY(), plot.getBottom());
            }

            const float thr = thresholdOf (slot), ratio = ratioOf (slot, gate);
            auto map = [&plot] (float inDb, float outDb)
            {
                return juce::Point<float> (plot.getX() + plot.getWidth() * (inDb + 60.0f) / 60.0f,
                                           plot.getBottom() - plot.getHeight() * juce::jlimit (0.0f, 1.0f, (outDb + 60.0f) / 60.0f));
            };
            juce::Path p;
            for (int i = 0; i <= 60; ++i)
            {
                const float in = -60.0f + (float) i;
                float out = in;
                if (gate && in < thr)
                    out = thr + (in - thr) * juce::jmin (ratio, 8.0f);
                else if (! gate && in > thr)
                    out = thr + (in - thr) / ratio;
                if (i == 0)
                    p.startNewSubPath (map (in, out));
                else
                    p.lineTo (map (in, out));
            }
            const bool on = slot.on.load();
            g.setColour (on ? Colours::amber : Colours::textDim);
            g.strokePath (p, juce::PathStrokeType (2.0f));
            g.setColour (Colours::text);
            const auto knee = map (thr, thr);
            g.fillEllipse (knee.x - 3.5f, knee.y - 3.5f, 7.0f, 7.0f);

            g.setColour (on ? Colours::amber : Colours::textDim);
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.drawText (models::model (models::Dynamics, slot.model.load()).token, r.reduced (6.0f, 3.0f).toNearestInt(),
                        juce::Justification::topLeft);
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (getLocalBounds().contains (e.getPosition()) && onOpen)
                onOpen();
        }

    private:
        void timerCallback() override
        {
            const auto h = std::make_tuple (slot.on.load(), slot.model.load(), thresholdOf (slot), ratioOf (slot, gate));
            if (h != shown)
            {
                shown = h;
                repaint();
            }
        }

        DynSlotParams& slot;
        bool gate;
        std::function<void()> onOpen;
        std::tuple<bool, int, float, float> shown;
    };

    /** The source tile of the INPUT panel. */
    class SourceTile final : public juce::Component, private juce::Timer
    {
    public:
        SourceTile (UiContext& c, Strip& s, std::function<void()> open) : ctx (c), strip (s), onOpen (std::move (open))
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            startTimerHz (4);
        }

        juce::String sourceName() const
        {
            if (! strip.isInput())
                return strip.getLabel();
            const int in = strip.params.inputLeft.load();
            if (in < 0)
                return "OFF";
            const auto& names = ctx.engine.getHardwareInputNames();
            juce::String name = in < names.size() ? names[in] : "IN " + juce::String (in + 1);
            if (strip.params.stereo.load())
            {
                const int right = strip.params.inputRight.load() >= 0 ? strip.params.inputRight.load() : in + 1;
                name = "IN " + juce::String (in + 1) + "/" + juce::String (right + 1);
            }
            return name;
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (juce::Colours::black);
            g.fillRect (r);
            auto inner = r.reduced (3.0f);
            auto header = inner.removeFromTop (juce::jmin (26.0f, inner.getHeight() * 0.22f));
            g.setColour (Colours::amber);
            g.fillRect (header);
            g.setColour (juce::Colours::black);
            g.setFont (juce::FontOptions (header.getHeight() * 0.8f, juce::Font::bold));
            g.drawFittedText (shown, header.toNearestInt(), juce::Justification::centred, 1);

            g.setColour (Colours::screenHeader);
            g.fillRect (inner);
            auto label = inner.removeFromBottom (inner.getHeight() * 0.28f);
            drawIcon (g, strip.isInput() ? Icon::Xlr : Icon::Bus, inner.reduced (inner.getHeight() * 0.18f).withTrimmedRight (inner.getWidth() * 0.3f), Colours::text);
            drawChip (g, inner.removeFromRight (inner.getWidth() * 0.32f).reduced (4.0f).withSizeKeepingCentre (inner.getWidth() * 0.26f, 18.0f),
                      strip.params.stereo.load() || strip.isOutput() ? "ST" : "M", Colours::text, juce::Colours::black);
            g.setColour (Colours::screenLine);
            g.fillRect (label.removeFromTop (1.0f));
            g.setColour (Colours::lime);
            g.setFont (juce::FontOptions (label.getHeight() * 0.75f, juce::Font::bold));
            g.drawFittedText (stripName (strip), label.toNearestInt(), juce::Justification::centred, 1);
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (getLocalBounds().contains (e.getPosition()) && onOpen)
                onOpen();
        }

    private:
        void timerCallback() override
        {
            const auto s = sourceName();
            if (s != shown)
            {
                shown = s;
                repaint();
            }
        }

        UiContext& ctx;
        Strip& strip;
        std::function<void()> onOpen;
        juce::String shown;
    };

    /** The TRIM readout of the INPUT panel; drag up/down to change it. */
    class TrimBox final : public juce::Component, private juce::Timer
    {
    public:
        explicit TrimBox (std::atomic<float>& t) : trim (t) { startTimerHz (10); }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (juce::Colours::black);
            g.fillRect (r);
            auto ring = r.removeFromLeft (r.getHeight()).reduced (r.getHeight() * 0.15f);
            const float a0 = juce::MathConstants<float>::pi * 1.25f, a1 = juce::MathConstants<float>::pi * 2.75f;
            const float pos = (trim.load() + 18.0f) / 36.0f;
            juce::Path track, value;
            const float rad = ring.getWidth() * 0.4f;
            track.addCentredArc (ring.getCentreX(), ring.getCentreY(), rad, rad, 0.0f, a0, a1, true);
            value.addCentredArc (ring.getCentreX(), ring.getCentreY(), rad, rad, 0.0f, (a0 + a1) * 0.5f, a0 + pos * (a1 - a0), true);
            g.setColour (Colours::screenHeader);
            g.strokePath (track, juce::PathStrokeType (ring.getWidth() * 0.16f));
            g.setColour (Colours::text);
            g.strokePath (value, juce::PathStrokeType (ring.getWidth() * 0.16f));
            r.removeFromLeft (6.0f);
            g.setFont (juce::FontOptions (r.getHeight() * 0.32f, juce::Font::bold));
            g.drawText ("TRIM", r.removeFromTop (r.getHeight() * 0.42f), juce::Justification::bottomLeft);
            g.setFont (juce::FontOptions (r.getHeight() * 0.8f, juce::Font::bold));
            g.drawText (juce::String (trim.load(), 1) + " dB", r, juce::Justification::topLeft);
        }

        void mouseDown (const juce::MouseEvent&) override { start = trim.load(); }
        void mouseDrag (const juce::MouseEvent& e) override { trim = juce::jlimit (-18.0f, 18.0f, start - (float) e.getDistanceFromDragStartY() * 0.1f); repaint(); }
        void mouseDoubleClick (const juce::MouseEvent&) override { trim = 0.0f; repaint(); }

    private:
        void timerCallback() override
        {
            if (trim.load() != shown)
            {
                shown = trim.load();
                repaint();
            }
        }

        std::atomic<float>& trim;
        float start = 0.0f, shown = 1.0e9f;
    };

    /** The LEVEL panel's fader: a light blue bar with the value in a tag; drag to move. */
    class ScreenFader final : public juce::Component, private juce::Timer
    {
    public:
        explicit ScreenFader (std::atomic<float>& f) : faderDb (f) { startTimerHz (20); }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            auto scale = r.removeFromLeft (r.getWidth() * 0.42f);
            auto slot = r.reduced (r.getWidth() * 0.22f, 8.0f);
            g.setColour (juce::Colours::black);
            g.fillRect (r);

            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            for (float db : { 10.0f, 5.0f, 0.0f, -5.0f, -10.0f, -20.0f, -30.0f, -50.0f })
            {
                const float y = slot.getBottom() - slot.getHeight() * fader::dbToPos (db);
                g.setColour (Colours::textDim);
                g.drawText ((db > 0 ? "+" : "") + juce::String ((int) db), scale.withY (y - 7.0f).withHeight (14.0f).toNearestInt(),
                            juce::Justification::centredRight);
                g.setColour (Colours::screenLine);
                g.fillRect (slot.getX() - 6.0f, y, 4.0f, 1.0f);
            }
            g.setColour (Colours::textDim);
            g.drawText ("-oo", scale.withY (slot.getBottom() - 7.0f).withHeight (14.0f).toNearestInt(), juce::Justification::centredRight);

            g.setColour (Colours::screenDark);
            g.fillRect (slot);
            const float pos = fader::dbToPos (faderDb.load());
            const float y = slot.getBottom() - slot.getHeight() * pos;
            g.setColour (Colours::cyan);
            g.fillRect (slot.withTop (y));

            auto tag = juce::Rectangle<float> (juce::jmin (getWidth() * 0.9f, 74.0f), 24.0f).withCentre ({ slot.getCentreX(), y });
            tag.setX (juce::jmin (tag.getX(), (float) getWidth() - tag.getWidth()));
            g.setColour (Colours::text);
            g.fillRect (tag);
            g.setColour (juce::Colours::black);
            g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
            g.drawFittedText (fader::text (faderDb.load()) + " dB", tag.toNearestInt(), juce::Justification::centred, 1);
        }

        void mouseDown (const juce::MouseEvent&) override { start = fader::dbToPos (faderDb.load()); }
        void mouseDrag (const juce::MouseEvent& e) override
        {
            faderDb = fader::posToDb (start - (float) e.getDistanceFromDragStartY() / juce::jmax (50.0f, (float) getHeight() - 16.0f));
            repaint();
        }
        void mouseDoubleClick (const juce::MouseEvent&) override { faderDb = 0.0f; repaint(); }

    private:
        void timerCallback() override
        {
            if (faderDb.load() != shown)
            {
                shown = faderDb.load();
                repaint();
            }
        }

        std::atomic<float>& faderDb;
        float start = 0.0f, shown = 1.0e9f;
    };

    /** Main 1-4: the selected main (white frame) and which mains the strip feeds (lit). */
    class MainSelector final : public juce::Component, private juce::Timer
    {
    public:
        explicit MainSelector (StripParams& p) : params (p) { startTimerHz (6); }

        std::function<void (int)> onSelect;
        int selected = 0;

        void paint (juce::Graphics& g) override
        {
            const float h = (float) getHeight() / (float) kNumMains;
            for (int i = 0; i < kNumMains; ++i)
            {
                auto r = juce::Rectangle<float> (0.0f, (float) i * h, (float) getWidth(), h).reduced (1.0f, 2.0f);
                const bool on = params.mains[i].on.load();
                g.setColour (on ? Colours::cyan : juce::Colours::black);
                g.fillRect (r);
                if (i == selected)
                {
                    g.setColour (Colours::text);
                    g.drawRect (r, 1.5f);
                }
                g.setColour (on ? juce::Colours::black : Colours::text);
                g.setFont (juce::FontOptions (h * 0.7f, juce::Font::bold));
                g.drawText (juce::String (i + 1), r.toNearestInt(), juce::Justification::centred);
            }
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            const int i = juce::jlimit (0, kNumMains - 1, e.y * kNumMains / juce::jmax (1, getHeight()));
            if (e.mods.isPopupMenu() || e.getNumberOfClicks() > 1 || i == selected)
                params.mains[i].on = ! params.mains[i].on.load();
            selected = i;
            if (onSelect)
                onSelect (i);
            repaint();
        }

    private:
        void timerCallback() override { repaint(); }
        StripParams& params;
    };
} // namespace

//==============================================================================
namespace
{
/** OVERVIEW: INPUT, FILTER, LEVEL, EQ and MAIN panels, as on the console. */
class OverviewScreen final : public HomeScreen
{
public:
    OverviewScreen (UiContext& c, Strip& s)
        : HomeScreen (c, s),
          meter ([this] (int ch) { return ctx.meters.get (&strip, false, ch); }, [this] { return strip.getNumChannels(); })
    {
        auto& p = strip.params;
        auto open = [this] (HomeTab t) { return [this, t] { if (ctx.showHomeTab) ctx.showHomeTab (t); }; };

        source = &make<SourceTile> (ctx, strip, open (HomeTab::Input));
        invert = &make<IconKey> (juce::String::fromUTF8 ("\xc3\x98"), Icon::Check, Colours::yellow);
        invert->isOn = [&p] { return p.input.invert.load(); };
        invert->onClick = [&p] { p.input.invert = ! p.input.invert.load(); };
        trim = &make<TrimBox> (p.input.trimDb);
        if (strip.getKind() == StripKind::Channel)
            gateGraph = &make<TransferGraph> (p.gate, true, open (HomeTab::Gate));
        dynGraph = &make<TransferGraph> (p.dynamics, false, open (HomeTab::Dynamics));

        if (strip.getKind() == StripKind::Channel)
        {
            auto addFilterKey = [this] (const char* text, Icon icon, std::atomic<bool>& flag)
            {
                auto& k = make<IconKey> (text, icon, Colours::yellow);
                k.isOn = [&flag] { return flag.load(); };
                k.onClick = [&flag] { flag = ! flag.load(); };
                return &k;
            };
            loCut = addFilterKey ("LO CUT", Icon::LowCut, p.filter.lowCut);
            hiCut = addFilterKey ("HI CUT", Icon::HighCut, p.filter.highCut);
            tilt = addFilterKey ("TOOL", Icon::Tilt, p.filter.tool);
        }
        else
        {
            delayOn = &make<IconKey> ("DELAY", Icon::Wrench, Colours::yellow);
            delayOn->isOn = [&p] { return p.input.delayOn.load(); };
            delayOn->onClick = [&p] { p.input.delayOn = ! p.input.delayOn.load(); };
            monoKey = &make<IconKey> ("MONO", Icon::Speaker, Colours::yellow);
            monoKey->isOn = [&p] { return p.mono.load(); };
            monoKey->onClick = [&p] { p.mono = ! p.mono.load(); };
        }

        solo = &make<IconKey> ("SOLO", Icon::Headphones, Colours::yellow);
        solo->textOnly = true;
        solo->isOn = [&p] { return p.solo.load(); };
        solo->onClick = [&p] { p.solo = ! p.solo.load(); };
        mute = &make<IconKey> ("MUTE", Icon::Speaker, Colours::mute);
        mute->textOnly = true;
        mute->isOn = [&p] { return p.mute.load(); };
        mute->onClick = [&p] { p.mute = ! p.mute.load(); };
        meter.setShowScale (true);
        addAndMakeVisible (meter);
        fader = &make<ScreenFader> (p.faderDb);

        graph = &make<ResponseGraph>();
        graph->setStrip (&strip, ResponseGraph::Eq);

        if (strip.getKind() != StripKind::Main && strip.getKind() != StripKind::Matrix)
        {
            mainKnob = &make<DisplayKnob>();
            mains = &make<MainSelector> (p);
            mains->onSelect = [this] (int i) { mainKnob->setBinding (bindFader ("MAIN " + juce::String (i + 1), strip.params.mains[i].levelDb)); };
            mains->onSelect (0);
        }
        else
        {
            mainKnob = &make<DisplayKnob>();
            mainKnob->setBinding (bindFader ("LEVEL", p.faderDb));
        }
        soloA = &make<IconKey> ("", Icon::Headphones, Colours::lime);
        soloA->isOn = [&p] { return p.monitorBus.load() != 1; };
        soloA->onClick = [&p] { p.monitorBus = p.monitorBus.load() == 2 ? 1 : (p.monitorBus.load() == 1 ? 2 : 0); };
        soloB = &make<IconKey> ("", Icon::Speaker, Colours::lime);
        soloB->isOn = [&p] { return p.monitorBus.load() != 0; };
        soloB->onClick = [&p] { p.monitorBus = p.monitorBus.load() == 2 ? 0 : (p.monitorBus.load() == 0 ? 2 : 1); };
        soloBusLabel = &label ("SOLO BUS", 16.0f, Colours::text);
        soloBusLabel->setJustificationType (juce::Justification::centred);
    }

    std::vector<KnobBinding> knobs() override
    {
        auto& p = strip.params;
        std::vector<KnobBinding> k;
        k.push_back (bindLinear ("TRIM", p.input.trimDb, -18.0f, 18.0f, " dB", 1, 0.0f));
        if (strip.getKind() == StripKind::Channel)
        {
            k.push_back (bindFreq ("LO CUT", p.filter.lowCutFreq, 20.0f, 2000.0f, 80.0f));
            k.push_back (thresholdBinding (p.gate, "GATE THR"));
        }
        else
        {
            k.push_back ({});
            k.push_back ({});
        }
        k.push_back (thresholdBinding (p.dynamics, "DYN THR"));
        k.push_back (bindLinear ("WIDTH", p.width, -150.0f, 150.0f, " %", 0, 100.0f));
        k.back().bipolar = true;
        k.push_back (bindPan ("PAN", p.pan));
        return k;
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        auto levelCol = b.removeFromRight (b.getWidth() * 22 / 100);
        auto top = b.removeFromTop (b.getHeight() / 2);
        inputArea = top.removeFromLeft (top.getWidth() * 78 / 100);
        filterArea = top;
        eqArea = b.removeFromLeft (b.getWidth() * 78 / 100);
        mainArea = b;
        levelArea = levelCol;

        // INPUT: source tile, invert, gate/dyn curves, trim.
        auto in = box (inputArea, "INPUT", true);
        auto tileCol = in.removeFromLeft (juce::jmin (130, in.getWidth() / 4));
        source->setBounds (tileCol.removeFromTop (juce::jmin (tileCol.getHeight() - 40, 120)));
        tileCol.removeFromTop (8);
        invert->setBounds (tileCol.removeFromTop (juce::jmin (38, tileCol.getHeight())).withSizeKeepingCentre (60, juce::jmin (38, tileCol.getHeight())));
        in.removeFromLeft (12);
        auto right = in.removeFromRight (juce::jmin (190, in.getWidth() / 2));
        trim->setBounds (right.removeFromTop (juce::jmin (56, right.getHeight() / 2)));
        auto graphs = in.reduced (0, 4);
        const int gh = (graphs.getHeight() - 8) / 2;
        if (gateGraph != nullptr)
            gateGraph->setBounds (graphs.removeFromTop (gh).withSizeKeepingCentre (juce::jmin (graphs.getWidth(), gh * 2), gh));
        else
            graphs.removeFromTop (gh);
        graphs.removeFromTop (8);
        dynGraph->setBounds (graphs.removeFromTop (gh).withSizeKeepingCentre (juce::jmin (graphs.getWidth(), gh * 2), gh));

        // FILTER: low cut, high cut, tool (buses: delay and mono).
        auto f = box (filterArea, strip.getKind() == StripKind::Channel ? "FILTER" : "OUTPUT", strip.getKind() == StripKind::Channel);
        const int kh = juce::jmin (46, f.getHeight() / 3);
        for (auto* k : { loCut, hiCut, tilt, delayOn, monoKey })
            if (k != nullptr)
                k->setBounds (f.removeFromTop (kh).reduced (0, 4));

        // LEVEL: SOLO, meter, fader, MUTE.
        auto l = box (levelArea, "LEVEL");
        auto meterCol = l.removeFromLeft (l.getWidth() * 38 / 100);
        solo->setBounds (meterCol.removeFromTop (40).reduced (2, 2));
        mute->setBounds (meterCol.removeFromBottom (40).reduced (2, 2));
        meter.setBounds (meterCol.reduced (6, 8));
        fader->setBounds (l);

        // EQ graph.
        graph->setBounds (box (eqArea, "EQ", true));

        // MAIN: send level knob, main 1-4, solo bus.
        auto m = box (mainArea, strip.getKind() == StripKind::Main || strip.getKind() == StripKind::Matrix ? "LEVEL" : "MAIN", true);
        auto soloRow = m.removeFromBottom (juce::jmin (44, m.getHeight() / 4));
        soloBusLabel->setBounds (m.removeFromBottom (22));
        soloA->setBounds (soloRow.removeFromLeft (soloRow.getWidth() / 2).reduced (3, 2));
        soloB->setBounds (soloRow.reduced (3, 2));
        if (mains != nullptr)
            mains->setBounds (m.removeFromRight (juce::jmin (34, m.getWidth() / 4)).reduced (0, 4));
        mainKnob->setBounds (m);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        // Panel headers with a chevron open their screen.
        const auto pos = e.getPosition();
        auto header = [] (juce::Rectangle<int> r) { return r.reduced (3).removeFromTop (kPanelHeaderHeight); };
        HomeTab target = HomeTab::Home;
        if (header (inputArea).contains (pos))
            target = HomeTab::Input;
        else if (header (filterArea).contains (pos) && strip.getKind() == StripKind::Channel)
            target = HomeTab::Filter;
        else if (header (eqArea).contains (pos))
            target = HomeTab::Eq;
        else if (header (mainArea).contains (pos))
            target = HomeTab::Main;
        if (target != HomeTab::Home && ctx.showHomeTab)
            ctx.showHomeTab (target);
    }

private:
    SourceTile* source = nullptr;
    IconKey *invert = nullptr, *loCut = nullptr, *hiCut = nullptr, *tilt = nullptr, *delayOn = nullptr, *monoKey = nullptr;
    IconKey *solo = nullptr, *mute = nullptr, *soloA = nullptr, *soloB = nullptr;
    TrimBox* trim = nullptr;
    TransferGraph *gateGraph = nullptr, *dynGraph = nullptr;
    MeterComponent meter;
    ScreenFader* fader = nullptr;
    ResponseGraph* graph = nullptr;
    DisplayKnob* mainKnob = nullptr;
    MainSelector* mains = nullptr;
    juce::Label* soloBusLabel = nullptr;
    juce::Rectangle<int> inputArea, filterArea, levelArea, eqArea, mainArea;
};

//==============================================================================
/** ICON/COLOR: the strip's colour, icon and scribble light. */
class IconColorScreen final : public HomeScreen, private juce::Timer
{
public:
    IconColorScreen (UiContext& c, Strip& s) : HomeScreen (c, s)
    {
        for (int i = 1; i <= 18; ++i)
        {
            auto* sw = new juce::TextButton (wingColourName (i).toUpperCase());
            sw->setColour (juce::TextButton::buttonColourId, wingColour (i));
            sw->setColour (juce::TextButton::buttonOnColourId, wingColour (i));
            sw->setColour (juce::TextButton::textColourOffId, juce::Colours::black);
            sw->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
            sw->onClick = [this, i] { strip.setColourIndex (i); };
            swatches.add (sw);
            addAndMakeVisible (sw);
        }
        icon = &knob (iconBinding());
        led = &toggle ("SCRIBBLE LIGHT", strip.params.led, Colours::yellow);
        startTimerHz (6);
        timerCallback();
    }

    KnobBinding iconBinding()
    {
        auto& i = strip.params.icon;
        KnobBinding b;
        b.label = "ICON";
        b.get = [&i] { return (float) i.load() / 999.0f; };
        b.set = [&i] (float p) { i = juce::roundToInt (p * 999.0f); };
        b.text = [&i] { return i.load() == 0 ? juce::String ("NONE") : "#" + juce::String (i.load()); };
        b.reset = [&i] { i = 0; };
        b.steps = 1000.0f;
        return b;
    }

    KnobBinding colourBinding()
    {
        auto& col = strip.params.colour;
        KnobBinding b;
        b.label = "COLOR";
        b.get = [&col] { return (float) (juce::jlimit (1, 18, (int) col.load()) - 1) / 17.0f; };
        b.set = [&col] (float p) { col = 1 + juce::roundToInt (p * 17.0f); };
        b.text = [&col] { return wingColourName (col.load()).toUpperCase(); };
        b.steps = 18.0f;
        return b;
    }

    std::vector<KnobBinding> knobs() override { return { colourBinding(), iconBinding() }; }

    void timerCallback() override
    {
        for (int i = 0; i < swatches.size(); ++i)
            swatches[i]->setToggleState (strip.getColourIndex() == i + 1, juce::dontSendNotification);
        repaint();
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        auto colours = box (b.removeFromTop (b.getHeight() * 55 / 100), "COLOR");
        const int columns = 6;
        const int w = colours.getWidth() / columns, h = colours.getHeight() / 3;
        for (int i = 0; i < swatches.size(); ++i)
            swatches[i]->setBounds (colours.getX() + (i % columns) * w, colours.getY() + (i / columns) * h, w - 6, h - 6);
        auto rest = b;
        auto ic = box (rest.removeFromLeft (rest.getWidth() / 2), "ICON");
        icon->setBounds (ic.withSizeKeepingCentre (juce::jmin (140, ic.getWidth()), ic.getHeight()));
        auto sc = box (rest, "SCRIBBLE");
        led->setBounds (sc.withSizeKeepingCentre (juce::jmin (220, sc.getWidth()), juce::jmin (44, sc.getHeight())));
    }

    void paint (juce::Graphics& g) override
    {
        HomeScreen::paint (g);
        for (auto* sw : swatches)
            if (sw->getToggleState())
            {
                g.setColour (juce::Colours::white);
                g.drawRect (sw->getBounds().expanded (3), 3);
            }
    }

private:
    juce::OwnedArray<juce::TextButton> swatches;
    DisplayKnob* icon = nullptr;
    BoolButton* led = nullptr;
};

/** NAME: the strip's name. */
class NameScreen final : public HomeScreen, private juce::Timer
{
public:
    NameScreen (UiContext& c, Strip& s) : HomeScreen (c, s)
    {
        editor = &make<juce::TextEditor>();
        editor->setText (strip.getName(), false);
        editor->setFont (juce::FontOptions (40.0f, juce::Font::bold));
        editor->setJustification (juce::Justification::centredLeft);
        editor->onTextChange = [this] { strip.setName (editor->getText()); };
        editor->onReturnKey = [this] { unfocusAllComponents(); };
        hint = &label ("The name shows on the scribble strip, the channel tab and in every list.", 16.0f);
        clearKey = &make<juce::TextButton> ("CLEAR");
        clearKey->onClick = [this] { editor->setText ({}, true); };
        startTimerHz (4);
    }

    void timerCallback() override
    {
        if (! editor->hasKeyboardFocus (true) && editor->getText() != strip.getName())
            editor->setText (strip.getName(), false);
    }

    void resized() override
    {
        clearBoxes();
        auto n = box (getLocalBounds().removeFromTop (juce::jmin (220, getHeight())), "NAME");
        auto row = n.removeFromTop (60);
        clearKey->setBounds (row.removeFromRight (110).reduced (4, 8));
        editor->setBounds (row.reduced (4, 4));
        n.removeFromTop (10);
        hint->setBounds (n.removeFromTop (26));
    }

private:
    juce::TextEditor* editor = nullptr;
    juce::Label* hint = nullptr;
    juce::TextButton* clearKey = nullptr;
};

/** TAGS: DCA and mute group assignments. */
class TagsScreen final : public HomeScreen
{
public:
    TagsScreen (UiContext& c, Strip& s) : HomeScreen (c, s)
    {
        dcas = &make<MaskButtons> (strip.params.dcaMask, kNumDcas, "DCA ", Colours::cyan);
        mutes = &make<MaskButtons> (strip.params.muteGroupMask, kNumMuteGroups, "MUTE ", Colours::mute);
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        dcas->setBounds (box (b.removeFromTop (b.getHeight() * 55 / 100), "DCA GROUPS"));
        mutes->setBounds (box (b, "MUTE GROUPS").withHeight (juce::jmin (60, b.getHeight() - 40)));
    }

private:
    MaskButtons *dcas = nullptr, *mutes = nullptr;
};
} // namespace

//==============================================================================
/** The left column of the HOME screen: HOME, then a thumbnail per processing
    block in signal order with a key that switches it, and the wrench. */
class HomePage::BlockColumn final : public juce::Component, private juce::Timer
{
public:
    explicit BlockColumn (HomePage& p) : page (p) { startTimerHz (8); }

    void setStrip (Strip* s)
    {
        strip = s;
        rows.clear();
        if (strip == nullptr)
        {
            repaint();
            return;
        }

        rows.push_back ({ HomeTab::Home, false });
        rows.push_back ({ HomeTab::Input, false });
        const auto order = models::list (models::ProcessOrders)[juce::jlimit (0, 23, (int) strip->params.processOrder.load())];
        for (auto letter : order)
        {
            if (letter == 'G' && strip->getKind() == StripKind::Channel)
                rows.push_back ({ HomeTab::Gate, true });
            else if (letter == 'E')
                rows.push_back ({ HomeTab::Eq, true });
            else if (letter == 'D')
                rows.push_back ({ HomeTab::Dynamics, true });
            else if (letter == 'I')
                rows.push_back ({ HomeTab::PreInsert, true });
        }
        rows.push_back ({ HomeTab::PostInsert, true });
        rows.push_back ({ HomeTab::Main, false });
        if (strip->isInput() || (strip->isBus() && strip->getId() >= kBusToBusTargets))
            rows.push_back ({ HomeTab::Sends, false });
        builtOrder = strip->params.processOrder.load();
        resized();
        repaint();
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (4, 2);
        wrench = b.removeFromBottom (juce::jmin (48, b.getHeight() / 10)).removeFromRight (juce::jmin (52, b.getWidth() / 3)).reduced (2);
        b.removeFromBottom (4);
        const int n = (int) rows.size();
        if (n == 0)
            return;
        const int h = juce::jmin (70, b.getHeight() / n);
        for (auto& r : rows)
        {
            auto row = b.removeFromTop (h).reduced (0, 3);
            r.key = r.hasKey ? row.removeFromRight (juce::jmin (52, row.getWidth() / 3)).reduced (2, 2) : juce::Rectangle<int>();
            if (r.hasKey)
                row.removeFromRight (8);
            r.thumb = r.tab == HomeTab::Home ? row.withWidth (juce::jmin (row.getWidth(), 110)) : row;
        }
    }

    void paint (juce::Graphics& g) override
    {
        if (strip == nullptr)
            return;
        auto& p = strip->params;

        for (auto& r : rows)
        {
            const bool selected = page.current == r.tab || (r.tab == HomeTab::Home && isOverviewGroup (page.current));
            auto t = r.thumb.toFloat();
            g.setColour (Colours::screenHeader);
            g.fillRect (t);
            paintThumb (g, r.tab, t.reduced (3.0f));
            if (selected)
            {
                g.setColour (Colours::yellow);
                g.drawRect (t, 2.5f);
            }

            if (r.hasKey)
            {
                const bool on = isOn (r.tab);
                auto k = r.key.toFloat();
                g.setColour (on ? (r.tab == HomeTab::Eq ? Colours::cyan : Colours::amber) : Colours::screenHeader);
                g.fillRect (k);
                drawIcon (g, keyIcon (r.tab), k.reduced (k.getHeight() * 0.22f), on ? juce::Colours::black : Colours::text);
                // Pointer from the key to its thumbnail.
                juce::Path tri;
                tri.addTriangle (k.getX() - 7.0f, k.getCentreY(), k.getX() - 1.0f, k.getCentreY() - 6.0f, k.getX() - 1.0f, k.getCentreY() + 6.0f);
                g.setColour (Colours::screenHeader);
                g.fillPath (tri);
            }
        }

        g.setColour (Colours::screenHeader);
        g.fillRect (wrench.toFloat());
        drawIcon (g, Icon::Wrench, wrench.toFloat().reduced (wrench.getHeight() * 0.2f), Colours::text);
        (void) p;
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (strip == nullptr || ! getLocalBounds().contains (e.getPosition()))
            return;
        const auto pos = e.getPosition();

        if (wrench.contains (pos))
        {
            showOrderMenu();
            return;
        }

        for (auto& r : rows)
        {
            if (r.hasKey && r.key.contains (pos))
            {
                toggle (r.tab);
                repaint();
                return;
            }
            if (r.thumb.contains (pos))
            {
                page.showTab (r.tab);
                return;
            }
        }
    }

private:
    struct Row
    {
        HomeTab tab;
        bool hasKey;
        juce::Rectangle<int> thumb {}, key {};
    };

    static Icon keyIcon (HomeTab t)
    {
        if (t == HomeTab::Gate)     return Icon::Gate;
        if (t == HomeTab::Dynamics) return Icon::Compressor;
        if (t == HomeTab::Eq)       return Icon::Eq;
        return Icon::Insert;
    }

    bool isOn (HomeTab t) const
    {
        auto& p = strip->params;
        if (t == HomeTab::Gate)       return p.gate.on.load();
        if (t == HomeTab::Dynamics)   return p.dynamics.on.load();
        if (t == HomeTab::Eq)         return p.eq.on.load();
        if (t == HomeTab::PreInsert)  return p.preInsert.on.load() && p.preInsert.fx.load() >= 0;
        if (t == HomeTab::PostInsert) return p.postInsert.on.load() && (p.postInsert.fx.load() >= 0 || p.postInsert.mode.load() > 0);
        return false;
    }

    void toggle (HomeTab t)
    {
        auto& p = strip->params;
        auto flip = [] (std::atomic<bool>& b) { b = ! b.load(); };
        if (t == HomeTab::Gate)            flip (p.gate.on);
        else if (t == HomeTab::Dynamics)   flip (p.dynamics.on);
        else if (t == HomeTab::Eq)         flip (p.eq.on);
        else if (t == HomeTab::PreInsert)  flip (p.preInsert.on);
        else if (t == HomeTab::PostInsert) flip (p.postInsert.on);
    }

    void chip (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, bool on, juce::Colour onColour = Colours::text) const
    {
        drawChip (g, r, text, on ? onColour : juce::Colour (0xff2c2c2c), on ? juce::Colours::black : Colours::textDim);
    }

    void miniMeter (juce::Graphics& g, juce::Rectangle<float> r, float level, juce::Colour c) const
    {
        const int segments = 10;
        const float h = r.getHeight() / (float) segments;
        for (int i = 0; i < segments; ++i)
        {
            const bool lit = level > (float) i / (float) segments;
            g.setColour (lit ? c : juce::Colour (0xff2c2c2c));
            g.fillRect (r.getX(), r.getBottom() - (float) (i + 1) * h + 1.0f, r.getWidth(), h - 1.5f);
        }
    }

    void paintThumb (juce::Graphics& g, HomeTab tab, juce::Rectangle<float> r) const
    {
        auto& p = strip->params;
        auto& fx = page.ctx.engine.getFx();
        const float chipH = juce::jmin (20.0f, r.getHeight() * 0.46f);

        switch (tab)
        {
            case HomeTab::Home:
                g.setColour (Colours::text);
                g.setFont (juce::FontOptions (juce::jmin (26.0f, r.getHeight() * 0.7f), juce::Font::plain));
                g.drawText ("HOME", r.toNearestInt(), juce::Justification::centred);
                return;

            case HomeTab::Input:
            {
                auto meter = r.removeFromRight (12.0f);
                const float level = MeterComponent::dbToProportion (dsp::gainToDb (page.ctx.meters.get (strip, true, 0)));
                miniMeter (g, meter, level, Colours::meterGreen);
                r.removeFromRight (4.0f);
                const float w = (r.getWidth() - 4.0f) / 3.0f;
                auto row1 = r.removeFromTop (r.getHeight() / 2.0f).withSizeKeepingCentre (r.getWidth(), chipH);
                auto row2 = r.withSizeKeepingCentre (r.getWidth(), chipH);
                const bool ch = strip->getKind() == StripKind::Channel;
                chip (g, row1.removeFromLeft (w), "INV", p.input.invert.load());
                row1.removeFromLeft (2.0f);
                chip (g, row1.removeFromLeft (w), "LC", ch && p.filter.lowCut.load());
                row1.removeFromLeft (2.0f);
                chip (g, row1, "HC", ch && p.filter.highCut.load());
                chip (g, row2.removeFromLeft (w), "TOOL", ch && p.filter.tool.load());
                row2.removeFromLeft (2.0f);
                chip (g, row2.removeFromLeft (w), "DLY", p.input.delayOn.load());
                row2.removeFromLeft (2.0f);
                chip (g, row2, p.stereo.load() || strip->isOutput() ? "ST" : "M", true);
                return;
            }

            case HomeTab::Gate:
            case HomeTab::Dynamics:
            {
                auto& slot = tab == HomeTab::Gate ? p.gate : p.dynamics;
                auto meter = r.removeFromRight (10.0f);
                const float gr = (tab == HomeTab::Gate ? strip->meters.gateReductionDb.load() : strip->meters.dynReductionDb.load()) / 24.0f;
                miniMeter (g, meter, juce::jlimit (0.0f, 1.0f, gr), tab == HomeTab::Gate ? Colours::mute : Colours::amber);
                r.removeFromRight (4.0f);
                chip (g, r.removeFromTop (r.getHeight() * 0.55f).withWidth (juce::jmin (r.getWidth() * 0.6f, 84.0f)),
                      models::model (models::Dynamics, slot.model.load()).token, slot.on.load(), Colours::yellow);
                g.setColour (Colours::textDim);
                g.setFont (juce::FontOptions (juce::jmin (16.0f, r.getHeight() * 0.95f), juce::Font::bold));
                g.drawFittedText (models::model (models::Dynamics, slot.model.load()).name, r.toNearestInt(), juce::Justification::bottomRight, 1);
                return;
            }

            case HomeTab::PreInsert:
            case HomeTab::PostInsert:
            {
                auto& ins = tab == HomeTab::PreInsert ? p.preInsert : p.postInsert;
                const int slot = ins.fx.load();
                const int mode = tab == HomeTab::PostInsert ? ins.mode.load() : 0;
                const juce::String text = mode > 0 ? (mode == 1 ? "AUTO X" : "AUTO Y") : (slot >= 0 ? "FX " + juce::String (slot + 1) : "INS");
                chip (g, r.removeFromTop (r.getHeight() * 0.55f).withWidth (juce::jmin (r.getWidth() * 0.6f, 84.0f)), text, isOn (tab), Colours::text);
                g.setColour (Colours::textDim);
                g.setFont (juce::FontOptions (juce::jmin (16.0f, r.getHeight() * 0.95f), juce::Font::bold));
                auto* plugin = slot >= 0 && mode == 0 ? fx.getPlugin (slot) : nullptr;
                g.drawFittedText (plugin != nullptr ? plugin->getName().toUpperCase() : juce::String(), r.toNearestInt(), juce::Justification::bottomRight, 1);
                return;
            }

            case HomeTab::Eq:
            {
                g.setColour (juce::Colours::black);
                g.fillRect (r);
                if (eqCurve.size() > 1)
                {
                    juce::Path curve, fill;
                    for (size_t i = 0; i < eqCurve.size(); ++i)
                    {
                        const float x = r.getX() + r.getWidth() * (float) i / (float) (eqCurve.size() - 1);
                        const float y = r.getCentreY() - juce::jlimit (-1.0f, 1.0f, eqCurve[i] / 15.0f) * r.getHeight() * 0.45f;
                        if (i == 0)
                        {
                            curve.startNewSubPath (x, y);
                            fill.startNewSubPath (x, r.getCentreY());
                        }
                        curve.lineTo (x, y);
                        fill.lineTo (x, y);
                    }
                    fill.lineTo (r.getRight(), r.getCentreY());
                    fill.closeSubPath();
                    const bool on = p.eq.on.load();
                    g.setColour ((on ? Colours::yellow : Colours::textDim).withAlpha (0.25f));
                    g.fillPath (fill);
                    g.setColour (on ? Colours::yellow : Colours::textDim);
                    g.strokePath (curve, juce::PathStrokeType (1.5f));
                }
                return;
            }

            case HomeTab::Main:
            {
                auto knob = r.removeFromLeft (r.getHeight()).reduced (2.0f);
                const float pos = fader::dbToPos (p.faderDb.load());
                const float a0 = juce::MathConstants<float>::pi * 1.25f, a1 = juce::MathConstants<float>::pi * 2.75f;
                juce::Path track, value;
                const float rad = knob.getWidth() * 0.38f;
                track.addCentredArc (knob.getCentreX(), knob.getCentreY(), rad, rad, 0.0f, a0, a1, true);
                value.addCentredArc (knob.getCentreX(), knob.getCentreY(), rad, rad, 0.0f, a0, a0 + pos * (a1 - a0), true);
                g.setColour (juce::Colour (0xff262626));
                g.strokePath (track, juce::PathStrokeType (knob.getWidth() * 0.2f));
                g.setColour (Colours::lime);
                g.strokePath (value, juce::PathStrokeType (knob.getWidth() * 0.2f));
                r.removeFromLeft (4.0f);
                if (strip->getKind() != StripKind::Main && strip->getKind() != StripKind::Matrix)
                {
                    const float w = juce::jmin (16.0f, (r.getWidth() - 6.0f) / 4.0f);
                    auto row = r.removeFromTop (r.getHeight() / 2.0f).withSizeKeepingCentre (r.getWidth(), juce::jmin (16.0f, chipH));
                    for (int i = 0; i < kNumMains; ++i)
                    {
                        chip (g, row.removeFromLeft (w), juce::String (i + 1), p.mains[i].on.load(), Colours::cyan);
                        row.removeFromLeft (2.0f);
                    }
                }
                g.setColour (Colours::text);
                g.setFont (juce::FontOptions (juce::jmin (16.0f, r.getHeight() * 0.8f), juce::Font::bold));
                g.drawFittedText (fader::text (p.faderDb.load()), r.toNearestInt(), juce::Justification::centredLeft, 1);
                return;
            }

            case HomeTab::Sends:
            {
                auto labels = r.removeFromBottom (12.0f);
                const int n = juce::jmin (16, (int) page.ctx.engine.getBuses().size());
                const float w = r.getWidth() / (float) juce::jmax (1, n);
                for (int i = 0; i < n; ++i)
                {
                    auto* send = strip->sends.find (i);
                    const bool on = send != nullptr && send->enabled.load();
                    const float level = on ? fader::dbToPos (send->levelDb.load()) : 0.0f;
                    auto bar = juce::Rectangle<float> (r.getX() + (float) i * w, r.getY(), w - 1.5f, r.getHeight());
                    g.setColour (juce::Colour (0xff2c2c2c));
                    g.fillRect (bar);
                    g.setColour (Colours::meterGreen);
                    g.fillRect (bar.withTop (bar.getBottom() - bar.getHeight() * level));
                }
                g.setColour (Colours::textDim);
                g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
                g.drawText ("1-8", labels.removeFromLeft (labels.getWidth() / 2).toNearestInt(), juce::Justification::centred);
                g.drawText ("9-16", labels.toNearestInt(), juce::Justification::centred);
                return;
            }

            case HomeTab::Icon:
            case HomeTab::Tags:
            case HomeTab::Filter:
            case HomeTab::Name:
                return;
        }
    }

    void timerCallback() override
    {
        if (strip == nullptr)
            return;
        if (strip->params.processOrder.load() != builtOrder)
            setStrip (strip);

        // The EQ thumbnail's curve, from the engine's own filters.
        std::vector<double> freqs, db (24, 0.0);
        for (int i = 0; i < 24; ++i)
            freqs.push_back (20.0 * std::pow (1000.0, i / 23.0));
        ld::ld_strip_response (strip->getHandle(), 1, freqs.data(), db.data(), 24, 48000.0);
        eqCurve.assign (db.begin(), db.end());
        repaint();
    }

    void showOrderMenu()
    {
        if (strip == nullptr)
            return;
        auto* s = strip;
        juce::PopupMenu menu;
        menu.addSectionHeader ("PROCESS ORDER");
        const auto orders = models::list (models::ProcessOrders);
        for (int i = 0; i < orders.size(); ++i)
        {
            juce::StringArray names;
            for (auto letter : orders[i])
                names.add (letter == 'G' ? "GATE" : letter == 'E' ? "EQ" : letter == 'D' ? "DYN" : "INS");
            menu.addItem (names.joinIntoString ("  >  "), s->isInput(), s->params.processOrder.load() == i,
                          [this, i] { if (strip != nullptr) { strip->params.processOrder = i; setStrip (strip); } });
        }
        juce::PopupMenu taps;
        const auto tapNames = models::list (models::TapPoints);
        for (int i = 0; i < tapNames.size(); ++i)
            taps.addItem (tapNames[i], true, s->params.tap.load() == i, [this, i] { if (strip != nullptr) strip->params.tap = i; });
        menu.addSubMenu ("SEND TAP", taps);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withTargetScreenArea (localAreaToGlobal (wrench)));
    }

    HomePage& page;
    Strip* strip = nullptr;
    std::vector<Row> rows;
    juce::Rectangle<int> wrench;
    std::vector<float> eqCurve;
    int builtOrder = -1;
};

//==============================================================================
HomePage::HomePage (UiContext& c) : ctx (c)
{
    column = std::make_unique<BlockColumn> (*this);
    addAndMakeVisible (*column);

    const std::pair<HomeTab, const char*> tabs[] = { { HomeTab::Home, "OVERVIEW" }, { HomeTab::Icon, "ICON/COLOR" },
                                                     { HomeTab::Name, "NAME" }, { HomeTab::Tags, "TAGS" } };
    for (auto [tab, text] : tabs)
    {
        auto* b = topTabs.add (new WingTab (text));
        const auto t = tab;
        b->onClick = [this, t] { showTab (t); };
        addAndMakeVisible (b);
    }

    safeKey = std::make_unique<IconKey> ("SAFE", Icon::LockOpen, Colours::mute);
    safeKey->setTooltip ("Recall safe: scene recalls skip this strip");
    safeKey->onClick = [this] { if (strip != nullptr) strip->params.recallSafe = ! strip->params.recallSafe.load(); };
    static_cast<IconKey*> (safeKey.get())->isOn = [this] { return strip != nullptr && strip->params.recallSafe.load(); };
    addAndMakeVisible (*safeKey);
}

HomePage::~HomePage() = default;

bool HomePage::isAvailable (HomeTab t, const Strip* s) const
{
    if (s == nullptr)
        return false;
    if (t == HomeTab::Filter || t == HomeTab::Gate)
        return s->getKind() == StripKind::Channel;
    if (t == HomeTab::Sends)
        return s->isInput() || (s->isBus() && s->getId() >= kBusToBusTargets);
    return true;
}

void HomePage::showTab (HomeTab t)
{
    current = t;
    rebuild();
}

void HomePage::refresh()
{
    rebuild();
}

void HomePage::rebuild()
{
    strip = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr;
    if (! isAvailable (current, strip))
        current = HomeTab::Home;

    column->setStrip (strip);
    const bool overview = isOverviewGroup (current);
    const HomeTab group[] = { HomeTab::Home, HomeTab::Icon, HomeTab::Name, HomeTab::Tags };
    for (int i = 0; i < topTabs.size(); ++i)
    {
        topTabs[i]->setVisible (overview && strip != nullptr);
        topTabs[i]->setToggleState (group[i] == current, juce::dontSendNotification);
    }
    safeKey->setVisible (strip != nullptr);

    content.reset();
    if (strip != nullptr)
    {
        auto& s = *strip;
        switch (current)
        {
            case HomeTab::Home:       content = std::make_unique<OverviewScreen> (ctx, s); break;
            case HomeTab::Icon:       content = std::make_unique<IconColorScreen> (ctx, s); break;
            case HomeTab::Name:       content = std::make_unique<NameScreen> (ctx, s); break;
            case HomeTab::Tags:       content = std::make_unique<TagsScreen> (ctx, s); break;
            case HomeTab::Input:      content = makeInputScreen (ctx, s); break;
            case HomeTab::Filter:     content = makeFilterScreen (ctx, s); break;
            case HomeTab::Gate:       content = makeDynamicsScreen (ctx, s, true); break;
            case HomeTab::Eq:         content = makeEqScreen (ctx, s); break;
            case HomeTab::Dynamics:   content = makeDynamicsScreen (ctx, s, false); break;
            case HomeTab::PreInsert:  content = makeInsertScreen (ctx, s, false); break;
            case HomeTab::PostInsert: content = makeInsertScreen (ctx, s, true); break;
            case HomeTab::Main:       content = makeMainScreen (ctx, s); break;
            case HomeTab::Sends:      content = makeSendsScreen (ctx, s); break;
        }
        content->knobsChanged = [this] { notifyKnobsChanged(); };
        addAndMakeVisible (*content);
    }

    resized();
    repaint();
    notifyKnobsChanged();
}

std::vector<KnobBinding> HomePage::getKnobs()
{
    return content != nullptr ? content->knobs() : std::vector<KnobBinding> {};
}

void HomePage::paint (juce::Graphics& g)
{
    g.fillAll (Colours::screen);
    if (strip == nullptr)
    {
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (24.0f, juce::Font::bold));
        g.drawText ("PRESS SEL ON A STRIP", getLocalBounds(), juce::Justification::centred);
        return;
    }

    if (! isOverviewGroup (current))
    {
        // Block screens: a title key like the WING's screen headers.
        auto t = titleArea.toFloat();
        g.setColour (Colours::screenPanel);
        g.fillRect (t.withWidth (juce::jmin (t.getWidth(), 320.0f)));
        g.setColour (Colours::yellow);
        g.fillRect (t.withWidth (juce::jmin (t.getWidth(), 320.0f)).withHeight (3.0f));
        g.setFont (juce::FontOptions (t.getHeight() * 0.62f, juce::Font::bold));
        g.drawText (blockTitle (current), t.withWidth (juce::jmin (t.getWidth(), 320.0f)).toNearestInt(), juce::Justification::centred);
    }
}

void HomePage::resized()
{
    auto b = getLocalBounds();
    column->setBounds (b.removeFromLeft (juce::jlimit (150, 230, b.getWidth() * 22 / 100)));
    b.removeFromLeft (6);

    auto top = b.removeFromTop (juce::jlimit (36, 52, b.getHeight() / 12));
    titleArea = top;
    safeKey->setBounds (top.removeFromRight (juce::jmin (120, top.getWidth() / 6)).reduced (2, 2));
    top.removeFromRight (12);
    const int w = juce::jmin (160, top.getWidth() / 4);
    for (auto* t : topTabs)
        t->setBounds (top.removeFromLeft (w).reduced (2, 0));

    b.removeFromTop (6);
    if (content != nullptr)
        content->setBounds (b);
}

} // namespace livedaw::ui
