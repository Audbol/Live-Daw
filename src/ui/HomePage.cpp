#include "ui/HomePage.h"
#include "ui/Controls.h"
#include "ui/MeterComponent.h"
#include "ui/ModelParams.h"
#include "ui/ResponseGraph.h"

namespace livedaw::ui
{

namespace
{
    constexpr int kRowH = 26;

    const char* tabName (HomeTab t)
    {
        switch (t)
        {
            case HomeTab::Home:       return "HOME";
            case HomeTab::Input:      return "INPUT";
            case HomeTab::Filter:     return "FILTER";
            case HomeTab::Gate:       return "GATE";
            case HomeTab::Eq:         return "EQ";
            case HomeTab::Dynamics:   return "DYNAMICS";
            case HomeTab::PreInsert:  return "PRE INS";
            case HomeTab::PostInsert: return "POST INS";
            case HomeTab::Main:       return "MAIN";
            case HomeTab::Sends:      return "BUS SENDS";
            case HomeTab::Name:       return "ICON/COLOR";
        }
        return "";
    }

    constexpr HomeTab allTabs[] = { HomeTab::Home, HomeTab::Input, HomeTab::Filter, HomeTab::Gate, HomeTab::Eq,
                                    HomeTab::Dynamics, HomeTab::PreInsert, HomeTab::PostInsert, HomeTab::Main,
                                    HomeTab::Sends, HomeTab::Name };

    /** Places components side by side, equally wide. */
    void layoutRow (juce::Rectangle<int> r, std::initializer_list<juce::Component*> comps, int gap = 4)
    {
        const int n = (int) comps.size();
        if (n == 0)
            return;
        const int w = (r.getWidth() - gap * (n - 1)) / n;
        for (auto* c : comps)
        {
            if (c != nullptr)
                c->setBounds (r.removeFromLeft (w));
            r.removeFromLeft (gap);
        }
    }

    juce::StringArray hardwareNames (const juce::StringArray& names, int count)
    {
        juce::StringArray items;
        for (int i = 0; i < count; ++i)
            items.add (juce::String (i + 1) + ": " + (i < names.size() ? names[i] : juce::String ("(not available)")));
        return items;
    }

    juce::StringArray channelNames (MixerEngine& engine)
    {
        juce::StringArray items;
        for (auto& ch : engine.getChannels())
            items.add (describeStrip (*ch));
        return items;
    }

    /** The threshold-like parameter of a dynamics slot ("thr", else the first). */
    KnobBinding thresholdBinding (DynSlotParams& slot, const juce::String& label)
    {
        const auto& spec = models::model (models::Dynamics, slot.model.load());
        if (spec.params.empty())
            return {};
        const int i = juce::jmax (0, spec.find ("thr"));
        auto b = bindModelParam (slot.values[i], spec.params[(size_t) i]);
        b.label = label;
        return b;
    }

    /** A processing block of the HOME overview: yellow when on; click opens its
        screen, right-click (or Ctrl-click) switches it on or off. */
    class BlockButton final : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
    {
    public:
        BlockButton (juce::String t, std::function<bool()> on, std::function<void()> toggle,
                     std::function<juce::String()> info, std::function<void()> open)
            : title (std::move (t)), isOn (std::move (on)), toggleFn (std::move (toggle)), infoFn (std::move (info)),
              openFn (std::move (open))
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            setTooltip (title + ": click to edit, right-click to switch on/off");
            startTimerHz (8);
            timerCallback();
        }

        void paint (juce::Graphics& g) override
        {
            auto b = getLocalBounds().toFloat().reduced (2.0f);
            const bool on = isOn ? isOn() : true;
            g.setColour (on ? Colours::block : Colours::blockOff);
            g.fillRoundedRectangle (b, 4.0f);
            if (isMouseOver())
            {
                g.setColour (juce::Colours::white.withAlpha (0.6f));
                g.drawRoundedRectangle (b, 4.0f, 1.5f);
            }
            g.setColour (on ? juce::Colours::black : Colours::textDim);
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            auto t = b.reduced (4.0f, 3.0f);
            g.drawFittedText (title, t.removeFromTop (16.0f).toNearestInt(), juce::Justification::centred, 1);
            g.setFont (juce::FontOptions (10.5f));
            g.drawFittedText (shownInfo, t.toNearestInt(), juce::Justification::centred, 2);
        }

        void mouseEnter (const juce::MouseEvent&) override { repaint(); }
        void mouseExit (const juce::MouseEvent&) override { repaint(); }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (! getLocalBounds().contains (e.getPosition()))
                return;
            if ((e.mods.isPopupMenu() || e.mods.isCommandDown()) && toggleFn)
                toggleFn();
            else if (openFn)
                openFn();
            timerCallback();
        }

    private:
        void timerCallback() override
        {
            const auto info = infoFn ? infoFn() : juce::String();
            const bool on = isOn ? isOn() : true;
            if (info != shownInfo || on != shownOn)
            {
                shownInfo = info;
                shownOn = on;
                repaint();
            }
        }

        juce::String title, shownInfo;
        bool shownOn = false;
        std::function<bool()> isOn;
        std::function<void()> toggleFn;
        std::function<juce::String()> infoFn;
        std::function<void()> openFn;
    };

    /** A row of toggles editing the bits of an atomic mask (DCAs, mute groups). */
    class MaskButtons final : public juce::Component, private juce::Timer
    {
    public:
        MaskButtons (std::atomic<uint32_t>& m, int count, const juce::String& prefix, juce::Colour on) : mask (m)
        {
            for (int i = 0; i < count; ++i)
            {
                auto* b = buttons.add (new juce::TextButton (prefix + juce::String (i + 1)));
                b->setClickingTogglesState (true);
                b->setColour (juce::TextButton::buttonOnColourId, on);
                b->onClick = [this, i, b]
                {
                    const auto bit = 1u << i;
                    mask = b->getToggleState() ? (mask.load() | bit) : (mask.load() & ~bit);
                };
                addAndMakeVisible (b);
            }
            startTimerHz (8);
            timerCallback();
        }

        void resized() override
        {
            const int n = buttons.size();
            const int columns = n > 8 ? 8 : n;
            const int rows = (n + columns - 1) / columns;
            const int w = getWidth() / columns, h = getHeight() / juce::jmax (1, rows);
            for (int i = 0; i < n; ++i)
                buttons[i]->setBounds ((i % columns) * w, (i / columns) * h, w - 2, h - 2);
        }

    private:
        void timerCallback() override
        {
            for (int i = 0; i < buttons.size(); ++i)
                buttons[i]->setToggleState ((mask.load() >> i) & 1u, juce::dontSendNotification);
        }

        std::atomic<uint32_t>& mask;
        juce::OwnedArray<juce::TextButton> buttons;
    };
} // namespace

//==============================================================================
class HomePage::Tab : public juce::Component
{
public:
    Tab (UiContext& c, Strip& s) : ctx (c), strip (s) {}

    virtual std::vector<KnobBinding> knobs() { return {}; }
    std::function<void()> knobsChanged;

