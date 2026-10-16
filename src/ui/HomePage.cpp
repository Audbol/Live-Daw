#include "ui/HomePage.h"
#include "dsp/DspUtils.h"
#include "ui/Faceplate.h"
#include "ui/Icons.h"
#include "ui/MeterComponent.h"
#include "ui/ResponseGraph.h"

namespace livedaw::ui
{

using namespace home;

namespace
{
    /** The source tile of the INPUT panel: the source's name on its colour, its
        icon and stereo-link glyph, and the port (olive for a stereo source,
        blue for a mono one). Click to open the INPUT screen. */
    class SourceTile final : public juce::Component, private juce::Timer
    {
    public:
        SourceTile (UiContext& c, Strip& s, std::function<void()> open) : ctx (c), strip (s), onOpen (std::move (open))
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            startTimerHz (4);
            timerCallback();
        }

        static juce::String portName (UiContext& ctx, const Strip& strip)
        {
            const int in = strip.params.inputLeft.load();
            if (in < 0)
                return "NONE";
            juce::ignoreUnused (ctx);
            if (strip.params.stereo.load())
            {
                const int right = strip.params.inputRight.load() >= 0 ? strip.params.inputRight.load() : in + 1;
                return "IN " + juce::String (in + 1) + "/" + juce::String (right + 1);
            }
            return "IN " + juce::String (in + 1);
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (juce::Colours::black);
            g.fillRect (r);
            auto inner = r.reduced (2.0f);
            auto header = inner.removeFromTop (juce::jlimit (12.0f, 22.0f, inner.getHeight() * 0.2f));
            const auto colour = strip.getColour();
            g.setColour (colour);
            g.fillRect (header);
            g.setColour (colour.getPerceivedBrightness() > 0.45f ? juce::Colours::black : juce::Colours::white);
            g.setFont (juce::FontOptions (header.getHeight() * 0.85f, juce::Font::bold));
            g.drawFittedText (stripName (strip).toUpperCase(), header.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.7f);

            g.setColour (juce::Colour (0xff3a3a3a));
            g.fillRect (inner);
            auto label = inner.removeFromBottom (juce::jlimit (12.0f, 22.0f, inner.getHeight() * 0.26f));
            auto body = inner.reduced (4.0f, 3.0f);
            drawIcon (g, stripIcon (strip.getKind()), body.removeFromLeft (body.getWidth() * 0.55f), Colours::textDim);
            // The stereo-link glyph: a white pill, the knob right when linked.
            auto pill = body.removeFromTop (body.getHeight() * 0.45f).withSizeKeepingCentre (juce::jmin (body.getWidth() * 0.8f, 26.0f), juce::jmin (body.getHeight() * 0.3f, 12.0f));
            g.setColour (Colours::text);
            g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
            const bool st = strip.params.stereo.load();
            g.setColour (juce::Colours::black);
            const float d = pill.getHeight() * 0.7f;
            g.fillEllipse ((st ? pill.getRight() - d - 1.5f : pill.getX() + 1.5f), pill.getCentreY() - d * 0.5f, d, d);
            g.setColour (Colours::textDim);
            g.drawRect (body.withSizeKeepingCentre (juce::jmin (body.getWidth() * 0.6f, 18.0f), juce::jmin (body.getHeight() * 0.6f, 12.0f)), 1.0f);

            g.setColour (juce::Colour (0xff262626));
            g.fillRect (label);
            g.setColour (st ? colours::stereo : colours::mono);
            g.setFont (juce::FontOptions (label.getHeight() * 0.8f, juce::Font::bold));
            g.drawFittedText (shownPort, label.toNearestInt(), juce::Justification::centred, 1, 0.7f);
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (getLocalBounds().contains (e.getPosition()) && onOpen)
                onOpen();
        }

    private:
        void timerCallback() override
        {
            const auto now = portName (ctx, strip) + stripName (strip) + juce::String (strip.getColourIndex());
            if (now != shown)
            {
                shown = now;
                shownPort = portName (ctx, strip);
                repaint();
            }
        }

        UiContext& ctx;
        Strip& strip;
        std::function<void()> onOpen;
        juce::String shown, shownPort;
    };

    /** "Small graphic representations of the active signal processors" (p. 31):
        the faceplate of an analog model or effect, the transfer curve of the
        WING's own dynamics. Click one to open its slot. */
    class ProcessorThumbs final : public juce::Component, private juce::Timer
    {
    public:
        ProcessorThumbs (UiContext& c, Strip& s) : ctx (c), strip (s)
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            startTimerHz (4);
        }

        struct Item
        {
            HomeTab tab;
            juce::Rectangle<float> area;
        };

        std::vector<HomeTab> active() const
        {
            auto& p = strip.params;
            std::vector<HomeTab> tabs;
            juce::String order = strip.getKind() == StripKind::Channel
                                     ? models::list (models::ProcessOrders)[juce::jlimit (0, 23, (int) p.processOrder.load())]
                                     : juce::String ("GDI");
            for (auto letter : order)
            {
                if (letter == 'G' && strip.getKind() == StripKind::Channel && p.gate.on.load())
                    tabs.push_back (HomeTab::Gate);
                else if (letter == 'D' && p.dynamics.on.load())
                    tabs.push_back (HomeTab::Dynamics);
                else if (letter == 'I' && p.preInsert.on.load() && p.preInsert.fx.load() >= 0 && ctx.engine.getFx().isLoaded (p.preInsert.fx.load()))
                    tabs.push_back (HomeTab::PreInsert);
            }
            if (strip.hasPostInsert() && p.postInsert.on.load() && p.postInsert.mode.load() == 0 && p.postInsert.fx.load() >= 0
                && ctx.engine.getFx().isLoaded (p.postInsert.fx.load()))
                tabs.push_back (HomeTab::PostInsert);
            return tabs;
        }

        void layoutItems()
        {
            items.clear();
            const auto tabs = active();
            if (tabs.empty())
                return;
            auto r = getLocalBounds().toFloat();
            const int n = (int) tabs.size();
            const int columns = n > 2 ? 2 : n;
            const int rows = (n + columns - 1) / columns;
            const float cw = r.getWidth() / (float) columns, ch = juce::jmin (r.getHeight() / (float) rows, cw / 2.2f);
            for (int i = 0; i < n; ++i)
            {
                const bool curve = (tabs[(size_t) i] == HomeTab::Gate && isNativeDynamics (strip.params.gate.model.load()))
                                   || (tabs[(size_t) i] == HomeTab::Dynamics && isNativeDynamics (strip.params.dynamics.model.load()));
                auto cell = juce::Rectangle<float> (r.getX() + (float) (i % columns) * cw, r.getY() + (float) (i / columns) * ch, cw, ch).reduced (5.0f, 4.0f);
                items.push_back ({ tabs[(size_t) i], curve ? cell.withSizeKeepingCentre (juce::jmin (cell.getWidth(), cell.getHeight() * 1.5f), cell.getHeight())
                                                           : cell.withSizeKeepingCentre (juce::jmin (cell.getWidth(), cell.getHeight() * 3.0f), cell.getHeight()) });
            }
        }