    void paint (juce::Graphics& g) override
    {
        for (auto& [area, title] : boxes)
            drawSectionBox (g, area, title);
    }

protected:
    template <typename T, typename... Args>
    T& make (Args&&... args)
    {
        auto* c = new T (std::forward<Args> (args)...);
        owned.add (c);
        addAndMakeVisible (c);
        return *c;
    }

    juce::Label& label (const juce::String& text, float size = 11.0f, juce::Colour colour = Colours::textDim)
    {
        auto& l = make<juce::Label>();
        l.setText (text, juce::dontSendNotification);
        l.setFont (juce::FontOptions (size, juce::Font::bold));
        l.setColour (juce::Label::textColourId, colour);
        return l;
    }

    DisplayKnob& knob (KnobBinding b)
    {
        auto& k = make<DisplayKnob>();
        k.setCompact (true);
        k.setBinding (std::move (b));
        return k;
    }

    BoolButton& toggle (const juce::String& text, std::atomic<bool>& p, juce::Colour on = Colours::block)
    {
        auto& b = make<BoolButton> (text, on);
        b.bind (&p);
        return b;
    }

    IntCombo& combo (const juce::StringArray& items, std::atomic<int32_t>* p, const juce::String& noneText = {})
    {
        auto& c = make<IntCombo>();
        c.setItems (items, noneText);
        c.bind (p);
        return c;
    }

    /** Lays out a titled box and returns its inner area. */
    juce::Rectangle<int> box (juce::Rectangle<int> area, const juce::String& title)
    {
        boxes.push_back ({ area, title });
        return area.reduced (8, 6).withTrimmedTop (title.isNotEmpty() ? 16 : 0);
    }

    void clearBoxes() { boxes.clear(); }

    UiContext& ctx;
    Strip& strip;
    juce::OwnedArray<juce::Component> owned;
    std::vector<std::pair<juce::Rectangle<int>, juce::String>> boxes;
};

namespace
{
using Tab = HomePage::Tab;

//==============================================================================
class OverviewTab final : public Tab
{
public:
    OverviewTab (UiContext& c, Strip& s)
        : Tab (c, s),
          inMeter ([this] (int ch) { return ctx.meters.get (&strip, true, ch); }, [this] { return strip.getNumChannels(); }),
          outMeter ([this] (int ch) { return ctx.meters.get (&strip, false, ch); }, [this] { return strip.getNumChannels(); }),
          gateGr ([this] { return strip.meters.gateReductionDb.load(); }, 40.0f),
          dynGr ([this] { return strip.meters.dynReductionDb.load(); }, 24.0f)
    {
        auto& p = strip.params;
        auto open = [this] (HomeTab t) { return [this, t] { if (ctx.showHomeTab) ctx.showHomeTab (t); }; };
        auto flip = [] (std::atomic<bool>& b) { return [&b] { b = ! b.load(); }; };

        chain.add (new BlockButton ("INPUT", nullptr, nullptr,
                                    [&p] { return juce::String (p.input.trimDb.load(), 1) + " dB" + (p.input.invert.load() ? "  " + juce::String::fromUTF8 ("\xc3\x98") : juce::String()); },
                                    open (HomeTab::Input)));
        if (strip.getKind() == StripKind::Channel)
            chain.add (new BlockButton ("FILTER", [&p] { return p.filter.lowCut.load() || p.filter.highCut.load() || p.filter.tool.load(); },
                                        flip (p.filter.lowCut),
                                        [&p] { return p.filter.lowCut.load() ? "LC " + formatHz (p.filter.lowCutFreq.load()) : juce::String ("OFF"); },
                                        open (HomeTab::Filter)));

        const auto orderText = models::list (models::ProcessOrders)[juce::jlimit (0, 23, (int) p.processOrder.load())];
        for (auto letter : orderText)
        {
            if (letter == 'G' && strip.getKind() == StripKind::Channel)
                chain.add (new BlockButton ("GATE", [&p] { return p.gate.on.load(); }, flip (p.gate.on),
                                            [&p] { return models::model (models::Dynamics, p.gate.model.load()).token; },
                                            open (HomeTab::Gate)));
            else if (letter == 'E')
                chain.add (new BlockButton ("EQ", [&p] { return p.eq.on.load(); }, flip (p.eq.on),
                                            [this] { return models::model (static_cast<models::Table> (strip.getEqTable()), strip.params.eq.model.load()).token; },
                                            open (HomeTab::Eq)));
            else if (letter == 'D')
                chain.add (new BlockButton ("DYN", [&p] { return p.dynamics.on.load(); }, flip (p.dynamics.on),
                                            [&p] { return models::model (models::Dynamics, p.dynamics.model.load()).token; },
                                            open (HomeTab::Dynamics)));
            else if (letter == 'I')
                chain.add (new BlockButton ("PRE INS", [&p] { return p.preInsert.on.load() && p.preInsert.fx.load() >= 0; },
                                            flip (p.preInsert.on),
                                            [&p] { return p.preInsert.fx.load() >= 0 ? "FX " + juce::String (p.preInsert.fx.load() + 1) : juce::String ("NONE"); },
                                            open (HomeTab::PreInsert)));
        }

        chain.add (new BlockButton ("FADER", [&p] { return ! p.mute.load(); }, flip (p.mute),
                                    [&p] { return fader::text (p.faderDb.load()) + " dB"; }, open (HomeTab::Main)));
        chain.add (new BlockButton ("POST INS", [&p] { return p.postInsert.on.load(); }, flip (p.postInsert.on),
                                    [&p]
                                    {
                                        const int mode = p.postInsert.mode.load();
                                        if (mode > 0)
                                            return juce::String (mode == 1 ? "AUTO X" : "AUTO Y");
                                        return p.postInsert.fx.load() >= 0 ? "FX " + juce::String (p.postInsert.fx.load() + 1) : juce::String ("NONE");
                                    },
                                    open (HomeTab::PostInsert)));
        chain.add (new BlockButton ("MAIN", [&p] { return p.mains[0].on.load() || p.mains[1].on.load() || p.mains[2].on.load() || p.mains[3].on.load(); },
                                    nullptr,
                                    [&p]
                                    {
                                        juce::String text;
                                        for (int i = 0; i < kNumMains; ++i)
                                            if (p.mains[i].on.load())
                                                text << (text.isEmpty() ? "" : " ") << "M" << (i + 1);
                                        return text.isEmpty() ? juce::String ("OFF") : text;
                                    },
                                    open (HomeTab::Main)));

        for (auto* b : chain)
            addAndMakeVisible (b);

        graph.setStrip (&strip, ResponseGraph::Eq);
        addAndMakeVisible (graph);
        for (auto* m : { &inMeter, &outMeter })
        {
            m->setShowScale (false);
            addAndMakeVisible (m);
        }
        addAndMakeVisible (gateGr);
        addAndMakeVisible (dynGr);
        gateGr.setVisible (strip.getKind() == StripKind::Channel);

        orderCaption = &label ("PROCESS ORDER");
        order = &combo (models::list (models::ProcessOrders), &p.processOrder);
        order->onValueChosen = [this] (int v)
        {
            strip.params.processOrder = v;
            if (ctx.showHomeTab)
                juce::MessageManager::callAsync ([&c = ctx] { if (c.showHomeTab) c.showHomeTab (HomeTab::Home); });
        };
        tapCaption = &label ("SEND TAP");
        tap = &combo (models::list (models::TapPoints), &p.tap);
        order->setEnabled (strip.isInput());
        tap->setEnabled (strip.getKind() != StripKind::Main && strip.getKind() != StripKind::Matrix);

        panKnob = &knob (bindPan ("PAN", p.pan));
        widthKnob = &knob (bindLinear ("WIDTH", p.width, -150.0f, 150.0f, "%", 0, 100.0f));
        faderKnob = &knob (bindFader ("FADER", p.faderDb));
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
        k.push_back (bindPan ("PAN", p.pan));
        k.push_back (bindFader ("FADER", p.faderDb));
        return k;
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        auto chainArea = box (b.removeFromTop (78), "SIGNAL CHAIN  (right-click a block to switch it)");
        const int n = chain.size();
        const int w = chainArea.getWidth() / juce::jmax (1, n);
        for (int i = 0; i < n; ++i)
            chain[i]->setBounds (chainArea.getX() + i * w, chainArea.getY(), w - 4, chainArea.getHeight());

        auto bottom = b.removeFromBottom (64);
        auto wrench = box (bottom.removeFromLeft (bottom.getWidth() / 2), "");
        auto row = wrench.removeFromTop (14);
        orderCaption->setBounds (row.removeFromLeft (row.getWidth() / 2));
        tapCaption->setBounds (row);
        layoutRow (wrench.removeFromTop (26), { order, tap }, 6);
        auto knobsArea = box (bottom, "");
        layoutRow (knobsArea, { panKnob, widthKnob, faderKnob }, 4);

        auto right = b.removeFromRight (juce::jmin (220, b.getWidth() / 3));
        auto meters = box (right, "LEVELS");
        auto gr = meters.removeFromBottom (44);
        gateGr.setBounds (gr.removeFromTop (20));
        gr.removeFromTop (4);
        dynGr.setBounds (gr.removeFromTop (20));
        auto m = meters.reduced (10, 2);
        inMeter.setBounds (m.removeFromLeft (m.getWidth() / 2).withSizeKeepingCentre (18, m.getHeight()));
        outMeter.setBounds (m.withSizeKeepingCentre (18, m.getHeight()));

        graph.setBounds (box (b, "EQ").reduced (0, 2));
    }