        void paint (juce::Graphics& g) override
        {
            auto& p = strip.params;
            auto& fx = ctx.engine.getFx();
            for (auto& item : items)
            {
                switch (item.tab)
                {
                    case HomeTab::Gate:
                    case HomeTab::Dynamics:
                    {
                        auto& slot = item.tab == HomeTab::Gate ? p.gate : p.dynamics;
                        const int model = slot.model.load();
                        if (isNativeDynamics (model))
                            paintTransferCurve (g, item.area, slot, item.tab == HomeTab::Gate, true);
                        else
                            Faceplate::paintThumbnail (g, item.area, models::Dynamics, model, {});
                        break;
                    }
                    case HomeTab::PreInsert:
                    case HomeTab::PostInsert:
                    {
                        const int slot = (item.tab == HomeTab::PreInsert ? p.preInsert : p.postInsert).fx.load();
                        auto* plugin = fx.getPlugin (slot);
                        Faceplate::paintThumbnail (g, item.area, models::Fx, fx.getModel (slot), plugin != nullptr ? plugin->getName().toUpperCase() : juce::String());
                        break;
                    }
                    case HomeTab::Home:
                    case HomeTab::Icon:
                    case HomeTab::Tags:
                    case HomeTab::Input:
                    case HomeTab::Filter:
                    case HomeTab::Eq:
                    case HomeTab::Main:
                    case HomeTab::Sends:
                    case HomeTab::Name:
                        break;
                }
            }
        }

        void resized() override { layoutItems(); }

        void mouseUp (const juce::MouseEvent& e) override
        {
            for (auto& item : items)
                if (item.area.contains (e.position) && ctx.showHomeTab)
                {
                    ctx.showHomeTab (item.tab);
                    return;
                }
        }

    private:
        void timerCallback() override
        {
            auto& p = strip.params;
            const auto now = std::make_tuple (active(), p.gate.model.load(), p.dynamics.model.load(), p.preInsert.fx.load(), p.postInsert.fx.load());
            if (now != shown)
            {
                shown = now;
                layoutItems();
            }
            repaint();
        }

        UiContext& ctx;
        Strip& strip;
        std::vector<Item> items;
        std::tuple<std::vector<HomeTab>, int, int, int, int> shown;
    };

    /** The MAIN panel's column of mains 1-4: lit blue while the strip feeds that
        main; click to switch the send. */
    class MainSquares final : public juce::Component, private juce::Timer
    {
    public:
        explicit MainSquares (StripParams& p) : params (p) { startTimerHz (6); }

        void paint (juce::Graphics& g) override
        {
            const float h = (float) getHeight() / (float) kNumMains;
            const float s = juce::jmin (h - 3.0f, (float) getWidth());
            for (int i = 0; i < kNumMains; ++i)
            {
                auto r = juce::Rectangle<float> (0.0f, (float) i * h, (float) getWidth(), h).withSizeKeepingCentre (s, s);
                const bool on = params.mains[i].on.load();
                g.setColour (on ? colours::fader : juce::Colour (0xff262626));
                g.fillRect (r);
                g.setColour (on ? juce::Colours::black : Colours::textDim);
                g.setFont (juce::FontOptions (s * 0.8f, juce::Font::bold));
                g.drawText (juce::String (i + 1), r.toNearestInt(), juce::Justification::centred);
            }
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            const int i = juce::jlimit (0, kNumMains - 1, e.y * kNumMains / juce::jmax (1, getHeight()));
            params.mains[i].on = ! params.mains[i].on.load();
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
class OverviewScreen final : public HomeScreen, private juce::Timer
{
public:
    OverviewScreen (UiContext& c, Strip& s)
        : HomeScreen (c, s),
          meter ([this] (int ch) { return ctx.meters.get (&strip, false, ch); }, [this] { return strip.getNumChannels(); })
    {
        auto& p = strip.params;
        auto open = [this] (HomeTab t) { return [this, t] { if (ctx.showHomeTab) ctx.showHomeTab (t); }; };
        const bool outputKind = strip.isOutput();

        if (outputKind)
            feed = &make<FeedGrid> (ctx, strip, [this] { if (openFeedConfig) openFeedConfig(); });
        else
        {
            source = &make<SourceTile> (ctx, strip, open (HomeTab::Input));
            // The source mute belongs to the Sources (manual 5.4); the console's
            // sources are not modelled, so the key is shown off.
            sourceMute = &key ("MUTE");
            sourceMute->setEnabled (false);
            sourceMute->setTooltip ("Source mute (sources are not modelled: the channel MUTE is in the LEVEL panel)");
        }
        trim = &make<KnobBox> (wingTrim ("TRIM", p.input.trimDb));
        processors = &make<ProcessorThumbs> (ctx, strip);

        if (strip.getKind() == StripKind::Channel)
        {
            loCut = &key ("LO CUT", Icon::LowCut, colours::eqOn);
            bindKey (*loCut, p.filter.lowCut);
            hiCut = &key ("HI CUT", Icon::HighCut, colours::eqOn);
            bindKey (*hiCut, p.filter.highCut);
            tool = &key (toolFilterName (p.filter.toolModel.load()), toolFilterIcon (p.filter.toolModel.load()), colours::eqOn);
            bindKey (*tool, p.filter.tool);
        }

        mute = &key ("MUTE", std::nullopt, Colours::mute);
        bindKey (*mute, p.mute);
        solo = &key ("SOLO", std::nullopt, Colours::solo);
        bindKey (*solo, p.solo);
        meter.showScale = true;
        addAndMakeVisible (meter);
        fader = &make<ScreenFader> (p.faderDb);

        graph = &make<ResponseGraph>();
        graph->setStrip (&strip, ResponseGraph::Eq);
        graph->setInterceptsMouseClicks (false, false);

        pan = &make<PanWidthDisplay> (p.pan, p.width, false);
        pan->onClick = open (HomeTab::Main);
        if (strip.getKind() != StripKind::Main && strip.getKind() != StripKind::Matrix)
            mains = &make<MainSquares> (p);
        soloA = &key ({}, Icon::Headphones, colours::soloBus);
        soloA->isOn = [&p] { return p.monitorBus.load() != 1; };
        soloA->onClick = [&p] { p.monitorBus = p.monitorBus.load() == 2 ? 1 : (p.monitorBus.load() == 1 ? 2 : 0); };
        soloA->setTooltip ("Solo bus A (phones)");
        soloB = &key ({}, Icon::Speaker, colours::soloBus);
        soloB->isOn = [&p] { return p.monitorBus.load() != 0; };
        soloB->onClick = [&p] { p.monitorBus = p.monitorBus.load() == 2 ? 0 : (p.monitorBus.load() == 0 ? 2 : 1); };
        soloB->setTooltip ("Solo bus B (speakers)");
        soloBusLabel = &label ("SOLO BUS", 14.0f, Colours::text);
        soloBusLabel->setJustificationType (juce::Justification::centred);
        startTimer (400);
    }

    bool hasSettings() const override { return true; }

    std::vector<HomeKnob> knobs() override
    {
        auto& p = strip.params;
        return { {}, { wingTrim ("TRIM", p.input.trimDb), thumbStyle (trim) }, {}, {},
                 { wingWidth ("WIDTH", p.width), widthStyle (pan) }, { wingPan ("PAN", p.pan), thumbStyle (pan) } };
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        // Fig. 21: INPUT about 60 % of the width, FILTER 15 %, LEVEL 22 % (full
        // height); EQ under INPUT, MAIN under FILTER; equal halves.
        levelArea = b.removeFromRight (b.getWidth() * 22 / 100);
        auto top = b.removeFromTop (b.getHeight() / 2);
        // FILTER / MAIN keep room for their titles on small screens.
        const int sideW = juce::jmax (top.getWidth() * 21 / 100, juce::jmin (112, top.getWidth() / 3));
        inputArea = top.removeFromLeft (top.getWidth() - sideW);
        filterArea = top;
        eqArea = b.removeFromLeft (b.getWidth() - sideW);
        mainArea = b;

        // INPUT: source tile (or feed grid), trim, processor thumbnails.
        auto in = box (inputArea, "INPUT", true);
        if (feed != nullptr)
        {
            feed->setBounds (in.removeFromLeft (in.getWidth() * 50 / 100));
            in.removeFromLeft (8);
        }
        else
        {
            auto tileCol = in.removeFromLeft (juce::jmin (in.getWidth() / 4, juce::jmax (60, in.getHeight() * 7 / 10)));
            source->setBounds (tileCol.removeFromTop (tileCol.getHeight() * 64 / 100));
            tileCol.removeFromTop (juce::jmax (4, tileCol.getHeight() / 5));
            sourceMute->setBounds (tileCol.removeFromTop (juce::jmin (32, tileCol.getHeight())).withSizeKeepingCentre (juce::jmin (64, tileCol.getWidth()), juce::jmin (30, tileCol.getHeight())));
            in.removeFromLeft (10);
        }
        auto trimRow = in.removeFromTop (juce::jlimit (28, 52, in.getHeight() * 30 / 100));
        trim->setBounds (trimRow.removeFromRight (juce::jmin (trimRow.getWidth(), juce::jmax (110, trimRow.getHeight() * 3))));
        in.removeFromTop (4);
        processors->setBounds (in);

        // FILTER: low cut, high cut and the tool filter (input channels only).
        if (loCut != nullptr)
        {
            auto f = box (filterArea, "FILTER", true);
            const int kh = juce::jmin (40, (f.getHeight() - 8) / 3);
            for (auto* k : { loCut, hiCut, tool })
            {
                k->setBounds (f.removeFromTop (kh));
                f.removeFromTop (4);
            }
        }

        // LEVEL: MUTE top left, the meter, SOLO bottom left, the fader.
        auto l = box (levelArea, "LEVEL");
        auto meterCol = l.removeFromLeft (l.getWidth() * 45 / 100);
        const int kh = juce::jlimit (22, 34, l.getHeight() / 9);
        mute->setBounds (meterCol.removeFromTop (kh).reduced (1, 0));
        solo->setBounds (meterCol.removeFromBottom (kh).reduced (1, 0));
        meter.setBounds (meterCol.reduced (0, 6));
        fader->setBounds (l.reduced (0, 2));

        // EQ graph.
        graph->setBounds (box (eqArea, "EQ", true));

        // MAIN: pan/width display, mains 1-4, solo bus keys.
        auto m = box (mainArea, "MAIN", true);
        auto soloRow = m.removeFromBottom (juce::jlimit (20, 34, m.getHeight() / 4));
        soloBusLabel->setBounds (m.removeFromBottom (juce::jlimit (12, 18, m.getHeight() / 6)));
        soloA->setBounds (soloRow.removeFromLeft (soloRow.getWidth() / 2).reduced (2, 0));
        soloB->setBounds (soloRow.reduced (2, 0));
        if (mains != nullptr)
            mains->setBounds (m.removeFromRight (juce::jmin (16, m.getWidth() / 5)).reduced (0, 2));
        pan->setBounds (m.reduced (2));
    }

    void paint (juce::Graphics& g) override
    {
        HomeScreen::paint (g);
        // An analog EQ shows its faceplate at the right end of the EQ header.
        const auto& eqModel = models::model (static_cast<models::Table> (strip.getEqTable()), strip.params.eq.model.load());
        if (eqModel.token != "STD")
        {
            auto header = eqArea.reduced (3).removeFromTop (kPanelHeaderHeight).toFloat();
            auto plate = header.removeFromRight (header.getWidth() * 0.32f).withTrimmedRight (26.0f).reduced (0.0f, 3.0f);
            Faceplate::paintThumbnail (g, plate, static_cast<models::Table> (strip.getEqTable()), strip.params.eq.model.load(), {});
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        // Panel headers with a chevron open their screen.
        const auto pos = e.getPosition();
        auto header = [] (juce::Rectangle<int> r) { return r.reduced (3).removeFromTop (kPanelHeaderHeight); };
        HomeTab target = HomeTab::Home;
        if (header (inputArea).contains (pos))
            target = HomeTab::Input;
        else if (header (filterArea).contains (pos) && loCut != nullptr)
            target = HomeTab::Filter;
        else if (header (eqArea).contains (pos))
            target = HomeTab::Eq;
        else if (header (mainArea).contains (pos))
            target = HomeTab::Main;
        if (target != HomeTab::Home && ctx.showHomeTab)
            ctx.showHomeTab (target);
    }

private:
    void timerCallback() override
    {
        // The FILTER panel's third key is named after the current tool filter.
        if (tool != nullptr && tool->getButtonText() != toolFilterName (strip.params.filter.toolModel.load()))
        {
            tool->setButtonText (toolFilterName (strip.params.filter.toolModel.load()));
            tool->setIcon (toolFilterIcon (strip.params.filter.toolModel.load()));
        }
        if (strip.params.eq.model.load() != shownEq)
        {
            shownEq = strip.params.eq.model.load();
            repaint();
        }
    }

    SourceTile* source = nullptr;
    FeedGrid* feed = nullptr;
    IconKey *sourceMute = nullptr, *loCut = nullptr, *hiCut = nullptr, *tool = nullptr;
    IconKey *solo = nullptr, *mute = nullptr, *soloA = nullptr, *soloB = nullptr;
    KnobBox* trim = nullptr;
    ProcessorThumbs* processors = nullptr;
    LedMeter meter;
    ScreenFader* fader = nullptr;
    ResponseGraph* graph = nullptr;
    PanWidthDisplay* pan = nullptr;
    MainSquares* mains = nullptr;
    juce::Label* soloBusLabel = nullptr;
    juce::Rectangle<int> inputArea, filterArea, levelArea, eqArea, mainArea;
    int shownEq = -1;
};

//==============================================================================
/** ICON/COLOR: the strip's colour, icon and the light bar above the scribble strip. */
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
        light = &key ("LIGHT", std::nullopt, Colours::yellow);
        bindKey (*light, strip.params.led);
        light->setTooltip ("The light bar above the scribble strip");
        if (strip.isInput())
        {
            // Needs the Sources (manual 5.4): not modelled.
            linkCust = &key ("LINK CUSTOMIZATION TO SOURCE");
            linkCust->twoLines = true;
            linkCust->setEnabled (false);
        }
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

    std::vector<HomeKnob> knobs() override
    {
        return { { colourBinding(), styleOf (KnobStyle::Bar::Segments, KnobColours::yellow, nullptr, 18) },
                 { iconBinding(), styleOf (KnobStyle::Bar::Fill, KnobColours::white, icon) } };
    }

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
        auto sc = box (rest, "SCRIBBLE STRIP");
        light->setBounds (sc.removeFromTop (juce::jmin (36, sc.getHeight() / 2)).withSizeKeepingCentre (juce::jmin (120, sc.getWidth()), juce::jmin (34, sc.getHeight() / 2)));
        sc.removeFromTop (8);
        if (linkCust != nullptr)
            linkCust->setBounds (sc.removeFromTop (juce::jmin (44, sc.getHeight())).withSizeKeepingCentre (juce::jmin (200, sc.getWidth()), juce::jmin (44, sc.getHeight())));
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
    IconKey *light = nullptr, *linkCust = nullptr;
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
        hint = &label ("The name shows on the scribble strip, the channel tab and in every list.", 15.0f);
        clearKey = &key ("CLEAR");
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
        auto n = box (getLocalBounds().removeFromTop (juce::jmin (200, getHeight())), "NAME");
        auto row = n.removeFromTop (juce::jmin (56, n.getHeight() / 2));
        clearKey->setBounds (row.removeFromRight (100).reduced (4, 8));
        editor->setBounds (row.reduced (4, 4));
        n.removeFromTop (8);
        hint->setBounds (n.removeFromTop (24));
    }

private:
    juce::TextEditor* editor = nullptr;
    juce::Label* hint = nullptr;
    IconKey* clearKey = nullptr;
};

/** TAGS: DCA groups D1-D16 and the 8 mute groups. */
class TagsScreen final : public HomeScreen
{
public:
    TagsScreen (UiContext& c, Strip& s) : HomeScreen (c, s)
    {
        dcas = &make<MaskButtons> (strip.params.dcaMask, kNumDcas, "D", Colours::cyan);
        mutes = &make<MaskButtons> (strip.params.muteGroupMask, kNumMuteGroups, "", Colours::mute);
        // Talkback tags need the talkback system (not modelled).
        for (auto* t : { "TALK ON", "TALK SOLO", "TALK A ON", "TALK B ON", "TALK A SOLO", "TALK B SOLO" })
        {
            auto& k = key (t);
            k.setEnabled (false);
            talk.push_back (&k);
        }
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        dcas->setBounds (box (b.removeFromTop (b.getHeight() * 42 / 100), "DCA GROUPS"));
        auto m = box (b.removeFromTop (b.getHeight() / 2), "MUTE GROUPS");
        mutes->setBounds (m.withHeight (juce::jmin (48, m.getHeight())));
        auto t = box (b, "TALKBACK");
        const int w = t.getWidth() / 6;
        for (auto* k : talk)
            k->setBounds (t.removeFromLeft (w).withHeight (juce::jmin (40, t.getHeight())).reduced (2, 0));
    }

private:
    MaskButtons *dcas = nullptr, *mutes = nullptr;
    std::vector<IconKey*> talk;
};
} // namespace