    void paint (juce::Graphics& g) override
    {
        Tab::paint (g);
        if (! inMeter.getBounds().isEmpty())
        {
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            g.drawText ("IN", inMeter.getBounds().translated (0, -14).withHeight (12).expanded (12, 0), juce::Justification::centred);
            g.drawText ("OUT", outMeter.getBounds().translated (0, -14).withHeight (12).expanded (12, 0), juce::Justification::centred);
        }
    }

private:
    juce::OwnedArray<BlockButton> chain;
    ResponseGraph graph;
    MeterComponent inMeter, outMeter;
    GainReductionMeter gateGr, dynGr;
    juce::Label *orderCaption = nullptr, *tapCaption = nullptr;
    IntCombo *order = nullptr, *tap = nullptr;
    DisplayKnob *panKnob = nullptr, *widthKnob = nullptr, *faderKnob = nullptr;
};

//==============================================================================
class InputTab final : public Tab
{
public:
    InputTab (UiContext& c, Strip& s) : Tab (c, s)
    {
        auto& p = strip.params;
        trim = &knob (bindLinear ("TRIM", p.input.trimDb, -18.0f, 18.0f, " dB", 1, 0.0f));
        balance = &knob (bindLinear ("BALANCE", p.input.balanceDb, -9.0f, 9.0f, " dB", 1, 0.0f));
        invert = &toggle (juce::String::fromUTF8 ("\xc3\x98 INVERT"), p.input.invert);
        delayOn = &toggle (strip.isInput() ? "DELAY" : "DELAY (POST)", p.input.delayOn);
        delayMode = &combo (models::list (models::DelayUnits), &p.input.delayMode);
        delay = &knob (delayBinding());

        const int numIns = juce::jmax (ctx.engine.getHardwareInputNames().size(), 64);
        const int numOuts = juce::jmax (ctx.engine.getHardwareOutputNames().size(), 64);
        const auto ins = hardwareNames (ctx.engine.getHardwareInputNames(), numIns);
        const auto outs = hardwareNames (ctx.engine.getHardwareOutputNames(), numOuts);

        if (strip.isInput())
        {
            stereo = &toggle ("STEREO", p.stereo, Colours::accent);
            inLCaption = &label ("SOURCE L / MONO");
            inL = &combo (ins, &p.inputLeft, "OFF");
            inRCaption = &label ("SOURCE R");
            inR = &combo (ins, &p.inputRight, "NEXT");
        }
        else
        {
            mono = &toggle ("MONO SUM", p.mono, Colours::accent);
        }

        outLCaption = &label (strip.isInput() ? "DIRECT OUT L" : "OUTPUT L");
        outL = &combo (outs, &p.outputLeft, "OFF");
        outRCaption = &label (strip.isInput() ? "DIRECT OUT R" : "OUTPUT R");
        outR = &combo (outs, &p.outputRight, "OFF");

        monitorCaption = &label ("SOLO TO MONITOR");
        monitor = &combo (models::list (models::MonitorBuses), &p.monitorBus);
        soloSafe = &toggle ("SOLO SAFE", p.soloSafe, Colours::solo);
        recallSafe = &toggle ("RECALL SAFE", p.recallSafe, Colours::mute);
    }

    KnobBinding delayBinding()
    {
        auto& in = strip.params.input;
        KnobBinding b;
        b.label = "DELAY";
        auto maxFor = [&in] { static const float max[] = { 150.0f, 500.0f, 500.0f, 24000.0f }; return max[juce::jlimit (0, 3, (int) in.delayMode.load())]; };
        b.get = [&in, maxFor] { return juce::jlimit (0.0f, 1.0f, std::sqrt (in.delay.load() / maxFor())); };
        b.set = [&in, maxFor] (float p) { in.delay = p * p * maxFor(); };
        b.text = [&in]
        {
            static const char* units[] = { " m", " ft", " ms", " smp" };
            const int mode = juce::jlimit (0, 3, (int) in.delayMode.load());
            return juce::String (in.delay.load(), mode == 3 ? 0 : 1) + units[mode];
        };
        b.reset = [&in] { in.delay = 0.0f; };
        return b;
    }