//==============================================================================
/** The HOME toolbar: HOME, then a thumbnail per processing slot at fixed rows,
    the fader bar with the TAP point, MAIN and SENDS; on Channel Home a key
    beside each slot and the wrench. */
class HomePage::BlockColumn final : public juce::Component, private juce::Timer
{
public:
    explicit BlockColumn (HomePage& p) : page (p) { startTimerHz (8); }

    void setStrip (Strip* s)
    {
        strip = s;
        builtOrder = s != nullptr ? s->params.processOrder.load() : -1;
        if (strip == nullptr)
            editing = false;
        resized();
        repaint();
    }

    void setShowKeys (bool k)
    {
        if (keys != k)
        {
            keys = k;
            editing = editing && k;
            resized();
            repaint();
        }
    }

    void resized() override
    {
        rows.clear();
        tapY.clear();
        if (strip == nullptr)
            return;

        auto b = getLocalBounds();
        // Fig. 21: thumbnails about 52 % of the toolbar, a gap with the key's
        // pointer, square keys about 36 %.
        const int thumbW = keys ? b.getWidth() * 52 / 100 : b.getWidth();
        const int keyW = keys ? b.getWidth() * 36 / 100 : 0;
        const int keyX = b.getRight() - keyW;
        // Nine rows and the thin fader bar.
        const float rowH = (float) b.getHeight() / 9.45f;
        const int gap = juce::jmax (2, juce::roundToInt (rowH * 0.12f));
        const auto kind = strip->getKind();

        // The slots between INPUT and MAIN, with the fader bar after `pre` of them.
        std::vector<std::optional<HomeTab>> slots;
        int pre = 4;
        if (strip->hasSelectableTap())
        {
            const juce::String order = models::list (models::ProcessOrders)[juce::jlimit (0, 23, (int) strip->params.processOrder.load())];
            for (auto letter : order)
                slots.push_back (tabFor (letter));
            slots.push_back (HomeTab::PostInsert);
        }
        else if (! strip->hasPostFaderEq())
        {
            // Aux (Fig. 68): DYN, EQ, INS 1, then the fader.
            slots = { std::nullopt, HomeTab::Dynamics, HomeTab::Eq, HomeTab::PreInsert, std::nullopt };
            if (strip->hasPostInsert())
                slots.back() = HomeTab::PostInsert;
        }
        else
        {
            // Buses, mains, matrices (Figs. 69-71): COMP, INS 1, fader, EQ, INS 2.
            slots = { std::nullopt, HomeTab::Dynamics, HomeTab::PreInsert, HomeTab::Eq, std::nullopt };
            if (strip->hasPostInsert())
                slots.back() = HomeTab::PostInsert;
            pre = 3;
        }

        float y = (float) b.getY();
        auto addRow = [&] (std::optional<HomeTab> tab, bool block)
        {
            const auto cell = juce::Rectangle<int> (b.getX(), juce::roundToInt (y), thumbW, juce::roundToInt (rowH) - gap);
            if (tab.has_value())
            {
                Row r;
                r.tab = *tab;
                r.thumb = cell;
                if (keys && block)
                    r.key = juce::Rectangle<int> (keyX, cell.getY(), keyW, cell.getHeight()).withSizeKeepingCentre (keyW, juce::jmin (keyW, cell.getHeight()));
                rows.push_back (r);
            }
            y += rowH;
        };

        addRow (HomeTab::Home, false);
        addRow (HomeTab::Input, false);
        const float inputBottom = y - (float) gap;
        std::vector<float> above; // the boundary above each slot
        float pfl = 0.0f, afl = 0.0f;
        for (int i = 0; i < (int) slots.size(); ++i)
        {
            if (i == pre)
            {
                const float barH = rowH * 0.45f;
                pfl = y - (float) gap * 0.5f;
                faderBar = juce::Rectangle<float> ((float) b.getX(), y + barH * 0.15f, (float) thumbW, juce::jmax (3.0f, barH * 0.35f));
                y += barH;
                afl = y - (float) gap * 0.5f;
            }
            above.push_back (y - (float) gap * 0.5f);
            addRow (slots[(size_t) i], true);
        }
        // IN, FILTER, TAP 3, TAP 4, TAP 5, PRE FDR, POST FDR, POST PROC.
        tapY = { inputBottom - rowH * 0.3f, above[0], above[1], above[2], above[3], pfl, afl, y - (float) gap * 0.5f };
        addRow (HomeTab::Main, false);
        if (kind != StripKind::Matrix)
            addRow (HomeTab::Sends, false);
        else
            y += rowH;

        // The wrench (input channels: the slot order and TAP point can be dragged).
        wrench = keys && strip->hasSelectableTap()
                     ? juce::Rectangle<int> (keyX, juce::roundToInt (y - rowH), keyW, juce::roundToInt (rowH) - gap).withSizeKeepingCentre (keyW, juce::jmin (keyW, juce::roundToInt (rowH) - gap))
                     : juce::Rectangle<int>();
        thumbWidth = thumbW;
    }