    std::vector<KnobBinding> knobs() override
    {
        auto& p = strip.params;
        return { bindLinear ("TRIM", p.input.trimDb, -18.0f, 18.0f, " dB", 1, 0.0f),
                 bindLinear ("BALANCE", p.input.balanceDb, -9.0f, 9.0f, " dB", 1, 0.0f), delayBinding(), {}, {},
                 bindFader ("FADER", p.faderDb) };
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        auto top = b.removeFromTop (b.getHeight() / 2);

        auto gain = box (top.removeFromLeft (top.getWidth() / 2), "GAIN");
        auto knobs = gain.removeFromLeft (gain.getWidth() * 2 / 3);
        layoutRow (knobs, { trim, balance });
        invert->setBounds (gain.removeFromTop (kRowH).reduced (4, 0));
        gain.removeFromTop (6);
        if (stereo != nullptr)
            stereo->setBounds (gain.removeFromTop (kRowH).reduced (4, 0));
        if (mono != nullptr)
            mono->setBounds (gain.removeFromTop (kRowH).reduced (4, 0));

        auto dly = box (top, "DELAY");
        auto left = dly.removeFromLeft (dly.getWidth() / 2);
        delayOn->setBounds (left.removeFromTop (kRowH).reduced (4, 0));
        left.removeFromTop (6);
        delayMode->setBounds (left.removeFromTop (kRowH).reduced (4, 0));
        delay->setBounds (dly);

        auto routing = box (b.removeFromLeft (b.getWidth() * 2 / 3), "ROUTING");
        auto col1 = routing.removeFromLeft (routing.getWidth() / 2).reduced (4, 0);
        auto col2 = routing.reduced (4, 0);
        auto place = [] (juce::Rectangle<int>& col, juce::Label* l, juce::Component* c)
        {
            if (l == nullptr)
                return;
            l->setBounds (col.removeFromTop (14));
            c->setBounds (col.removeFromTop (kRowH));
            col.removeFromTop (6);
        };
        place (col1, inLCaption, inL);
        place (col1, inRCaption, inR);
        place (col2, outLCaption, outL);
        place (col2, outRCaption, outR);

        auto solo = box (b, "SOLO / SAFES");
        monitorCaption->setBounds (solo.removeFromTop (14));
        monitor->setBounds (solo.removeFromTop (kRowH));
        solo.removeFromTop (6);
        soloSafe->setBounds (solo.removeFromTop (kRowH));
        solo.removeFromTop (4);
        recallSafe->setBounds (solo.removeFromTop (kRowH));
    }

private:
    DisplayKnob *trim = nullptr, *balance = nullptr, *delay = nullptr;
    BoolButton *invert = nullptr, *delayOn = nullptr, *stereo = nullptr, *mono = nullptr, *soloSafe = nullptr, *recallSafe = nullptr;
    IntCombo *delayMode = nullptr, *inL = nullptr, *inR = nullptr, *outL = nullptr, *outR = nullptr, *monitor = nullptr;
    juce::Label *inLCaption = nullptr, *inRCaption = nullptr, *outLCaption = nullptr, *outRCaption = nullptr, *monitorCaption = nullptr;
};

//==============================================================================
class FilterTab final : public Tab
{
public:
    FilterTab (UiContext& c, Strip& s) : Tab (c, s)
    {
        auto& f = strip.params.filter;
        lcOn = &toggle ("LOW CUT", f.lowCut);
        lcFreq = &knob (bindFreq ("FREQ", f.lowCutFreq, 20.0f, 2000.0f, 80.0f));
        lcSlope = &combo (dbOct (models::list (models::LowCutSlopes)), &f.lowCutSlope);
        hcOn = &toggle ("HIGH CUT", f.highCut);
        hcFreq = &knob (bindFreq ("FREQ", f.highCutFreq, 200.0f, 20000.0f, 10000.0f));
        hcSlope = &combo (dbOct (models::list (models::HighCutSlopes)), &f.highCutSlope);
        toolOn = &toggle ("TOOL", f.tool);
        toolModel = &make<ModelSelector>();
        toolModel->bind (models::FilterTools, &f.toolModel, [this] (int m) { strip.setModel (ModelBlock::FilterTool, m); });
        toolParams = &make<ModelParamGrid>();
        toolParams->bind (models::FilterTools, &f.toolModel, f.toolParams);
        toolParams->onModelChanged = [this] { if (knobsChanged) knobsChanged(); };
        graph = &make<ResponseGraph>();
        graph->setStrip (&strip, ResponseGraph::Filter);
    }

    static juce::StringArray dbOct (juce::StringArray items)
    {
        for (auto& i : items)
            i << " dB/oct";
        return items;
    }

    std::vector<KnobBinding> knobs() override
    {
        auto& f = strip.params.filter;
        std::vector<KnobBinding> k { bindFreq ("LO CUT", f.lowCutFreq, 20.0f, 2000.0f, 80.0f),
                                     bindFreq ("HI CUT", f.highCutFreq, 200.0f, 20000.0f, 10000.0f), {} };
        for (auto& b : toolParams->bindings (2))
            k.push_back (b);
        return k;
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        auto controls = b.removeFromBottom (juce::jmin (150, b.getHeight() / 2));
        graph->setBounds (box (b, "RESPONSE"));

        auto lc = box (controls.removeFromLeft (controls.getWidth() / 3), "LOW CUT");
        auto lcLeft = lc.removeFromLeft (lc.getWidth() / 2);
        lcOn->setBounds (lcLeft.removeFromTop (kRowH).reduced (2, 0));
        lcLeft.removeFromTop (6);
        lcSlope->setBounds (lcLeft.removeFromTop (kRowH).reduced (2, 0));
        lcFreq->setBounds (lc);

        auto hc = box (controls.removeFromLeft (controls.getWidth() / 2), "HIGH CUT");
        auto hcLeft = hc.removeFromLeft (hc.getWidth() / 2);
        hcOn->setBounds (hcLeft.removeFromTop (kRowH).reduced (2, 0));
        hcLeft.removeFromTop (6);
        hcSlope->setBounds (hcLeft.removeFromTop (kRowH).reduced (2, 0));
        hcFreq->setBounds (hc);

        auto tool = box (controls, "TOOL FILTER");
        auto top = tool.removeFromTop (kRowH);
        toolOn->setBounds (top.removeFromLeft (70).reduced (2, 0));
        toolModel->setBounds (top.reduced (2, 0));
        tool.removeFromTop (4);
        toolParams->setBounds (tool);
    }

private:
    BoolButton *lcOn = nullptr, *hcOn = nullptr, *toolOn = nullptr;
    DisplayKnob *lcFreq = nullptr, *hcFreq = nullptr;
    IntCombo *lcSlope = nullptr, *hcSlope = nullptr;
    ModelSelector* toolModel = nullptr;
    ModelParamGrid* toolParams = nullptr;
    ResponseGraph* graph = nullptr;
};

//==============================================================================
/** GATE or DYNAMICS slot: model, parameters, sidechain (and crossover). */
class DynTab final : public Tab
{
public:
    DynTab (UiContext& c, Strip& s, bool gateSlot) : Tab (c, s), isGate (gateSlot), slot (gateSlot ? s.params.gate : s.params.dynamics)
    {
        on = &toggle ("ON", slot.on);
        model = &make<ModelSelector>();
        const auto allowed = strip.getKind() == StripKind::Aux && ! isGate ? models::list (models::AuxDynModels) : juce::StringArray();
        model->bind (models::Dynamics, &slot.model,
                     [this] (int m) { strip.setModel (isGate ? ModelBlock::Gate : ModelBlock::Dynamics, m); }, allowed);
        mix = &knob (bindLinear ("MIX", slot.mix, 0.0f, 100.0f, "%", 0, 100.0f));
        gain = &knob (bindLinear ("GAIN", slot.gain, -6.0f, 12.0f, " dB", 1, 0.0f));
        gr = &make<GainReductionMeter> ([this] { return isGate ? strip.meters.gateReductionDb.load() : strip.meters.dynReductionDb.load(); },
                                        isGate ? 40.0f : 24.0f);
        params = &make<ModelParamGrid>();
        params->bind (models::Dynamics, &slot.model, slot.values);
        params->onModelChanged = [this] { if (knobsChanged) knobsChanged(); };

        scCaption = &label ("SIDECHAIN FILTER / SOURCE / TAP");
        scFilter = &combo (models::list (models::SidechainFilters), &slot.scFilter);
        scFreq = &knob (bindFreq ("SC FREQ", slot.scFreq, 20.0f, 20000.0f, 1000.0f));
        scQ = &knob (bindLog ("SC Q", slot.scQ, 0.5f, 10.0f, {}, 1.0f));
        scSource = &combo (channelNames (ctx.engine), &slot.scSource, "SELF");
        scTap = &combo (models::list (models::TapPoints), &slot.scTap);

        if (! isGate)
        {
            xoKind = &combo (models::list (models::XoverTypes), &slot.xoKind);
            xoFreq = &knob (bindFreq ("XO FREQ", slot.xoFreq, 20.0f, 20000.0f, 1000.0f));
            xoQ = &knob (bindLog ("XO Q", slot.xoQ, 0.5f, 10.0f, {}, 1.0f));
        }
    }

    std::vector<KnobBinding> knobs() override { return params->bindings (6); }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        auto header = box (b.removeFromTop (58), isGate ? "GATE MODEL" : "DYNAMICS MODEL");
        on->setBounds (header.removeFromLeft (56).reduced (2, 4));
        gr->setBounds (header.removeFromRight (150).reduced (4, 6));
        model->setBounds (header.reduced (4, 4));

        auto side = b.removeFromRight (juce::jmin (300, b.getWidth() * 2 / 5));
        auto sc = box (side.removeFromTop (isGate ? side.getHeight() : side.getHeight() * 3 / 5), "SIDECHAIN");
        scCaption->setBounds (sc.removeFromTop (14));
        layoutRow (sc.removeFromTop (kRowH), { scFilter, scSource, scTap });
        sc.removeFromTop (4);
        layoutRow (sc.removeFromTop (juce::jmin (90, sc.getHeight())), { scFreq, scQ });
        if (! isGate)
        {
            auto xo = box (side, "CROSSOVER");
            auto row = xo.removeFromTop (kRowH);
            xoKind->setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (2, 0));
            xo.removeFromTop (2);
            layoutRow (xo, { xoFreq, xoQ });
        }

        auto bottom = b.removeFromBottom (92);
        layoutRow (box (bottom, "OUTPUT"), { mix, gain });
        params->setBounds (box (b, "PARAMETERS"));
    }

private:
    bool isGate;
    DynSlotParams& slot;
    BoolButton* on = nullptr;
    ModelSelector* model = nullptr;
    DisplayKnob *mix = nullptr, *gain = nullptr, *scFreq = nullptr, *scQ = nullptr, *xoFreq = nullptr, *xoQ = nullptr;
    GainReductionMeter* gr = nullptr;
    ModelParamGrid* params = nullptr;
    juce::Label* scCaption = nullptr;
    IntCombo *scFilter = nullptr, *scSource = nullptr, *scTap = nullptr, *xoKind = nullptr;
};

//==============================================================================
class EqTab final : public Tab
{
public:
    EqTab (UiContext& c, Strip& s) : Tab (c, s)
    {
        auto& eq = strip.params.eq;
        const auto table = static_cast<models::Table> (strip.getEqTable());
        on = &toggle ("ON", eq.on);
        model = &make<ModelSelector>();
        model->bind (table, &eq.model, [this] (int m) { strip.setModel (ModelBlock::Eq, m); });
        mix = &knob (bindLinear ("MIX", eq.mix, 0.0f, 125.0f, "%", 0, 100.0f));
        graph = &make<ResponseGraph>();
        graph->setStrip (&strip, ResponseGraph::Eq);
        graph->onBandSelected = [this] (int) { if (knobsChanged) knobsChanged(); };
        params = &make<ModelParamGrid>();
        params->bind (table, &eq.model, eq.values);
        params->onModelChanged = [this] { if (knobsChanged) knobsChanged(); };
    }

    std::vector<KnobBinding> knobs() override
    {
        auto& eq = strip.params.eq;
        const auto bands = ResponseGraph::stdBands (strip);
        if (bands.isEmpty())
            return params->bindings (6);

        // WING EQ: the touched band's gain, frequency, Q and shape, plus the mix.
        const auto& spec = models::model (static_cast<models::Table> (strip.getEqTable()), eq.model.load());
        const auto band = bands[juce::jmax (0, graph->getSelectedBand())];
        std::vector<KnobBinding> k;
        for (auto suffix : { "g", "f", "q", "eq" })
        {
            const int i = spec.find (band + suffix);
            if (i >= 0)
            {
                auto b = bindModelParam (eq.values[i], spec.params[(size_t) i]);
                k.push_back (b);
            }
            else
                k.push_back ({});
        }
        k.push_back ({});
        k.push_back (bindLinear ("EQ MIX", eq.mix, 0.0f, 125.0f, "%", 0, 100.0f));
        return k;
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        auto header = box (b.removeFromTop (58), "EQ MODEL");
        on->setBounds (header.removeFromLeft (56).reduced (2, 4));
        mix->setBounds (header.removeFromRight (90));
        model->setBounds (header.reduced (4, 4));
        auto graphArea = b.removeFromTop (b.getHeight() * 45 / 100);
        graph->setBounds (box (graphArea, "RESPONSE  (drag a band, wheel for Q)"));
        params->setBounds (box (b, "PARAMETERS"));
    }

private:
    BoolButton* on = nullptr;
    ModelSelector* model = nullptr;
    DisplayKnob* mix = nullptr;
    ResponseGraph* graph = nullptr;
    ModelParamGrid* params = nullptr;
};