    void paint (juce::Graphics& g) override
    {
        if (strip == nullptr)
            return;
        auto& p = strip->params;

        for (auto& r : rows)
        {
            if (dragTab.has_value() && *dragTab == r.tab)
                continue;
            const bool selected = page.current == r.tab || (r.tab == HomeTab::Home && isOverviewGroup (page.current));
            paintThumbBox (g, r, selected);
            if (! r.key.isEmpty())
                paintKey (g, r);
        }

        // The fader bar: the fader level (red when muted), and the TAP point.
        g.setColour (juce::Colour (0xff2a2a2a));
        g.fillRect (faderBar);
        const float pos = fader::dbToPos (p.faderDb.load());
        g.setColour (p.mute.load() ? Colours::mute : colours::fader);
        g.fillRect (faderBar.withWidth (faderBar.getWidth() * pos));
        if (const int tap = tapIndex(); tap >= 0 && tap < (int) tapY.size())
        {
            const float d = juce::jlimit (6.0f, 11.0f, faderBar.getHeight() * 2.5f);
            const float ty = draggingTap ? (float) dragY : tapY[(size_t) tap];
            g.setColour (Colours::mute);
            g.fillEllipse ((float) thumbWidth * 0.55f - d * 0.5f, ty - d * 0.5f, d, d);
        }

        if (! wrench.isEmpty())
        {
            paintKeyFrame (g, wrench.toFloat(), editing ? Colours::yellow : colours::keyOff, Icon::Wrench, editing);
        }

        // A slot being dragged into another position.
        if (dragTab.has_value())
            for (auto& r : rows)
                if (r.tab == *dragTab)
                {
                    Row moved = r;
                    moved.thumb.setY (dragY - r.thumb.getHeight() / 2);
                    paintThumbBox (g, moved, true);
                }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragTab.reset();
        draggingTap = false;
        if (strip == nullptr || ! editing || ! strip->hasSelectableTap())
            return;
        const auto pos = e.getPosition();
        if (const int tap = tapIndex(); tap >= 0 && tap < (int) tapY.size()
                                         && juce::Point<float> ((float) thumbWidth * 0.55f, tapY[(size_t) tap]).getDistanceFrom (pos.toFloat()) < 10.0f)
        {
            draggingTap = true;
            dragY = pos.y;
            return;
        }
        for (auto& r : rows)
            if (r.thumb.contains (pos) && preIndex (r.tab) >= 0)
            {
                dragTab = r.tab;
                dragY = pos.y;
                return;
            }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragTab.has_value() || draggingTap)
        {
            dragY = juce::jlimit (0, getHeight(), e.y);
            repaint();
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (strip == nullptr)
            return;
        const auto pos = e.getPosition();

        if (draggingTap)
        {
            draggingTap = false;
            int best = 0;
            for (int i = 1; i < (int) tapY.size(); ++i)
                if (std::abs (tapY[(size_t) i] - (float) pos.y) < std::abs (tapY[(size_t) best] - (float) pos.y))
                    best = i;
            strip->params.tap = best;
            repaint();
            return;
        }
        if (dragTab.has_value())
        {
            const auto from = *dragTab;
            dragTab.reset();
            if (e.mouseWasDraggedSinceMouseDown())
                moveBlock (from, pos.y);
            repaint();
            if (e.mouseWasDraggedSinceMouseDown())
                return;
        }
        if (! getLocalBounds().contains (pos))
            return;

        if (wrench.contains (pos))
        {
            if (e.mods.isPopupMenu())
                showOrderMenu();
            else
            {
                editing = ! editing;
                repaint();
            }
            return;
        }

        for (auto& r : rows)
        {
            if (! r.key.isEmpty() && r.key.expanded (0, 2).contains (pos))
            {
                toggle (r.tab);
                repaint();
                return;
            }
            if (r.thumb.contains (pos))
            {
                if (r.tab == HomeTab::Sends)
                {
                    // The top half opens Matrix Sends, the bottom half Bus Sends;
                    // clicking the slot twice toggles between them.
                    const bool mainStrip = strip->getKind() == StripKind::Main;
                    bool matrix = mainStrip || pos.y < r.thumb.getCentreY();
                    if (page.current == HomeTab::Sends && ! mainStrip && matrix == page.matrixSends)
                        matrix = ! matrix;
                    page.showSends (matrix);
                }
                else
                    page.showTab (r.tab);
                return;
            }
        }
    }

private:
    struct Row
    {
        HomeTab tab = HomeTab::Home;
        juce::Rectangle<int> thumb {}, key {};
    };

    static std::optional<HomeTab> tabFor (juce::juce_wchar letter)
    {
        switch (letter)
        {
            case 'G': return HomeTab::Gate;
            case 'E': return HomeTab::Eq;
            case 'D': return HomeTab::Dynamics;
            case 'I': return HomeTab::PreInsert;
            default:  return std::nullopt;
        }
    }

    static juce::juce_wchar letterFor (HomeTab t)
    {
        return t == HomeTab::Gate ? 'G' : (t == HomeTab::Eq ? 'E' : (t == HomeTab::Dynamics ? 'D' : 'I'));
    }

    /** Index of a pre-fader block of an input channel (0-3), or -1. */
    int preIndex (HomeTab t) const
    {
        if (strip == nullptr || ! strip->hasSelectableTap())
            return -1;
        if (t != HomeTab::Gate && t != HomeTab::Eq && t != HomeTab::Dynamics && t != HomeTab::PreInsert)
            return -1;
        const auto order = models::list (models::ProcessOrders)[juce::jlimit (0, 23, (int) strip->params.processOrder.load())];
        return order.indexOfChar (letterFor (t));
    }

    void moveBlock (HomeTab t, int y)
    {
        const int from = preIndex (t);
        if (from < 0)
            return;
        // The pre-fader slot nearest the drop point.
        int to = from;
        float best = 1.0e9f;
        for (auto& r : rows)
            if (const int i = preIndex (r.tab); i >= 0 && std::abs ((float) r.thumb.getCentreY() - (float) y) < best)
            {
                best = std::abs ((float) r.thumb.getCentreY() - (float) y);
                to = i;
            }
        if (to == from)
            return;
        const auto orders = models::list (models::ProcessOrders);
        auto order = orders[juce::jlimit (0, 23, (int) strip->params.processOrder.load())];
        const auto letter = order[from];
        order = order.substring (0, from) + order.substring (from + 1);
        order = order.substring (0, to) + juce::String::charToString (letter) + order.substring (to);
        if (const int index = orders.indexOf (order); index >= 0)
        {
            strip->params.processOrder = index;
            builtOrder = index;
            resized();
        }
    }

    /** The TAP point's position in tapY, or -1 when the strip shows none. */
    int tapIndex() const
    {
        if (strip == nullptr)
            return -1;
        if (strip->hasSelectableTap())
            return juce::jlimit (0, 7, (int) strip->params.tap.load());
        // Fixed after INS 1, pre-fader; matrices send nowhere.
        return strip->getKind() == StripKind::Matrix ? -1 : ld::kTapPfl;
    }