//==============================================================================
class InsertTab final : public Tab, private juce::Timer
{
public:
    InsertTab (UiContext& c, Strip& s, bool postInsert) : Tab (c, s), post (postInsert), ins (post ? s.params.postInsert : s.params.preInsert)
    {
        on = &toggle ("ON", ins.on);
        slotCaption = &label ("FX SLOT");
        slotCombo = &make<IntCombo>();
        slotCombo->bind (&ins.fx);
        fillSlots();

        if (post)
        {
            modeCaption = &label ("MODE (FX or automix group)");
            mode = &combo (models::list (models::PostInsertModes), &ins.mode);
            weight = &knob (bindLinear ("AUTOMIX WEIGHT", ins.weight, -12.0f, 12.0f, " dB", 1, 0.0f));
        }

        status = &label ("", 13.0f, Colours::text);
        status->setJustificationType (juce::Justification::topLeft);
        openButton = &make<juce::TextButton> ("OPEN EDITOR");
        openButton->onClick = [this] { if (ins.fx.load() >= 0 && ctx.openPluginEditor) ctx.openPluginEditor (ins.fx.load()); };
        loadButton = &make<juce::TextButton> ("LOAD / REPLACE...");
        loadButton->onClick = [this] { if (ins.fx.load() >= 0) showFxSlotMenu (ctx, ins.fx.load(), loadButton); };
        effectsButton = &make<juce::TextButton> ("EFFECTS SCREEN");
        effectsButton->onClick = [this] { if (ctx.showScreen) ctx.showScreen (Screen::Effects); };
        mixKnob = &knob ({});
        startTimerHz (4);
        timerCallback();
    }

    void fillSlots()
    {
        juce::StringArray items;
        for (int i = 0; i < kNumFx; ++i)
            items.add (ctx.engine.getFx().describe (i));
        slotCombo->setItems (items, "NONE");
    }

    std::vector<KnobBinding> knobs() override
    {
        std::vector<KnobBinding> k;
        if (const int slot = ins.fx.load(); slot >= 0)
            k.push_back (bindLinear ("FX MIX", ctx.engine.getFx().params (slot).mix, 0.0f, 100.0f, "%", 0, 100.0f));
        else
            k.push_back ({});
        if (post)
            k.push_back (bindLinear ("WEIGHT", ins.weight, -12.0f, 12.0f, " dB", 1, 0.0f));
        return k;
    }

    void timerCallback() override
    {
        const int slot = ins.fx.load();
        auto& fx = ctx.engine.getFx();
        juce::String text;
        if (post && ins.mode.load() > 0)
            text << "Automix group " << (ins.mode.load() == 1 ? "X" : "Y") << ": gain is shared between the channels of the group.";
        else if (slot < 0)
            text << "No FX slot assigned.";
        else
        {
            text << fx.describe (slot) << "\n";
            if (! fx.isLoaded (slot))
                text << "The slot is empty: load a plugin into it.";
            else
            {
                text << "Load " << juce::roundToInt (fx.getLoad (slot) * 100.0f) << "%   users " << fx.getNumUsers (slot);
                if (fx.isFaulted (slot))
                    text << "\nFAULT: auto-bypassed (plugin failed or produced NaN)";
                if (fx.isDoubleAssigned (slot))
                    text << "\nDOUBLE ASSIGNED: another insert uses this slot";
            }
        }
        status->setText (text, juce::dontSendNotification);
        openButton->setEnabled (slot >= 0 && fx.isLoaded (slot));
        loadButton->setEnabled (slot >= 0);

        if (slot != shownSlot)
        {
            shownSlot = slot;
            mixKnob->setBinding (slot >= 0 ? bindLinear ("FX MIX", fx.params (slot).mix, 0.0f, 100.0f, "%", 0, 100.0f) : KnobBinding {});
            mixKnob->setVisible (slot >= 0);
            if (knobsChanged)
                knobsChanged();
        }
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        auto assign = box (b.removeFromTop (post ? 140 : 84), post ? "POST FADER INSERT" : "PRE FADER INSERT");
        if (post)
            weight->setBounds (assign.removeFromRight (110));
        auto row = assign.removeFromTop (14);
        slotCaption->setBounds (row.withTrimmedLeft (66));
        row = assign.removeFromTop (kRowH);
        on->setBounds (row.removeFromLeft (60).reduced (2, 0));
        slotCombo->setBounds (row.reduced (4, 0));
        if (post)
        {
            assign.removeFromTop (6);
            modeCaption->setBounds (assign.removeFromTop (14).withTrimmedLeft (66));
            auto r = assign.removeFromTop (kRowH);
            r.removeFromLeft (64);
            mode->setBounds (r.reduced (4, 0));
        }

        auto slotArea = box (b, "SLOT");
        auto buttons = slotArea.removeFromBottom (kRowH + 4);
        layoutRow (buttons.reduced (0, 2), { openButton, loadButton, effectsButton }, 6);
        mixKnob->setBounds (slotArea.removeFromRight (110).removeFromTop (100));
        status->setBounds (slotArea);
    }

private:
    bool post;
    InsertParams& ins;
    BoolButton* on = nullptr;
    IntCombo *slotCombo = nullptr, *mode = nullptr;
    juce::Label *slotCaption = nullptr, *modeCaption = nullptr, *status = nullptr;
    DisplayKnob *weight = nullptr, *mixKnob = nullptr;
    juce::TextButton *openButton = nullptr, *loadButton = nullptr, *effectsButton = nullptr;
    int shownSlot = -2;
};

//==============================================================================
class MainTab final : public Tab
{
public:
    MainTab (UiContext& c, Strip& s) : Tab (c, s)
    {
        auto& p = strip.params;
        faderKnob = &knob (bindFader ("FADER", p.faderDb));
        panKnob = &knob (bindPan ("PAN", p.pan));
        widthKnob = &knob (bindLinear ("WIDTH", p.width, -150.0f, 150.0f, "%", 0, 100.0f));

        const bool hasMainSends = strip.getKind() != StripKind::Main && strip.getKind() != StripKind::Matrix;
        if (hasMainSends)
        {
            for (int i = 0; i < kNumMains; ++i)
            {
                auto& m = p.mains[i];
                mainRows.push_back ({ &toggle ("M" + juce::String (i + 1), m.on, Colours::accent), &toggle ("PRE", m.pre, Colours::solo),
                                      &knob (bindFader ("LEVEL", m.levelDb)), nullptr });
            }
        }

        if (strip.getKind() != StripKind::Matrix)
        {
            for (int i = 0; i < kNumMatrices; ++i)
            {
                auto& m = p.matrix[i];
                mtxRows.push_back ({ &toggle ("MX" + juce::String (i + 1), m.on, Colours::accent), &toggle ("PRE", m.pre, Colours::solo),
                                     &knob (bindFader ("LVL", m.levelDb)), &knob (bindPan ("PAN", m.pan)) });
            }
        }
        else
        {
            directOn = &toggle ("DIRECT IN", p.directOn, Colours::accent);
            directInvert = &toggle (juce::String::fromUTF8 ("\xc3\x98 INVERT (MIX-MINUS)"), p.directInvert, Colours::mute);
            directPre = &toggle ("PRE FADER", p.directPre, Colours::solo);
            directSource = &combo (channelNames (ctx.engine), &p.directSource, "NONE");
            directLevel = &knob (bindFader ("LEVEL", p.directLevelDb));
        }

        dcas = &make<MaskButtons> (p.dcaMask, kNumDcas, "DCA", Colours::accent);
        muteGroups = &make<MaskButtons> (p.muteGroupMask, kNumMuteGroups, "", Colours::mute);
    }