    static Icon keyIcon (HomeTab t)
    {
        if (t == HomeTab::Gate)     return Icon::Gate;
        if (t == HomeTab::Dynamics) return Icon::Compressor;
        if (t == HomeTab::Eq)       return Icon::Eq;
        return Icon::Insert;
    }

    static juce::Colour keyColour (HomeTab t)
    {
        if (t == HomeTab::Gate)     return Colours::yellow;
        if (t == HomeTab::Dynamics) return colours::dynOn;
        if (t == HomeTab::Eq)       return colours::eqOn;
        return colours::insOn;
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

    static void paintKeyFrame (juce::Graphics& g, juce::Rectangle<float> k, juce::Colour fill, Icon icon, bool on)
    {
        g.setColour (colours::keyFrame);
        g.fillRoundedRectangle (k, 3.0f);
        auto inner = k.reduced (juce::jmax (2.0f, k.getWidth() * 0.1f));
        g.setColour (fill);
        g.fillRoundedRectangle (inner, 2.0f);
        drawIcon (g, icon, inner.reduced (inner.getHeight() * 0.18f), on ? juce::Colours::black : Colours::textDim);
    }

    void paintKey (juce::Graphics& g, const Row& r) const
    {
        const bool on = isOn (r.tab);
        auto k = r.key.toFloat();
        paintKeyFrame (g, k, on ? keyColour (r.tab) : colours::keyOff, keyIcon (r.tab), on);
        // The pointer joining the key to its thumbnail.
        juce::Path tri;
        const float s = juce::jlimit (3.0f, 6.0f, k.getHeight() * 0.12f);
        tri.addTriangle (k.getX() - s, k.getCentreY(), k.getX(), k.getCentreY() - s, k.getX(), k.getCentreY() + s);
        g.setColour (colours::keyFrame);
        g.fillPath (tri);
    }

    void paintThumbBox (juce::Graphics& g, const Row& r, bool selected) const
    {
        auto t = r.thumb.toFloat();
        if (r.tab == HomeTab::Home)
        {
            g.setColour (juce::Colour (0xff2c2c2c));
            g.fillRoundedRectangle (t, 2.0f);
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (juce::jlimit (11.0f, 24.0f, t.getHeight() * 0.5f), juce::Font::plain));
            g.drawFittedText ("HOME", t.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.7f);
        }
        else
        {
            g.setColour (colours::thumb);
            g.fillRoundedRectangle (t, 2.0f);
            paintThumb (g, r.tab, t.reduced (juce::jmax (2.0f, t.getHeight() * 0.07f)));
        }
        if (selected)
        {
            g.setColour (Colours::yellow);
            g.drawRoundedRectangle (t.reduced (1.0f), 2.0f, 2.0f);
        }
        if (editing && preIndex (r.tab) >= 0)
        {
            juce::Path outline;
            outline.addRectangle (t.reduced (1.0f));
            juce::Path dashed;
            const float dashes[] = { 4.0f, 3.0f };
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
            g.setColour (Colours::yellow.withAlpha (0.7f));
            g.fillPath (dashed);
        }
    }

    void chip (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, bool on, juce::Colour onColour = Colours::text) const
    {
        g.setColour (on ? onColour : colours::chipOff);
        g.fillRoundedRectangle (r, 1.5f);
        g.setColour (on ? juce::Colours::black : juce::Colour (0xff1e1e1e));
        g.setFont (juce::FontOptions (juce::jlimit (6.0f, 15.0f, r.getHeight() * 0.86f), juce::Font::bold));
        g.drawFittedText (text, r.toNearestInt(), juce::Justification::centred, 1, 0.6f);
    }

    void miniMeter (juce::Graphics& g, juce::Rectangle<float> r, float level, juce::Colour c) const
    {
        const int segments = juce::jlimit (4, 10, (int) (r.getHeight() / 3.0f));
        const float h = r.getHeight() / (float) segments;
        for (int i = 0; i < segments; ++i)
        {
            const bool lit = level > (float) i / (float) segments;
            g.setColour (lit ? c : juce::Colour (0xff232323));
            g.fillRect (r.getX(), r.getBottom() - (float) (i + 1) * h + 0.5f, r.getWidth(), h - 1.0f);
        }
    }

    void paintThumb (juce::Graphics& g, HomeTab tab, juce::Rectangle<float> r) const
    {
        auto& p = strip->params;
        auto& fx = page.ctx.engine.getFx();
        const float chipH = juce::jlimit (7.0f, 18.0f, r.getHeight() * 0.38f);
        const float meterW = juce::jlimit (3.0f, 7.0f, r.getWidth() * 0.08f);

        switch (tab)
        {
            case HomeTab::Input:
            {
                const int n = strip->getNumChannels();
                auto meters = r.removeFromRight (meterW * (float) n + (n > 1 ? 1.0f : 0.0f));
                for (int ch = 0; ch < n; ++ch)
                {
                    const float level = MeterComponent::dbToProportion (dsp::gainToDb (page.ctx.meters.get (strip, true, ch)));
                    miniMeter (g, meters.removeFromLeft (meterW), level, Colours::meterGreen);
                    meters.removeFromLeft (1.0f);
                }
                r.removeFromRight (2.0f);
                const bool ch = strip->getKind() == StripKind::Channel;
                const float w = (r.getWidth() - 2.0f) / 3.0f;
                auto row1 = r.removeFromTop (r.getHeight() / 2.0f).withSizeKeepingCentre (r.getWidth(), chipH);
                auto row2 = r.withSizeKeepingCentre (r.getWidth(), chipH);
                if (ch)
                {
                    chip (g, row1.removeFromLeft (w), juce::String::fromUTF8 ("\xc3\x98"), p.input.invert.load());
                    row1.removeFromLeft (1.0f);
                    chip (g, row1.removeFromLeft (w), "LC", p.filter.lowCut.load(), colours::eqOn);
                    row1.removeFromLeft (1.0f);
                    chip (g, row1, "HC", p.filter.highCut.load(), colours::eqOn);
                    chip (g, row2.removeFromLeft (w), "TOOL", p.filter.tool.load(), colours::eqOn);
                    row2.removeFromLeft (1.0f);
                    chip (g, row2.removeFromLeft (w), "DLY", p.input.delayOn.load());
                    row2.removeFromLeft (1.0f);
                    chip (g, row2, p.stereo.load() ? "ST" : "M", true);
                }
                else
                {
                    chip (g, row1.removeFromLeft (w * 1.5f), juce::String::fromUTF8 ("\xc3\x98"), p.input.invert.load());
                    row1.removeFromLeft (1.0f);
                    chip (g, row1, "DLY", p.input.delayOn.load());
                    chip (g, row2.removeFromLeft (w * 1.5f), strip->isInput() && ! p.stereo.load() ? "M" : "ST", true);
                }
                return;
            }

            case HomeTab::Gate:
            case HomeTab::Dynamics:
            {
                auto& slot = tab == HomeTab::Gate ? p.gate : p.dynamics;
                auto meter = r.removeFromRight (meterW);
                const float gr = (tab == HomeTab::Gate ? strip->meters.gateReductionDb.load() : strip->meters.dynReductionDb.load()) / 24.0f;
                miniMeter (g, meter, juce::jlimit (0.0f, 1.0f, gr), Colours::amber);
                r.removeFromRight (2.0f);
                const auto& model = models::model (models::Dynamics, slot.model.load());
                chip (g, r.removeFromTop (chipH).withWidth (juce::jmin (r.getWidth(), chipH * 2.8f)), model.token, slot.on.load(), Colours::yellow);
                // The model's name as a caption when there is room.
                if (r.getHeight() >= 11.0f && r.getWidth() >= 40.0f)
                {
                    g.setColour (Colours::textDim);
                    g.setFont (juce::FontOptions (juce::jmin (12.0f, r.getHeight() * 0.7f), juce::Font::bold));
                    g.drawFittedText (model.name.toUpperCase(), r.toNearestInt(), juce::Justification::bottomRight, 1, 0.6f);
                }
                return;
            }

            case HomeTab::PreInsert:
            case HomeTab::PostInsert:
            {
                auto& ins = tab == HomeTab::PreInsert ? p.preInsert : p.postInsert;
                const int slot = ins.fx.load();
                const int mode = tab == HomeTab::PostInsert ? ins.mode.load() : 0;
                // Automix on INS 2: chip AUTO with the group below right (Fig. 64).
                const juce::String text = mode > 0 ? juce::String ("AUTO") : (slot >= 0 ? "FX " + juce::String (slot + 1) : juce::String ("INS"));
                chip (g, r.removeFromTop (chipH).withWidth (juce::jmin (r.getWidth(), chipH * 2.8f)), text, isOn (tab) || (mode > 0 && ins.on.load()), Colours::yellow);
                juce::String device;
                if (mode > 0)
                    device = mode == 1 ? "X" : "Y";
                else if (auto* plugin = slot >= 0 ? fx.getPlugin (slot) : nullptr)
                    device = plugin->getName().toUpperCase();
                else if (slot >= 0 && fx.getModel (slot) > ld::kFxExt)
                    device = models::model (models::Fx, fx.getModel (slot)).token;
                g.setColour (Colours::text);
                g.setFont (juce::FontOptions (juce::jlimit (8.0f, 14.0f, r.getHeight() * 0.75f), juce::Font::bold));
                g.drawFittedText (device, r.toNearestInt(), juce::Justification::bottomRight, 1, 0.6f);
                return;
            }

            case HomeTab::Eq:
            {
                g.setColour (juce::Colours::black);
                g.fillRect (r);
                if (eqCurve.size() > 1)
                {
                    juce::Path curve;
                    for (size_t i = 0; i < eqCurve.size(); ++i)
                    {
                        const float x = r.getX() + r.getWidth() * (float) i / (float) (eqCurve.size() - 1);
                        const float y = r.getCentreY() - juce::jlimit (-1.0f, 1.0f, eqCurve[i] / 15.0f) * r.getHeight() * 0.45f;
                        if (i == 0)
                            curve.startNewSubPath (x, y);
                        else
                            curve.lineTo (x, y);
                    }
                    g.setColour (p.eq.on.load() ? juce::Colour (0xffe8a33a) : Colours::textDim);
                    g.strokePath (curve, juce::PathStrokeType (1.5f));
                }
                return;
            }

            case HomeTab::Main:
            {
                auto knob = r.removeFromLeft (juce::jmin (r.getHeight(), r.getWidth() * 0.5f));
                PanWidthDisplay::paintRing (g, knob, p.pan.load(), p.width.load(), false);
                r.removeFromLeft (2.0f);
                if (strip->getKind() != StripKind::Main && strip->getKind() != StripKind::Matrix)
                {
                    // The main assignments: a column of 4 small squares.
                    auto col = r.removeFromLeft (juce::jmin (r.getWidth() * 0.4f, r.getHeight() / 4.0f + 2.0f));
                    const float h = col.getHeight() / (float) kNumMains;
                    for (int i = 0; i < kNumMains; ++i)
                    {
                        auto sq = juce::Rectangle<float> (col.getX(), col.getY() + (float) i * h, col.getWidth(), h).reduced (0.5f);
                        g.setColour (p.mains[i].on.load() ? colours::fader : juce::Colour (0xff232323));
                        g.fillRect (sq.withSizeKeepingCentre (juce::jmin (sq.getWidth(), sq.getHeight()), juce::jmin (sq.getWidth(), sq.getHeight())));
                    }
                }
                else
                {
                    // Output slot: DLY and a stereo meter.
                    auto meters = r.removeFromRight (meterW * 2.0f + 1.0f);
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        const float level = MeterComponent::dbToProportion (dsp::gainToDb (page.ctx.meters.get (strip, false, ch)));
                        miniMeter (g, meters.removeFromLeft (meterW), level, Colours::meterGreen);
                        meters.removeFromLeft (1.0f);
                    }
                    chip (g, r.withSizeKeepingCentre (r.getWidth() - 2.0f, chipH).withY (r.getBottom() - chipH), "DLY", p.input.delayOn.load());
                }
                return;
            }

            case HomeTab::Sends:
            {
                // Top half: matrix sends; bottom half: bus sends (1-16).
                auto top = r.removeFromTop (r.getHeight() / 2.0f).reduced (0.0f, 1.0f);
                auto bottom = r.reduced (0.0f, 1.0f);
                auto bars = [&g] (juce::Rectangle<float> area, int n, std::function<float (int)> level)
                {
                    const float w = area.getWidth() / (float) juce::jmax (1, n);
                    for (int i = 0; i < n; ++i)
                    {
                        auto bar = juce::Rectangle<float> (area.getX() + (float) i * w, area.getY(), juce::jmax (1.0f, w - 1.0f), area.getHeight());
                        g.setColour (juce::Colour (0xff232323));
                        g.fillRect (bar);
                        const float l = level (i);
                        if (l > 0.0f)
                        {
                            g.setColour (juce::Colour (0xff4fd6dc));
                            g.fillRect (bar.withTop (bar.getBottom() - bar.getHeight() * l));
                        }
                    }
                };
                if (strip->getKind() != StripKind::Matrix)
                    bars (top, kNumMatrices, [&p] (int i) { return p.matrix[i].on.load() ? fader::dbToPos (p.matrix[i].levelDb.load()) : 0.0f; });
                if (strip->getKind() != StripKind::Main)
                {
                    const int n = juce::jmin (16, (int) page.ctx.engine.getBuses().size());
                    bars (bottom, n, [this] (int i)
                    {
                        auto* send = strip->sends.find (i);
                        return send != nullptr && send->enabled.load() ? fader::dbToPos (send->levelDb.load()) : 0.0f;
                    });
                }
                return;
            }

            case HomeTab::Home:
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
                names.add (letter == 'G' ? "GATE" : letter == 'E' ? "EQ" : letter == 'D' ? "DYN" : "INS 1");
            menu.addItem (names.joinIntoString ("  >  "), s->hasSelectableTap(), s->params.processOrder.load() == i,
                          [this, i] { if (strip != nullptr) { strip->params.processOrder = i; setStrip (strip); } });
        }
        juce::PopupMenu taps;
        const auto tapNames = tapPointNames();
        for (int i = 0; i < tapNames.size(); ++i)
            taps.addItem (tapNames[i], s->hasSelectableTap(), s->params.tap.load() == i, [this, i] { if (strip != nullptr) strip->params.tap = i; });
        menu.addSubMenu ("TAP POINT", taps);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withTargetScreenArea (localAreaToGlobal (wrench)));
    }

    HomePage& page;
    Strip* strip = nullptr;
    std::vector<Row> rows;
    std::vector<float> tapY;
    juce::Rectangle<float> faderBar;
    juce::Rectangle<int> wrench;
    std::vector<float> eqCurve;
    int builtOrder = -1, thumbWidth = 0, dragY = 0;
    bool keys = true, editing = false, draggingTap = false;
    std::optional<HomeTab> dragTab;
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

    auto safe = std::make_unique<IconKey> ("SAFE", Icon::LockOpen, Colours::mute);
    safe->setTooltip ("Recall safe: scene recalls skip this strip");
    safe->onClick = [this] { if (strip != nullptr) strip->params.recallSafe = ! strip->params.recallSafe.load(); };
    safe->isOn = [this] { return strip != nullptr && strip->params.recallSafe.load(); };
    safeKey = std::move (safe);
    addAndMakeVisible (*safeKey);
}