    std::vector<KnobBinding> knobs() override
    {
        auto& p = strip.params;
        std::vector<KnobBinding> k { bindFader ("FADER", p.faderDb), bindPan ("PAN", p.pan),
                                     bindLinear ("WIDTH", p.width, -150.0f, 150.0f, "%", 0, 100.0f) };
        if (! mainRows.empty())
            for (int i = 0; i < 3; ++i)
                k.push_back (bindFader ("MAIN " + juce::String (i + 1), p.mains[i].levelDb));
        return k;
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        auto groups = b.removeFromBottom (92);
        auto dcaArea = box (groups.removeFromLeft (groups.getWidth() * 2 / 3), "DCA ASSIGN");
        dcas->setBounds (dcaArea);
        muteGroups->setBounds (box (groups, "MUTE GROUPS"));

        auto left = b.removeFromLeft (b.getWidth() * 2 / 5);
        auto out = box (left.removeFromTop (juce::jmin (100, left.getHeight() * 2 / 5)), "OUTPUT");
        layoutRow (out, { faderKnob, panKnob, widthKnob });

        if (! mainRows.empty())
        {
            auto mains = box (left, "MAIN SENDS");
            const int w = mains.getWidth() / kNumMains;
            for (auto& row : mainRows)
            {
                auto cell = mains.removeFromLeft (w).reduced (3, 0);
                row.on->setBounds (cell.removeFromTop (22));
                cell.removeFromTop (2);
                row.pre->setBounds (cell.removeFromTop (22));
                row.level->setBounds (cell);
            }
        }

        if (! mtxRows.empty())
        {
            auto mtx = box (b, "MATRIX SENDS");
            const int columns = 4;
            const int w = mtx.getWidth() / columns, h = mtx.getHeight() / 2;
            for (size_t i = 0; i < mtxRows.size(); ++i)
            {
                auto cell = juce::Rectangle<int> (mtx.getX() + (int) (i % columns) * w, mtx.getY() + (int) (i / columns) * h, w, h).reduced (3);
                auto& row = mtxRows[i];
                auto buttons = cell.removeFromTop (22);
                row.on->setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (1, 0));
                row.pre->setBounds (buttons.reduced (1, 0));
                row.level->setBounds (cell.removeFromLeft (cell.getWidth() / 2));
                row.pan->setBounds (cell);
            }
        }
        else if (directOn != nullptr)
        {
            auto d = box (b, "DIRECT INPUT");
            directOn->setBounds (d.removeFromTop (kRowH).removeFromLeft (d.getWidth() / 2).reduced (2, 0));
            d.removeFromTop (4);
            directSource->setBounds (d.removeFromTop (kRowH).reduced (2, 0));
            d.removeFromTop (4);
            auto r = d.removeFromTop (kRowH);
            directInvert->setBounds (r.removeFromLeft (r.getWidth() * 2 / 3).reduced (2, 0));
            directPre->setBounds (r.reduced (2, 0));
            directLevel->setBounds (d.withSizeKeepingCentre (juce::jmin (100, d.getWidth()), d.getHeight()));
        }
    }

private:
    struct SendRow
    {
        BoolButton* on;
        BoolButton* pre;
        DisplayKnob* level;
        DisplayKnob* pan;
    };

    DisplayKnob *faderKnob = nullptr, *panKnob = nullptr, *widthKnob = nullptr, *directLevel = nullptr;
    std::vector<SendRow> mainRows, mtxRows;
    BoolButton *directOn = nullptr, *directInvert = nullptr, *directPre = nullptr;
    IntCombo* directSource = nullptr;
    MaskButtons *dcas = nullptr, *muteGroups = nullptr;
};

//==============================================================================
class SendsTab final : public Tab
{
public:
    SendsTab (UiContext& c, Strip& s) : Tab (c, s)
    {
        tapCaption = &label ("TAP (pre-fader sends)");
        tap = &combo (models::list (models::TapPoints), &strip.params.tap);

        viewport = &make<juce::Viewport>();
        viewport->setViewedComponent (&rows, false);
        viewport->setScrollBarsShown (true, false);

        for (auto& bus : ctx.engine.getBuses())
        {
            if (bus.get() == &strip)
                continue;
            if (strip.isBus() && ! (strip.getId() >= kBusToBusTargets && bus->getId() < kBusToBusTargets))
                continue;

            auto& send = strip.sends[(size_t) bus->getId()];
            Row row;
            row.bus = bus.get();
            row.name = new juce::Label ({}, describeStrip (*bus));
            row.name->setColour (juce::Label::textColourId, bus->getColour());
            row.name->setFont (juce::FontOptions (12.0f, juce::Font::bold));
            row.on = new BoolButton ("ON", Colours::accent);
            row.on->bind (&send.enabled);
            row.level = new DisplayKnob();
            row.level->setCompact (true);
            row.level->setBinding (bindFader ("LEVEL", send.levelDb));
            row.mode = new IntCombo();
            row.mode->setItems (models::list (models::SendModes));
            row.mode->bind (&send.mode);
            row.pan = new DisplayKnob();
            row.pan->setCompact (true);
            row.pan->setBinding (bindPan ("PAN", send.pan));
            row.link = new BoolButton ("LINK", Colours::solo);
            row.link->bind (&send.panLink);
            for (juce::Component* comp : { (juce::Component*) row.name, (juce::Component*) row.on, (juce::Component*) row.level,
                                           (juce::Component*) row.mode, (juce::Component*) row.pan, (juce::Component*) row.link })
            {
                rowComponents.add (comp);
                rows.addAndMakeVisible (comp);
            }
            sendRows.push_back (row);
        }

        if (sendRows.empty())
            empty = &label (strip.isBus() ? "Buses 1-8 don't send to other buses (buses 9+ can send to buses 1-8)."
                                          : "This strip has no bus sends.",
                            13.0f, Colours::textDim);
    }

    std::vector<KnobBinding> knobs() override
    {
        std::vector<KnobBinding> k;
        for (size_t i = 0; i < sendRows.size() && k.size() < 6; ++i)
        {
            auto b = bindFader (sendRows[i].bus->getLabel(), strip.sends[(size_t) sendRows[i].bus->getId()].levelDb);
            k.push_back (b);
        }
        return k;
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        auto top = box (b.removeFromTop (54), "");
        tapCaption->setBounds (top.removeFromLeft (160));
        tap->setBounds (top.removeFromLeft (140).withSizeKeepingCentre (140, kRowH));

        auto list = box (b, "SENDS TO BUSES");
        viewport->setBounds (list);
        if (empty != nullptr)
            empty->setBounds (list);

        const int w = list.getWidth() - viewport->getScrollBarThickness();
        const int rowH = 62;
        for (size_t i = 0; i < sendRows.size(); ++i)
        {
            auto r = juce::Rectangle<int> (0, (int) i * rowH, w, rowH - 4);
            auto& row = sendRows[i];
            row.name->setBounds (r.removeFromLeft (juce::jmax (120, w / 4)));
            row.on->setBounds (r.removeFromLeft (60).withSizeKeepingCentre (54, kRowH));
            row.level->setBounds (r.removeFromLeft (90));
            row.mode->setBounds (r.removeFromLeft (90).withSizeKeepingCentre (84, kRowH));
            row.pan->setBounds (r.removeFromLeft (80));
            row.link->setBounds (r.removeFromLeft (64).withSizeKeepingCentre (56, kRowH));
        }
        rows.setSize (w, (int) sendRows.size() * rowH);
    }

private:
    struct Row
    {
        Strip* bus;
        juce::Label* name;
        BoolButton* on;
        DisplayKnob* level;
        IntCombo* mode;
        DisplayKnob* pan;
        BoolButton* link;
    };

    juce::Label* tapCaption = nullptr;
    juce::Label* empty = nullptr;
    IntCombo* tap = nullptr;
    juce::Viewport* viewport = nullptr;
    juce::Component rows;
    juce::OwnedArray<juce::Component> rowComponents;
    std::vector<Row> sendRows;
};

//==============================================================================
class NameTab final : public Tab, private juce::Timer
{
public:
    NameTab (UiContext& c, Strip& s) : Tab (c, s)
    {
        nameCaption = &label ("NAME");
        name = &make<juce::TextEditor>();
        name->setText (strip.getName(), false);
        name->setFont (juce::FontOptions (18.0f, juce::Font::bold));
        name->onTextChange = [this] { strip.setName (name->getText()); };
        name->onReturnKey = [this] { unfocusAllComponents(); };

        for (int i = 1; i <= 18; ++i)
        {
            auto* sw = new juce::TextButton (juce::String (i));
            sw->setColour (juce::TextButton::buttonColourId, wingColour (i));
            sw->setColour (juce::TextButton::buttonOnColourId, wingColour (i));
            sw->setColour (juce::TextButton::textColourOffId, juce::Colours::black);
            sw->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
            sw->setTooltip (wingColourName (i));
            sw->onClick = [this, i] { strip.setColourIndex (i); };
            swatches.add (sw);
            addAndMakeVisible (sw);
        }

        icon = &knob (iconBinding());
        led = &toggle ("SCRIBBLE LIGHT", strip.params.led, Colours::accent);
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
        b.text = [&col] { return wingColourName (col.load()); };
        b.steps = 18.0f;
        return b;
    }

    std::vector<KnobBinding> knobs() override { return { colourBinding(), iconBinding() }; }

    void timerCallback() override
    {
        for (int i = 0; i < swatches.size(); ++i)
            swatches[i]->setToggleState (strip.getColourIndex() == i + 1, juce::dontSendNotification);
        if (! name->hasKeyboardFocus (true) && name->getText() != strip.getName())
            name->setText (strip.getName(), false);
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        auto n = box (b.removeFromTop (76), "SCRIBBLE STRIP");
        nameCaption->setBounds (n.removeFromLeft (50));
        led->setBounds (n.removeFromRight (140).withSizeKeepingCentre (130, kRowH));
        name->setBounds (n.withSizeKeepingCentre (n.getWidth() - 8, 32));

        auto colours = box (b.removeFromTop (juce::jmin (150, b.getHeight() * 2 / 3)), "COLOR");
        const int columns = 9;
        const int w = colours.getWidth() / columns, h = colours.getHeight() / 2;
        for (int i = 0; i < swatches.size(); ++i)
            swatches[i]->setBounds (colours.getX() + (i % columns) * w, colours.getY() + (i / columns) * h, w - 4, h - 4);

        auto ic = box (b, "ICON");
        icon->setBounds (ic.removeFromLeft (110));
    }

    void paint (juce::Graphics& g) override
    {
        Tab::paint (g);
        for (auto* sw : swatches)
            if (sw->getToggleState())
            {
                g.setColour (juce::Colours::white);
                g.drawRect (sw->getBounds().expanded (2), 2);
            }
    }

private:
    juce::Label* nameCaption = nullptr;
    juce::TextEditor* name = nullptr;
    juce::OwnedArray<juce::TextButton> swatches;
    DisplayKnob* icon = nullptr;
    BoolButton* led = nullptr;
};

} // namespace

//==============================================================================
HomePage::HomePage (UiContext& c) : ctx (c)
{
    for (auto t : allTabs)
    {
        auto* b = tabButtons.add (new juce::TextButton (tabName (t)));
        b->setClickingTogglesState (false);
        b->setColour (juce::TextButton::buttonOnColourId, Colours::accent);
        b->setColour (juce::TextButton::buttonColourId, Colours::screenPanel);
        b->onClick = [this, t] { showTab (t); };
        addAndMakeVisible (b);
    }
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

    for (int i = 0; i < tabButtons.size(); ++i)
    {
        tabButtons[i]->setEnabled (isAvailable (allTabs[i], strip));
        tabButtons[i]->setToggleState (allTabs[i] == current, juce::dontSendNotification);
    }

    content.reset();
    if (strip != nullptr)
    {
        auto& s = *strip;
        switch (current)
        {
            case HomeTab::Home:       content = std::make_unique<OverviewTab> (ctx, s); break;
            case HomeTab::Input:      content = std::make_unique<InputTab> (ctx, s); break;
            case HomeTab::Filter:     content = std::make_unique<FilterTab> (ctx, s); break;
            case HomeTab::Gate:       content = std::make_unique<DynTab> (ctx, s, true); break;
            case HomeTab::Eq:         content = std::make_unique<EqTab> (ctx, s); break;
            case HomeTab::Dynamics:   content = std::make_unique<DynTab> (ctx, s, false); break;
            case HomeTab::PreInsert:  content = std::make_unique<InsertTab> (ctx, s, false); break;
            case HomeTab::PostInsert: content = std::make_unique<InsertTab> (ctx, s, true); break;
            case HomeTab::Main:       content = std::make_unique<MainTab> (ctx, s); break;
            case HomeTab::Sends:      content = std::make_unique<SendsTab> (ctx, s); break;
            case HomeTab::Name:       content = std::make_unique<NameTab> (ctx, s); break;
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
        g.setFont (juce::FontOptions (16.0f));
        g.drawText ("Press SEL on a strip", getLocalBounds().withTrimmedLeft (100), juce::Justification::centred);
    }
}

void HomePage::resized()
{
    auto b = getLocalBounds();
    auto column = b.removeFromLeft (96).reduced (4, 4);
    const int h = juce::jmin (34, column.getHeight() / juce::jmax (1, tabButtons.size()));
    for (auto* t : tabButtons)
        t->setBounds (column.removeFromTop (h).reduced (0, 2));

    if (content != nullptr)
        content->setBounds (b.reduced (2));
}

} // namespace livedaw::ui