HomePage::~HomePage() = default;

bool HomePage::isAvailable (HomeTab t, const Strip* s) const
{
    if (s == nullptr)
        return false;
    if (t == HomeTab::Filter || t == HomeTab::Gate)
        return s->getKind() == StripKind::Channel;
    if (t == HomeTab::PostInsert)
        return s->hasPostInsert();
    if (t == HomeTab::Sends)
        return s->getKind() != StripKind::Matrix;
    return true;
}

void HomePage::showTab (HomeTab t)
{
    if (t == HomeTab::Sends && strip != nullptr && strip->getKind() == StripKind::Main)
        matrixSends = true;
    current = t;
    closeOverlay();
    rebuild();
}

void HomePage::showSends (bool matrixPage)
{
    matrixSends = matrixPage;
    showTab (HomeTab::Sends);
}

void HomePage::refresh()
{
    rebuild();
}

void HomePage::rebuild()
{
    auto* now = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr;
    if (now != strip)
        closeOverlay();
    strip = now;
    if (! isAvailable (current, strip))
        current = HomeTab::Home;

    column->setStrip (strip);
    column->setShowKeys (isOverviewGroup (current));
    const bool overviewGroup = isOverviewGroup (current);
    const HomeTab group[] = { HomeTab::Home, HomeTab::Icon, HomeTab::Name, HomeTab::Tags };
    for (int i = 0; i < topTabs.size(); ++i)
    {
        topTabs[i]->setVisible (overviewGroup && strip != nullptr);
        topTabs[i]->setToggleState (group[i] == current, juce::dontSendNotification);
    }
    safeKey->setVisible (overviewGroup && strip != nullptr);

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
            case HomeTab::Sends:      content = makeSendsScreen (ctx, s, matrixSends || s.getKind() == StripKind::Main); break;
        }
        content->knobsChanged = [this] { notifyKnobsChanged(); };
        content->openFeedConfig = [this] { openOverlay (OverlayKind::Feed); };
        addAndMakeVisible (*content);
    }
    if (overlay != nullptr)
        overlay->toFront (false);

    resized();
    repaint();
    notifyKnobsChanged();
    notifyStatusKeysChanged();
}

void HomePage::openOverlay (OverlayKind kind)
{
    if (strip == nullptr)
        return;
    if (overlayKind == kind)
    {
        closeOverlay();
        return;
    }
    overlay.reset();
    switch (kind)
    {
        case OverlayKind::Init:     overlay = makeInitCopyScreen (ctx, *strip, false, current); break;
        case OverlayKind::Copy:     overlay = makeInitCopyScreen (ctx, *strip, true, current); break;
        case OverlayKind::Settings: overlay = makeSettingsScreen (ctx, *strip, current == HomeTab::Eq); break;
        case OverlayKind::Feed:     overlay = makeFeedConfigScreen (ctx, *strip); break;
        case OverlayKind::None:     break;
    }
    overlayKind = overlay != nullptr ? kind : OverlayKind::None;
    if (overlay != nullptr)
    {
        overlay->onClose = [this] { juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<HomePage> (this)] { if (safe) safe->closeOverlay(); }); };
        addAndMakeVisible (*overlay);
        overlay->setBounds (getLocalBounds());
    }
    notifyStatusKeysChanged();
}

void HomePage::closeOverlay()
{
    if (overlay == nullptr)
        return;
    overlay.reset();
    overlayKind = OverlayKind::None;
    if (content != nullptr)
        content->repaint();
    notifyStatusKeysChanged();
}

std::vector<KnobBinding> HomePage::getKnobs()
{
    std::vector<KnobBinding> k;
    if (content != nullptr)
        for (auto& knob : content->knobs())
            k.push_back (knob.binding);
    return k;
}

std::vector<KnobStyle> HomePage::getKnobStyles()
{
    std::vector<KnobStyle> k;
    if (content != nullptr)
        for (auto& knob : content->knobs())
            k.push_back (knob.style);
    return k;
}

std::vector<StatusKey> HomePage::getStatusKeys()
{
    if (strip == nullptr)
        return {};
    // INIT and COPY on every slot screen; SETTINGS on OVERVIEW, FILTER and EQ.
    std::vector<StatusKey> keys;
    keys.push_back ({ "INIT", Icon::Reset, overlayKind == OverlayKind::Init, [this] { openOverlay (OverlayKind::Init); } });
    keys.push_back ({ "COPY", Icon::Copy, overlayKind == OverlayKind::Copy, [this] { openOverlay (OverlayKind::Copy); } });
    if (content != nullptr && content->hasSettings())
        keys.push_back ({ "SETTINGS", Icon::Gear, overlayKind == OverlayKind::Settings, [this] { openOverlay (OverlayKind::Settings); } });
    return keys;
}

void HomePage::paint (juce::Graphics& g)
{
    g.fillAll (Colours::screen);
    if (strip == nullptr)
    {
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (24.0f, juce::Font::bold));
        g.drawText ("PRESS SEL ON A STRIP", getLocalBounds(), juce::Justification::centred);
    }
}

void HomePage::resized()
{
    auto b = getLocalBounds();
    if (overlay != nullptr)
        overlay->setBounds (b);

    // The toolbar: thumbnails and keys on Channel Home (about 15 % of the
    // screen in Fig. 21), only the thumbnails on the slot screens (about 8 %).
    const bool overviewGroup = isOverviewGroup (current);
    const int screenW = getWidth();
    const int thumbW = juce::jlimit (40, 120, screenW * 75 / 1000);
    const int colW = overviewGroup ? thumbW * 100 / 52 : thumbW;
    column->setBounds (b.removeFromLeft (colW));
    b.removeFromLeft (juce::jmax (4, screenW / 120));

    if (overviewGroup)
    {
        // The tab row sits level with the HOME key.
        auto top = b.removeFromTop (juce::roundToInt ((float) getHeight() / 9.45f) - juce::jmax (2, getHeight() / 80));
        safeKey->setBounds (top.removeFromRight (juce::jmin (110, top.getWidth() / 7)));
        top.removeFromRight (8);
        const int w = juce::jmin (150, top.getWidth() / 5);
        for (auto* t : topTabs)
            t->setBounds (top.removeFromLeft (w).reduced (1, 0));
        b.removeFromTop (juce::jmax (2, getHeight() / 80));
    }

    if (content != nullptr)
        content->setBounds (b);
}

} // namespace livedaw::ui
