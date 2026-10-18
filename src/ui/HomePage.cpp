#include "ui/HomePage.h"
#include "dsp/DspUtils.h"
#include "ui/Faceplate.h"
#include "ui/HomeIcons.h"
#include "ui/Icons.h"
#include "ui/MeterComponent.h"
#include "ui/ResponseGraph.h"
#include "ui/WingPopups.h"

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
                return inputPortName (in) + "/" + inputPortName (right).fromLastOccurrenceOf (" ", false, false);
            }
            return inputPortName (in);
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (juce::Colours::black);
            g.fillRect (r);
            auto inner = r.reduced (2.0f);
            auto header = inner.removeFromTop (juce::jlimit (12.0f, 22.0f, inner.getHeight() * 0.2f));
            const auto colour = stripDisplayColour (strip);
            g.setColour (colour);
            g.fillRect (header);
            g.setColour (colour.getPerceivedBrightness() > 0.45f ? juce::Colours::black : juce::Colours::white);
            g.setFont (juce::FontOptions (header.getHeight() * 0.85f, juce::Font::bold));
            g.drawFittedText (stripName (strip).toUpperCase(), header.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.7f);

            g.setColour (colours::keyOff);
            g.fillRect (inner);
            auto label = inner.removeFromBottom (juce::jlimit (12.0f, 22.0f, inner.getHeight() * 0.26f));
            auto body = inner.reduced (4.0f, 3.0f);
            drawChannelIcon (g, strip.params.icon.load(), strip.getKind(), body.removeFromLeft (body.getWidth() * 0.55f), colours::ink);
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

            g.setColour (colours::well);
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
            const auto now = portName (ctx, strip) + stripName (strip) + juce::String (strip.getColourIndex()) + "|" + juce::String (strip.params.icon.load());
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
                g.setColour (on ? colours::fader : colours::well);
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
    OverviewScreen (UiContext& c, Strip& s) : HomeScreen (c, s)
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
            tool = &key (toolFilterName (p.filter.toolModel.load()), toolFilterIcon (p.filter.toolModel.load()), toolFilterColour (p.filter.toolModel.load()));
            bindKey (*tool, p.filter.tool);
        }

        level = &make<LevelPanel> (ctx, strip);

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
            // I1: a square source tile (112 of the panel's 224) and the MUTE
            // key (84 x 56) under it.
            const int tile = juce::jmin (in.getWidth() / 4, in.getHeight() * 52 / 100);
            auto tileCol = in.removeFromLeft (tile);
            source->setBounds (tileCol.removeFromTop (tile));
            tileCol.removeFromTop (tile / 6);
            const auto muteRow = tileCol.removeFromTop (tile / 2);
            sourceMute->setBounds (muteRow.withSizeKeepingCentre (tile * 3 / 4, muteRow.getHeight()));
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
        level->setBounds (box (levelArea, "LEVEL"));

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
            tool->setOnColour (toolFilterColour (strip.params.filter.toolModel.load()));
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
    IconKey *soloA = nullptr, *soloB = nullptr;
    KnobBox* trim = nullptr;
    ProcessorThumbs* processors = nullptr;
    LevelPanel* level = nullptr;
    ResponseGraph* graph = nullptr;
    PanWidthDisplay* pan = nullptr;
    MainSquares* mains = nullptr;
    juce::Label* soloBusLabel = nullptr;
    juce::Rectangle<int> inputArea, filterArea, levelArea, eqArea, mainArea;
    int shownEq = -1;
};

//==============================================================================
/** The strip before or after `from` in CHANNEL SELECT order (channels, aux,
    buses, mains, matrices), wrapping round. */
Strip* adjacentStrip (MixerEngine& engine, const Strip* from, int delta)
{
    std::vector<Strip*> order;
    for (int k = 0; k < kNumStripKinds; ++k)
        for (auto& s : engine.getStrips (static_cast<StripKind> (k)))
            order.push_back (s.get());
    if (order.empty())
        return nullptr;
    const auto it = std::find (order.begin(), order.end(), from);
    const int at = it != order.end() ? (int) std::distance (order.begin(), it) : (delta > 0 ? -1 : 0);
    const int n = (int) order.size();
    return order[(size_t) (((at + delta) % n + n) % n)];
}

/** The LINK CUSTOMIZATION TO SOURCE row of ICON/COLOR and NAME: the link key
    (a keyboard-and-pencil icon, gold while linked), the caption and the NONE
    and ALL keys. The console's sources (manual 5.4) are not modelled, so the
    row is shown off. */
class LinkCustomizationRow final : public juce::Component
{
public:
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (colours::panel);
        g.fillRect (r);
        r = r.reduced (r.getHeight() * 0.12f);
        auto key = r.removeFromLeft (r.getHeight());
        paintKeyBody (g, key, colours::keyOff);
        drawIcon (g, Icon::KeyboardEdit, key.reduced (key.getHeight() * 0.18f), colours::dim);
        r.removeFromLeft (r.getHeight() * 0.25f);
        // NONE and ALL: list icons with their captions under them.
        for (int i = 1; i >= 0; --i)
        {
            auto k = r.removeFromRight (r.getHeight() * 1.1f);
            auto caption = k.removeFromBottom (k.getHeight() * 0.34f);
            drawIcon (g, i == 1 ? Icon::ListAll : Icon::ListNone, k.withSizeKeepingCentre (k.getHeight() * 0.9f, k.getHeight() * 0.9f), colours::dim);
            g.setColour (colours::dim);
            g.setFont (juce::FontOptions (juce::jlimit (8.0f, 13.0f, caption.getHeight() * 0.95f), juce::Font::plain));
            g.drawText (i == 1 ? "ALL" : "NONE", caption.toNearestInt(), juce::Justification::centred);
            r.removeFromRight (r.getHeight() * 0.2f);
        }
        g.setColour (colours::dim);
        g.setFont (homeFont (TypeScale::body, r.getHeight() * 0.46f));
        g.drawFittedText ("LINK CUSTOMIZATION TO SOURCE", r.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
    }
};

/** NAME (firmware 3.1): the link row; the name in a black field with the
    channel's icon, a gold caret and the circled clear key; five name
    suggestions for the channel's icon; the on-screen keyboard, whose NEXT key
    keeps the name and moves on to the next channel. */
class NameScreen final : public HomeScreen, private juce::Timer
{
public:
    NameScreen (UiContext& c, Strip& s) : HomeScreen (c, s)
    {
        link = &make<LinkCustomizationRow>();
        // The keyboard's own field is hidden: this screen's field shows the
        // channel's WING icon. Its suggestion row and keys follow.
        keyboard = &make<popups::WingKeyboard>();
        keyboard->setFieldVisible (false);
        keyboard->setMaxLength (kMaxNameLength);
        keyboard->setText (strip.getName());
        keyboard->setActionText ("NEXT");
        keyboard->onChange = [this] (const juce::String& t)
        {
            strip.setName (t.substring (0, kMaxNameLength));
            repaint (field);
        };
        keyboard->onAction = [this] (const juce::String& t)
        {
            strip.setName (t.substring (0, kMaxNameLength));
            if (auto* next = adjacentStrip (ctx.engine, &strip, 1); next != nullptr && ctx.selectStrip)
                juce::MessageManager::callAsync ([&c = ctx, next] { if (c.selectStrip) c.selectStrip (next); });
        };
        keyboard->setSuggestionRowAlwaysShown (true);
        keyboard->setSuggestions (nameSuggestions (strip.params.icon.load()));
        addLevelPanel();
        startTimerHz (4);
    }

    static constexpr int kMaxNameLength = 16;

    void timerCallback() override
    {
        if (keyboard->getText() != strip.getName())
        {
            keyboard->setText (strip.getName());
            repaint (field);
        }
        // The caret blinks.
        caretOn = ! caretOn;
        repaint (field);
    }

    void resized() override
    {
        clearBoxes();
        auto b = layoutLevelPanel (getLocalBounds());
        // WING pixels (from the top of the content, 564 high): the link row
        // 64, the field panel 80, then the suggestions and the keyboard.
        const float v = (float) b.getHeight() / 564.0f;
        link->setBounds (b.removeFromTop (juce::roundToInt (64.0f * v)));
        b.removeFromTop (juce::roundToInt (8.0f * v));
        fieldPanel = b.removeFromTop (juce::roundToInt (80.0f * v));
        field = fieldPanel.withSizeKeepingCentre (fieldPanel.getWidth() * 58 / 100, fieldPanel.getHeight() * 76 / 100);
        clearArea = field.withLeft (field.getRight() - field.getHeight());
        b.removeFromTop (juce::roundToInt (8.0f * v));
        keyboardPanel = b;
        keyboard->setBounds (b.reduced (juce::roundToInt (8.0f * v)));
    }

    void paint (juce::Graphics& g) override
    {
        HomeScreen::paint (g);
        g.setColour (colours::panel);
        for (auto r : { fieldPanel, keyboardPanel })
            g.fillRect (r);

        // The field: black, a grey outline, the icon, the name with the gold
        // caret, the circled clear key.
        auto f = field.toFloat();
        g.setColour (juce::Colours::black);
        g.fillRect (f);
        g.setColour (colours::outline);
        g.drawRect (f, 1.5f);
        auto inner = f.reduced (f.getHeight() * 0.18f);
        drawChannelIcon (g, strip.params.icon.load(), strip.getKind(), inner.removeFromLeft (inner.getHeight() * 1.3f), colours::ink);
        inner.removeFromLeft (inner.getHeight() * 0.5f);
        auto clear = clearArea.toFloat().reduced (clearArea.getHeight() * 0.24f);
        inner.removeFromRight (clearArea.getWidth());
        const auto name = strip.getName();
        g.setColour (colours::ink);
        const juce::Font font (homeFont (TypeScale::value * 1.2f, inner.getHeight() * 0.92f));
        g.setFont (font);
        g.drawText (name, inner.toNearestInt(), juce::Justification::centredLeft, true);
        if (caretOn)
        {
            const float x = inner.getX() + juce::jmin (inner.getWidth(), juce::GlyphArrangement::getStringWidth (font, name)) + 2.0f;
            g.setColour (colours::accent);
            g.fillRect (x, inner.getY(), 2.0f, inner.getHeight());
        }
        g.setColour (colours::caption);
        g.drawEllipse (clear, 1.5f);
        const auto cc = clear.getCentre();
        const float s = clear.getWidth() * 0.2f;
        g.drawLine (cc.x - s, cc.y - s, cc.x + s, cc.y + s, 2.0f);
        g.drawLine (cc.x - s, cc.y + s, cc.x + s, cc.y - s, 2.0f);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (clearArea.contains (e.getPosition()))
        {
            strip.setName ({});
            keyboard->setText ({});
            repaint (field);
        }
    }

private:
    LinkCustomizationRow* link = nullptr;
    popups::WingKeyboard* keyboard = nullptr;
    juce::Rectangle<int> fieldPanel, field, clearArea, keyboardPanel;
    bool caretOn = true;
};

/** ICON/COLOR (I13): the link row over the icon gallery (seven category keys
    and a grid of five columns, the selected icon framed gold); LIGHT (the
    light bar above the scribble strip) over the colour grid (3 x 6, the WING
    colours down the columns); the LEVEL panel at the right. */
class IconColorScreen final : public HomeScreen, private juce::Timer
{
public:
    IconColorScreen (UiContext& c, Strip& s) : HomeScreen (c, s)
    {
        link = &make<LinkCustomizationRow>();
        category = wingIconCategory (strip.params.icon.load());
        addLevelPanel();
        startTimerHz (6);
    }

    KnobBinding iconBinding()
    {
        auto& i = strip.params.icon;
        KnobBinding b;
        b.label = "ICON";
        // Steps through every icon of the gallery in order.
        auto all = [] { std::vector<int> v; for (auto& c : wingIconCategories()) for (auto& icon : c.icons) v.push_back (icon.number); return v; };
        b.get = [&i, all]
        {
            const auto v = all();
            const auto it = std::find (v.begin(), v.end(), (int) i.load());
            return it == v.end() ? 0.0f : (float) std::distance (v.begin(), it) / (float) (v.size() - 1);
        };
        b.set = [&i, all] (float p)
        {
            const auto v = all();
            i = v[(size_t) juce::roundToInt (p * (float) (v.size() - 1))];
        };
        b.text = [&i] { return wingIconName (i.load()).toUpperCase(); };
        b.reset = [&i] { i = 0; };
        b.steps = 0.0f;
        for (auto& cat : wingIconCategories())
            b.steps += (float) cat.icons.size();
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
        return { { colourBinding(), styleOf (KnobStyle::Bar::Segments, colours::accent, nullptr, 18) },
                 { iconBinding(), styleOf (KnobStyle::Bar::Fill, KnobColours::white) } };
    }

    void timerCallback() override
    {
        const auto now = std::make_tuple ((int) strip.params.icon.load(), strip.getColourIndex(), strip.params.led.load());
        if (now != shown)
        {
            // The gallery follows the icon chosen on the knob.
            if (std::get<0> (now) != std::get<0> (shown))
                category = wingIconCategory (std::get<0> (now));
            shown = now;
            repaint();
        }
    }

    void resized() override
    {
        clearBoxes();
        auto b = layoutLevelPanel (getLocalBounds());
        // WING pixels (I13, 3 colour columns for the 18 colours): the gallery
        // 506 wide, the colours 232, 8 apart; the header rows 64 high, the
        // frames 448.
        const float u = (float) b.getWidth() / 746.0f;
        const float v = (float) b.getHeight() / 598.0f;
        b.removeFromTop (juce::roundToInt (28.0f * v));
        auto left = b.removeFromLeft (juce::roundToInt (506.0f * u));
        b.removeFromLeft (juce::roundToInt (8.0f * u));
        auto right = b;
        link->setBounds (left.removeFromTop (juce::roundToInt (64.0f * v)));
        lightRow = right.removeFromTop (juce::roundToInt (64.0f * v));
        left.removeFromTop (juce::roundToInt (8.0f * v));
        right.removeFromTop (juce::roundToInt (8.0f * v));
        galleryFrame = left.withHeight (juce::roundToInt (448.0f * v));
        colourFrame = right.withHeight (galleryFrame.getHeight());
        auto g = galleryFrame.reduced (juce::roundToInt (8.0f * u));
        categoryColumn = g.removeFromLeft (juce::roundToInt (56.0f * u));
        g.removeFromLeft (juce::roundToInt (4.0f * u));
        grid = g;
    }

    void paint (juce::Graphics& g) override
    {
        HomeScreen::paint (g);
        g.setColour (colours::keyOff);
        g.fillRect (galleryFrame);
        g.fillRect (colourFrame);

        // The category keys: the selected one gold with a black glyph.
        const auto& cats = wingIconCategories();
        categoryKeys.clear();
        const float kh = (float) categoryColumn.getHeight() / (float) cats.size();
        for (int i = 0; i < (int) cats.size(); ++i)
        {
            auto k = juce::Rectangle<float> ((float) categoryColumn.getX(), (float) categoryColumn.getY() + kh * (float) i, (float) categoryColumn.getWidth(), kh)
                         .reduced (0.0f, kh * 0.05f);
            k = k.withSizeKeepingCentre (juce::jmin (k.getWidth(), k.getHeight()), juce::jmin (k.getWidth(), k.getHeight()));
            categoryKeys.push_back (k.toNearestInt());
            const bool sel = i == category;
            paintKeyBody (g, k, sel ? colours::accent : juce::Colour (0xff383838));
            drawIconCategory (g, i, k.reduced (k.getHeight() * 0.2f), sel ? juce::Colours::black : colours::dim);
        }

        // The icon grid: white line icons on black, 1 px dark lines between.
        g.setColour (juce::Colours::black);
        g.fillRect (grid);
        const auto& icons = cats[(size_t) juce::jlimit (0, (int) cats.size() - 1, category)].icons;
        const float cw = (float) grid.getWidth() / 5.0f, ch = (float) grid.getHeight() / 5.0f;
        g.setColour (juce::Colour (0xff282828));
        for (int i = 1; i < 5; ++i)
        {
            g.fillRect ((float) grid.getX() + cw * (float) i, (float) grid.getY(), 1.0f, (float) grid.getHeight());
            g.fillRect ((float) grid.getX(), (float) grid.getY() + ch * (float) i, (float) grid.getWidth(), 1.0f);
        }
        cells.clear();
        const int current = strip.params.icon.load();
        for (int i = 0; i < (int) icons.size() && i < 25; ++i)
        {
            auto cell = juce::Rectangle<float> ((float) grid.getX() + cw * (float) (i % 5), (float) grid.getY() + ch * (float) (i / 5), cw, ch);
            cells.push_back ({ cell.toNearestInt(), icons[(size_t) i].number });
            if (icons[(size_t) i].number != 0)
                drawWingIcon (g, icons[(size_t) i].number, cell.reduced (cell.getWidth() * 0.22f), colours::ink);
            if (icons[(size_t) i].number == current)
            {
                g.setColour (colours::accent);
                g.drawRect (cells.back().first.toFloat().reduced (1.0f), 2.0f);
            }
        }

        // LIGHT: the sun in a gold square while the light bar is on.
        auto lr = lightRow.toFloat();
        g.setColour (colours::panel);
        g.fillRect (lr);
        lr = lr.reduced (lr.getHeight() * 0.12f);
        lightKey = lr.removeFromLeft (lr.getHeight()).toNearestInt();
        const bool lit = strip.params.led.load();
        paintKeyBody (g, lightKey.toFloat(), lit ? colours::accent : colours::keyOff);
        drawIcon (g, Icon::Sun, lightKey.toFloat().reduced (lightKey.getHeight() * 0.18f), lit ? juce::Colours::black : colours::dim);
        g.setColour (lit ? colours::caption : colours::dim);
        g.setFont (homeFont (TypeScale::body, lr.getHeight() * 0.46f));
        g.drawText ("LIGHT", lr.withTrimmedLeft (lr.getHeight() * 0.3f).toNearestInt(), juce::Justification::centredLeft);

        // The colours: three columns of six, 1-6 down the first.
        swatches.clear();
        auto cf = colourFrame.toFloat().reduced (colourFrame.getWidth() * 0.035f);
        const float sw = cf.getWidth() / 3.0f, sh = cf.getHeight() / 6.0f;
        for (int i = 0; i < 18; ++i)
        {
            auto r = juce::Rectangle<float> (cf.getX() + sw * (float) (i / 6), cf.getY() + sh * (float) (i % 6), sw, sh);
            auto swatch = r.reduced (juce::jmax (2.0f, sw * 0.08f));
            swatches.push_back ({ r.toNearestInt(), i + 1 });
            g.setColour (wingColour (i + 1));
            g.fillRect (swatch);
            if (strip.getColourIndex() == i + 1)
            {
                g.setColour (colours::accent);
                g.drawRect (swatch.expanded (juce::jmax (1.5f, sw * 0.04f)), juce::jmax (2.0f, sw * 0.03f));
            }
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const auto pos = e.getPosition();
        for (size_t i = 0; i < categoryKeys.size(); ++i)
            if (categoryKeys[i].contains (pos))
            {
                category = (int) i;
                repaint();
                return;
            }
        for (auto& [area, icon] : cells)
            if (area.contains (pos))
            {
                strip.params.icon = icon;
                repaint();
                return;
            }
        for (auto& [area, colour] : swatches)
            if (area.contains (pos))
            {
                strip.setColourIndex (colour);
                repaint();
                return;
            }
        if (lightKey.contains (pos))
        {
            strip.params.led = ! strip.params.led.load();
            repaint();
        }
    }

private:
    LinkCustomizationRow* link = nullptr;
    int category = 0;
    juce::Rectangle<int> lightRow, galleryFrame, colourFrame, categoryColumn, grid, lightKey;
    std::vector<juce::Rectangle<int>> categoryKeys;
    std::vector<std::pair<juce::Rectangle<int>, int>> cells, swatches;
    std::tuple<int, int, bool> shown { -1, -1, false };
};

/** TAGS (I12): the DCA tiles (4 x 4: the DCA's name on a strip over a big
    D1, the strip lit while assigned), the mute group tiles (4 x 2, MG1...),
    the TALK tag tiles (TALK ON / TALK SOLO, for both talk keys, A or B), the strip's tag
    chips with ADD TAG, and the LEVEL panel. */
class TagsScreen final : public HomeScreen, private juce::Timer
{
public:
    TagsScreen (UiContext& c, Strip& s) : HomeScreen (c, s)
    {
        addTag = &key ("ADD TAG", Icon::Plus);
        addTag->offInk = colours::caption;
        addTag->onClick = [this]
        {
            popups::TextOptions options;
            options.maxLength = 16;
            options.actionText = "ADD";
            popups::editText (*this, "ADD TAG", {}, [safe = juce::Component::SafePointer<TagsScreen> (this)] (const juce::String& text)
            {
                if (safe == nullptr || text.trim().isEmpty())
                    return;
                auto tags = stripTags (safe->strip);
                tags.addIfNotAlreadyThere (text.trim().removeCharacters (","));
                setStripTags (safe->strip, tags);
                safe->repaint();
            }, options);
        };
        addLevelPanel();
        startTimerHz (4);
    }

    void resized() override
    {
        clearBoxes();
        auto b = layoutLevelPanel (getLocalBounds());
        // WING pixels (I12): the tiles 478 wide, the chips column 824-976 of
        // the 752 between the tab row and the LEVEL panel; DCA frame 320 high,
        // mute groups 160, TALK 60.
        const float u = (float) b.getWidth() / 752.0f;
        const float v = (float) b.getHeight() / 598.0f;
        b.removeFromTop (juce::roundToInt (16.0f * v));
        auto left = b.removeFromLeft (juce::roundToInt (478.0f * u));
        dcaFrame = left.removeFromTop (juce::roundToInt (320.0f * v));
        left.removeFromTop (juce::roundToInt (12.0f * v));
        muteFrame = left.removeFromTop (juce::roundToInt (160.0f * v));
        left.removeFromTop (juce::roundToInt (12.0f * v));
        talkFrame = left.removeFromTop (juce::roundToInt (60.0f * v));
        b.removeFromLeft (juce::roundToInt (62.0f * u));
        chipColumn = b.removeFromLeft (juce::roundToInt (152.0f * u));
        chipH = juce::roundToInt (60.0f * v);
        addTag->setBounds (chipColumn.withHeight (chipH).translated (0, (chipH + juce::roundToInt (8.0f * v)) * stripTags (strip).size()));
    }

    void paint (juce::Graphics& g) override
    {
        HomeScreen::paint (g);
        tiles.clear();
        auto& engine = ctx.engine;
        const auto& p = strip.params;
        auto frame = [&g] (juce::Rectangle<int> r)
        {
            g.setColour (colours::keyOff);
            g.fillRect (r);
            return r.reduced (juce::jmax (3, r.getWidth() / 80));
        };
        // A tile: the name strip, the big label, lit (or framed) when on.
        // [I12] the strip text about 60 % of the strip, the big label left-aligned
        // on DCA tiles and centred on the others.
        auto tile = [&g] (juce::Rectangle<float> r, const juce::String& name, const juce::String& big, juce::Colour stripOff, juce::Colour stripOn,
                          bool on, bool twoLines = false, bool centred = false)
        {
            g.setColour (juce::Colours::black);
            g.fillRect (r);
            auto inner = r.reduced (juce::jmax (2.0f, r.getWidth() * 0.04f));
            auto top = inner.removeFromTop (inner.getHeight() * (twoLines ? 0.48f : 0.3f));
            g.setColour (on ? stripOn : stripOff);
            g.fillRect (top);
            g.setColour (on ? juce::Colours::black : colours::caption);
            g.setFont (wingFontPx (top.getHeight() * 0.6f));
            g.drawFittedText (name, top.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.6f);
            g.setColour (on ? colours::ink : colours::caption);
            g.setFont (wingFontPx (inner.getHeight() * (twoLines ? 0.62f : 0.46f)));
            g.drawFittedText (big, inner.toNearestInt().reduced (4, 0), twoLines || centred ? juce::Justification::centred : juce::Justification::centredLeft, 1, 0.6f);
        };

        auto dcas = frame (dcaFrame).toFloat();
        const float dw = dcas.getWidth() / 4.0f, dh = dcas.getHeight() / 4.0f;
        for (int i = 0; i < kNumDcas; ++i)
        {
            auto r = juce::Rectangle<float> (dcas.getX() + dw * (float) (i % 4), dcas.getY() + dh * (float) (i / 4), dw, dh).reduced (3.0f);
            const bool on = (p.dcaMask.load() >> i) & 1u;
            tile (r, engine.getDcaName (i).toUpperCase(), "D" + juce::String (i + 1), colours::dcaTag, colours::fader, on);
            tiles.push_back ({ r.toNearestInt(), i });
        }
        auto mutes = frame (muteFrame).toFloat();
        const float mw = mutes.getWidth() / 4.0f, mh = mutes.getHeight() / 2.0f;
        for (int i = 0; i < kNumMuteGroups; ++i)
        {
            auto r = juce::Rectangle<float> (mutes.getX() + mw * (float) (i % 4), mutes.getY() + mh * (float) (i / 4), mw, mh).reduced (3.0f);
            const bool on = (p.muteGroupMask.load() >> i) & 1u;
            tile (r, engine.getMuteGroupName (i).toUpperCase(), "MG" + juce::String (i + 1), colours::muteTag, colours::mute, on, false, true);
            tiles.push_back ({ r.toNearestInt(), 100 + i });
        }
        // TALK tags (manual p. 31): TALK ON unmutes and TALK SOLO solos the
        // strip while talkback is on (A / B: only for that talk key). The
        // tiles' order is the tag bits' (ld::TalkTag).
        auto talk = frame (talkFrame).toFloat();
        static const char* talkNames[][2] = { { "TALK", "SOLO" }, { "TALK", "ON" }, { "TALK A", "SOLO" }, { "TALK A", "ON" }, { "TALK B", "SOLO" }, { "TALK B", "ON" } };
        const float tw = talk.getWidth() / 6.0f;
        const auto talkTags = strip.getTalkTags();
        for (int i = 0; i < 6; ++i)
        {
            auto r = juce::Rectangle<float> (talk.getX() + tw * (float) i, talk.getY(), tw, talk.getHeight()).reduced (3.0f);
            tile (r, talkNames[i][0], talkNames[i][1], colours::talkOff, colours::post, (talkTags >> i) & 1u, true);
            tiles.push_back ({ r.toNearestInt(), 200 + i });
        }

        // The tag chips, each with its remove key.
        chips.clear();
        auto column = chipColumn.toFloat();
        for (auto& tag : stripTags (strip))
        {
            auto row = column.removeFromTop ((float) chipH);
            column.removeFromTop ((float) chipH * 0.14f);
            g.setColour (colours::keyOff);
            g.fillRect (row);
            auto chip = row.reduced (row.getHeight() * 0.14f, row.getHeight() * 0.2f);
            const float w = paintTagChip (g, chip, tag);
            const float d = chip.getHeight() * 0.5f;
            auto remove = juce::Rectangle<float> (d, d).withCentre ({ chip.getX() + w, chip.getY() + d * 0.3f });
            g.setColour (juce::Colours::black);
            g.fillEllipse (remove);
            g.setColour (colours::ink);
            g.drawEllipse (remove.reduced (1.0f), 1.2f);
            const auto rc = remove.getCentre();
            const float s = d * 0.18f;
            g.drawLine (rc.x - s, rc.y - s, rc.x + s, rc.y + s, 1.5f);
            g.drawLine (rc.x - s, rc.y + s, rc.x + s, rc.y - s, 1.5f);
            chips.push_back ({ remove.expanded (3.0f).toNearestInt(), tag });
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const auto pos = e.getPosition();
        for (auto& [area, index] : tiles)
            if (area.contains (pos))
            {
                if (index >= 200)
                {
                    strip.setTalkTags (strip.getTalkTags() ^ (1u << (index - 200)));
                    repaint();
                    return;
                }
                auto& mask = index >= 100 ? strip.params.muteGroupMask : strip.params.dcaMask;
                mask = mask.load() ^ (1u << (index % 100));
                repaint();
                return;
            }
        for (auto& [area, tag] : chips)
            if (area.contains (pos))
            {
                auto tags = stripTags (strip);
                tags.removeString (tag);
                setStripTags (strip, tags);
                resized();
                repaint();
                return;
            }
    }

private:
    void timerCallback() override
    {
        const auto now = std::make_tuple (strip.params.dcaMask.load(), strip.params.muteGroupMask.load(),
                                          stripTags (strip).joinIntoString (",") + "|" + juce::String (strip.getTalkTags()));
        if (now != shown)
        {
            shown = now;
            resized();
            repaint();
        }
    }

    IconKey* addTag = nullptr;
    juce::Rectangle<int> dcaFrame, muteFrame, talkFrame, chipColumn;
    int chipH = 30;
    std::vector<std::pair<juce::Rectangle<int>, int>> tiles;
    std::vector<std::pair<juce::Rectangle<int>, juce::String>> chips;
    std::tuple<uint32_t, uint32_t, juce::String> shown;
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
        // On Channel Home (WING pixels): thumbnails 8-104, the key's pointer at
        // 116-124, keys 124-192, then the black gap up to the content at 284
        // (the tag chips and, in wrench mode, the TAP flag). The slot screens
        // show only the thumbnails.
        const int unitW = keys ? 276 : 96;
        const int thumbW = b.getWidth() * 96 / unitW;
        const int keyW = keys ? b.getWidth() * 68 / unitW : 0;
        const int keyX = b.getX() + b.getWidth() * 116 / unitW;
        gapArea = keys ? b.withLeft (keyX + keyW) : juce::Rectangle<int>();
        keyLeft = keyX;
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

        // The strip's tag chips right of the HOME key, above the keys (I12).
        if (keys && ! rows.empty())
        {
            const auto home = rows.front().thumb;
            const float h = juce::jlimit (9.0f, 16.0f, (float) home.getHeight() * 0.26f);
            auto area = juce::Rectangle<float> ((float) keyLeft, (float) home.getY() + 2.0f, (float) (getWidth() - keyLeft) - 2.0f, h);
            for (auto& tag : stripTags (*strip))
            {
                if (area.getY() + h > (float) home.getBottom())
                    break;
                paintTagChip (g, area, tag.toUpperCase());
                area.translate (0.0f, h + 2.0f);
            }
        }

        // The fader bar: the fader level (red when muted), and the TAP point.
        g.setColour (colours::keyOff);
        g.fillRect (faderBar);
        const float pos = fader::dbToPos (p.faderDb.load());
        g.setColour (p.mute.load() ? Colours::mute : colours::fader);
        g.fillRect (faderBar.withWidth (faderBar.getWidth() * pos));
        if (const int tap = tapIndex(); tap >= 0 && tap < (int) tapY.size())
        {
            const float d = juce::jlimit (6.0f, 11.0f, faderBar.getHeight() * 2.5f);
            const float ty = draggingTap ? (float) dragY : tapY[(size_t) tap];
            const float dotX = (float) thumbWidth * 0.5f;
            // "A red dot with a gray line connected to the bus send slot": along
            // the right edge of the thumbnails down to SENDS.
            if (auto sends = rowOf (HomeTab::Sends); ! sends.isEmpty())
            {
                const float edge = (float) thumbWidth + juce::jmax (1.5f, (float) thumbWidth * 0.02f);
                juce::Path line;
                line.startNewSubPath (dotX, ty);
                line.lineTo (edge, ty);
                line.lineTo (edge, (float) sends.getY() + 1.0f);
                line.lineTo ((float) sends.getRight() - 1.0f, (float) sends.getY() + 1.0f);
                g.setColour (colours::chipOff);
                g.strokePath (line, juce::PathStrokeType (1.0f));
            }
            if (editing && ! gapArea.isEmpty())
                paintTapFlag (g, dotX, ty);
            g.setColour (colours::tap);
            g.fillEllipse (dotX - d * 0.5f, ty - d * 0.5f, d, d);
        }

        if (! wrench.isEmpty())
        {
            paintKeyFrame (g, wrench.toFloat(), editing ? colours::accent : colours::keyOff, Icon::Wrench, editing);
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
                                         && (juce::Point<float> ((float) thumbWidth * 0.5f, tapY[(size_t) tap]).getDistanceFrom (pos.toFloat()) < 10.0f
                                             || tapFlagArea (tapY[(size_t) tap]).expanded (2.0f).contains (pos.toFloat())))
        {
            draggingTap = true;
            dragY = pos.y;
            return;
        }
        for (auto& r : rows)
            if ((r.thumb.contains (pos) || r.key.contains (pos)) && preIndex (r.tab) >= 0)
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
        if (t == HomeTab::Gate)     return colours::accent;
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

    juce::Rectangle<int> rowOf (HomeTab t) const
    {
        for (auto& r : rows)
            if (r.tab == t)
                return r.thumb;
        return {};
    }

    /** Wrench mode: the TAP point as a red flag in the gap left of the
        content, joined to the dot by a red line. Drag it to move the tap. */
    juce::Rectangle<float> tapFlagArea (float ty) const
    {
        auto gap = gapArea.toFloat();
        const float h = juce::jlimit (12.0f, 22.0f, gap.getWidth() * 0.3f);
        return { gap.getX() + gap.getWidth() * 0.18f, ty - h * 0.5f, gap.getWidth() * 0.7f, h };
    }

    void paintTapFlag (juce::Graphics& g, float dotX, float ty) const
    {
        const auto flag = tapFlagArea (ty);
        g.setColour (colours::tap);
        g.fillRect (juce::Rectangle<float> (dotX, ty - 0.75f, flag.getX() - dotX, 1.5f));
        juce::Path shape;
        const float tip = flag.getHeight() * 0.5f;
        shape.startNewSubPath (flag.getX(), flag.getCentreY());
        shape.lineTo (flag.getX() + tip, flag.getY());
        shape.lineTo (flag.getRight(), flag.getY());
        shape.lineTo (flag.getRight(), flag.getBottom());
        shape.lineTo (flag.getX() + tip, flag.getBottom());
        shape.closeSubPath();
        g.fillPath (shape);
        const float dot = flag.getHeight() * 0.22f;
        g.setColour (juce::Colours::black);
        g.fillEllipse (juce::Rectangle<float> (dot, dot).withCentre ({ flag.getX() + tip + dot * 0.5f, flag.getCentreY() }));
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (flag.getHeight() * 0.72f, juce::Font::bold));
        g.drawText ("TAP", flag.withTrimmedLeft (tip + dot * 1.5f).toNearestInt(), juce::Justification::centred);
    }

    void paintKey (juce::Graphics& g, const Row& r) const
    {
        const bool on = isOn (r.tab);
        auto k = r.key.toFloat();
        if (editing && r.tab == HomeTab::PostInsert)
        {
            // INS 2 can't move: its key shows a dash while editing.
            g.setColour (colours::keyFrame);
            g.fillRoundedRectangle (k, 3.0f);
            g.setColour (colours::keyOff);
            g.fillRoundedRectangle (k.reduced (juce::jmax (2.0f, k.getWidth() * 0.1f)), 2.0f);
            g.setColour (colours::dim);
            g.fillRect (juce::Rectangle<float> (k.getWidth() * 0.34f, 2.0f).withCentre (k.getCentre()));
        }
        else
            paintKeyFrame (g, k, on ? keyColour (r.tab) : colours::keyOff, keyIcon (r.tab), on);
        if (editing && preIndex (r.tab) >= 0)
        {
            g.setColour (colours::accent);
            g.drawRoundedRectangle (k.reduced (0.5f), 3.0f, juce::jmax (1.5f, k.getWidth() * 0.04f));
        }
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
            g.setColour (colours::thumb);
            g.fillRoundedRectangle (t, 2.0f);
            g.setColour (colours::dim);
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
            g.setColour (colours::accent);
            g.drawRoundedRectangle (t.reduced (1.0f), 2.0f, 2.0f);
        }
    }

    /** A thumbnail chip: grey with dark text, lit in `onColour` with black text. */
    static void chip (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, bool on, juce::Colour onColour = colours::caption)
    {
        g.setColour (on ? onColour : colours::chipOff);
        g.fillRoundedRectangle (r, juce::jmin (2.0f, r.getHeight() * 0.15f));
        g.setColour (on ? juce::Colours::black : colours::panel);
        g.setFont (juce::FontOptions (juce::jlimit (6.0f, 22.0f, r.getHeight() * 0.86f), juce::Font::bold));
        g.drawFittedText (text, r.toNearestInt(), juce::Justification::centred, 1, 0.55f);
    }

    static void miniMeter (juce::Graphics& g, juce::Rectangle<float> r, float level, juce::Colour c)
    {
        const int segments = juce::jlimit (4, 14, (int) (r.getHeight() / 3.0f));
        const float h = r.getHeight() / (float) segments;
        for (int i = 0; i < segments; ++i)
        {
            const bool lit = level > (float) i / (float) segments;
            g.setColour (lit ? c : colours::panel);
            g.fillRect (r.getX(), r.getBottom() - (float) (i + 1) * h + 0.5f, r.getWidth(), h - 1.0f);
        }
    }

    /** The thumbnails as on the 3.1 screens (WING pixels in a 96 x 64 box): INPUT
        chips in two columns of three with the meter at the right; GATE / COMP
        with the model chip, the state LED, S/C and X/O and the GR meter; INS
        with the slot chip and the effect; the EQ curve; MAIN; SENDS. */
    void paintThumb (juce::Graphics& g, HomeTab tab, juce::Rectangle<float> t) const
    {
        auto& p = strip->params;
        auto& fx = page.ctx.engine.getFx();
        const float u = t.getWidth() / 96.0f, v = t.getHeight() / 64.0f;
        auto at = [&t, u, v] (float x, float y, float w, float h) { return juce::Rectangle<float> (t.getX() + x * u, t.getY() + y * v, w * u, h * v); };
        auto meterArea = at (80.0f, 5.0f, 10.0f, 54.0f);
        auto r = t.reduced (juce::jmax (2.0f, t.getHeight() * 0.07f));
        const float meterW = juce::jlimit (3.0f, 7.0f, r.getWidth() * 0.08f);
        const float chipH = 15.0f * v;

        switch (tab)
        {
            case HomeTab::Input:
            {
                const int n = strip->getNumChannels();
                const float mw = meterArea.getWidth() / (float) n;
                for (int ch = 0; ch < n; ++ch)
                {
                    const float level = MeterComponent::dbToProportion (dsp::gainToDb (page.ctx.meters.get (strip, true, ch)));
                    miniMeter (g, meterArea.removeFromLeft (mw).withTrimmedRight (n > 1 ? 1.0f : 0.0f), level, Colours::meterGreen);
                }
                auto cell = [&at] (int col, int row) { return at (6.0f + 30.0f * (float) col, 5.0f + 20.0f * (float) row, 24.0f, 15.0f); };
                const bool trimmed = std::abs (p.input.trimDb.load()) > 0.05f;
                switch (strip->getKind())
                {
                    case StripKind::Channel:
                    {
                        const int tool = p.filter.toolModel.load();
                        chip (g, cell (0, 0), "48V", false);
                        chip (g, cell (1, 0), "INV", p.input.invert.load());
                        chip (g, cell (0, 1), "LC", p.filter.lowCut.load(), colours::eqOn);
                        chip (g, cell (1, 1), "HC", p.filter.highCut.load(), colours::eqOn);
                        chip (g, cell (0, 2), toolFilterToken (tool), p.filter.tool.load(), toolFilterColour (tool));
                        chip (g, cell (1, 2), "DLY", p.input.delayOn.load());
                        break;
                    }
                    case StripKind::Aux:
                        chip (g, cell (1, 0), "INV", p.input.invert.load());
                        chip (g, cell (1, 1), "TRIM", trimmed);
                        chip (g, cell (1, 2), "DLY", p.input.delayOn.load());
                        break;
                    case StripKind::Bus:
                    case StripKind::Main:
                    case StripKind::Matrix:
                        chip (g, cell (0, 0), "M", p.mono.load());
                        chip (g, cell (1, 0), "INV", p.input.invert.load());
                        chip (g, cell (1, 1), "TRIM", trimmed);
                        if (strip->getKind() == StripKind::Bus)
                            chip (g, cell (1, 2), "DLY", p.input.delayOn.load());
                        break;
                }
                return;
            }

            case HomeTab::Gate:
            case HomeTab::Dynamics:
            {
                const bool gate = tab == HomeTab::Gate;
                auto& slot = gate ? p.gate : p.dynamics;
                const float grDb = gate ? strip->meters.gateReductionDb.load() : strip->meters.dynReductionDb.load();
                miniMeter (g, meterArea, juce::jlimit (0.0f, 1.0f, grDb / (gate ? 40.0f : 24.0f)), Colours::meterRed);
                const auto& model = models::model (models::Dynamics, slot.model.load());
                const bool on = slot.on.load();
                chip (g, at (6.0f, 5.0f, 54.0f, 20.0f), model.token, on, colours::accent);
                // The state LED: the gate open, the compressor over its threshold.
                const bool lit = on && (gate ? grDb < 3.0f && page.ctx.meters.get (strip, true, 0) > 0.001f : grDb > 0.5f);
                g.setColour (lit ? juce::Colour (0xffe0e800) : juce::Colours::black);
                g.fillRect (at (66.0f, 5.0f, 7.0f, 7.0f));
                chip (g, at (6.0f, 44.0f, 24.0f, 15.0f), "S/C", slot.scSource.load() >= 0);
                if (! gate)
                    chip (g, at (36.0f, 44.0f, 24.0f, 15.0f), "X/O", slot.xoKind.load() > 0);
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
                chip (g, at (6.0f, 5.0f, 54.0f, 20.0f), text, isOn (tab) || (mode > 0 && ins.on.load()), colours::accent);
                // Slots 1-8 hold the premium effects: a small diamond.
                if (mode == 0 && slot >= 0 && slot < 8)
                    drawIcon (g, Icon::Premium, at (66.0f, 5.0f, 18.0f, 18.0f), isOn (tab) ? colours::accent : colours::dim);
                juce::String device;
                if (mode > 0)
                    device = mode == 1 ? "X" : "Y";
                else if (auto* plugin = slot >= 0 ? fx.getPlugin (slot) : nullptr)
                    device = plugin->getName().toUpperCase();
                else if (slot >= 0 && fx.getModel (slot) > ld::kFxExt)
                    device = models::model (models::Fx, fx.getModel (slot)).token;
                g.setColour (colours::ink);
                g.setFont (juce::FontOptions (juce::jlimit (8.0f, 20.0f, 18.0f * v), juce::Font::bold));
                g.drawFittedText (device, at (6.0f, 38.0f, 84.0f, 22.0f).toNearestInt(), juce::Justification::centredRight, 1, 0.6f);
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
                    const bool on = p.eq.on.load();
                    juce::Path fill (curve);
                    fill.lineTo (r.getRight(), r.getCentreY());
                    fill.lineTo (r.getX(), r.getCentreY());
                    fill.closeSubPath();
                    g.setColour (on ? colours::eqFill : colours::panel);
                    g.fillPath (fill);
                    g.setColour (on ? colours::eqCurve : colours::dim);
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
                        g.setColour (p.mains[i].on.load() ? colours::fader : colours::panel);
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
                // A bar per send, filled with its level in the send mode's colour
                // (TAP red, POST green); the 8 matrices right-aligned on top.
                auto bars = [&g] (juce::Rectangle<float> area, int n, int slots, std::function<std::pair<float, juce::Colour> (int)> level)
                {
                    const float w = area.getWidth() / (float) juce::jmax (1, slots);
                    area.removeFromLeft (w * (float) (slots - n));
                    for (int i = 0; i < n; ++i)
                    {
                        auto bar = juce::Rectangle<float> (area.getX() + (float) i * w, area.getY(), juce::jmax (1.0f, w - 1.0f), area.getHeight());
                        g.setColour (colours::outline);
                        g.fillRect (bar);
                        const auto [l, c] = level (i);
                        if (l > 0.0f)
                        {
                            g.setColour (c);
                            g.fillRect (bar.withTop (bar.getBottom() - bar.getHeight() * l).reduced (bar.getWidth() * 0.2f, 0.0f));
                        }
                    }
                };
                g.setColour (juce::Colours::black);
                g.fillRect (top.withTrimmedRight (top.getWidth() * 0.5f + 1.0f));
                if (strip->getKind() != StripKind::Matrix)
                    bars (top, kNumMatrices, 16, [&p] (int i)
                    {
                        const auto c = p.matrix[i].pre.load() ? colours::tap : colours::post;
                        return std::make_pair (p.matrix[i].on.load() ? fader::dbToPos (p.matrix[i].levelDb.load()) : 0.0f, c);
                    });
                if (strip->getKind() != StripKind::Main)
                {
                    const int n = juce::jmin (16, (int) page.ctx.engine.getBuses().size());
                    bars (bottom, n, 16, [this] (int i)
                    {
                        auto* send = strip->sends.find (i);
                        if (send == nullptr || ! send->enabled.load() || send->mode.load() == 2)
                            return std::make_pair (0.0f, colours::post);
                        return std::make_pair (fader::dbToPos (send->levelDb.load()), sendModeColour (send->mode.load()));
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
        // The 24 process orders, then the TAP POINT (a dim title row) and its taps.
        auto* s = strip;
        juce::StringArray items;
        popups::ListOptions options;
        options.columns = 2;
        options.caption = "PROCESS ORDER";
        const auto orders = models::list (models::ProcessOrders);
        for (int i = 0; i < orders.size(); ++i)
        {
            juce::StringArray names;
            for (auto letter : orders[i])
                names.add (letter == 'G' ? "GATE" : letter == 'E' ? "EQ" : letter == 'D' ? "DYN" : "INS 1");
            items.add (names.joinIntoString (" > "));
        }
        const int numOrders = items.size();
        options.disabled.push_back (items.size());
        items.add ("TAP POINT:");
        const auto tapNames = tapPointNames();
        for (auto& t : tapNames)
            items.add (t);
        if (! s->hasSelectableTap())
            for (int i = 0; i < items.size(); ++i)
                options.disabled.push_back (i);
        if (orderAnchor.getParentComponent() == nullptr)
        {
            orderAnchor.setInterceptsMouseClicks (false, false);
            addChildComponent (orderAnchor);
        }
        orderAnchor.setBounds (wrench);
        juce::Component::SafePointer<juce::Component> safe (this);
        popups::chooseFrom (orderAnchor, items, s->params.processOrder.load(), [this, safe, numOrders] (int i)
        {
            if (safe == nullptr || strip == nullptr)
                return;
            if (i < numOrders)
            {
                strip->params.processOrder = i;
                setStrip (strip);
            }
            else if (i > numOrders)
                strip->params.tap = i - numOrders - 1;
        }, options);
    }

    HomePage& page;
    Strip* strip = nullptr;
    std::vector<Row> rows;
    std::vector<float> tapY;
    juce::Rectangle<float> faderBar;
    juce::Rectangle<int> wrench, gapArea;
    juce::Component orderAnchor; // where the process order list opens (not shown)
    int keyLeft = 0;
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
    // 3.1: PREV CH and NEXT CH, then INIT and COPY on every slot screen;
    // SETTINGS on OVERVIEW, FILTER and EQ.
    std::vector<StatusKey> keys;
    keys.push_back ({ "PREV CH", Icon::ChevronLeft, false, [this] { stepStrip (-1); } });
    keys.push_back ({ "NEXT CH", Icon::ChevronRight, false, [this] { stepStrip (1); } });
    keys.push_back ({ "INIT", Icon::Reset, overlayKind == OverlayKind::Init, [this] { openOverlay (OverlayKind::Init); } });
    keys.push_back ({ "COPY", Icon::Copy, overlayKind == OverlayKind::Copy, [this] { openOverlay (OverlayKind::Copy); } });
    if (content != nullptr && content->hasSettings())
        keys.push_back ({ "SETTINGS", Icon::Gear, overlayKind == OverlayKind::Settings, [this] { openOverlay (OverlayKind::Settings); } });
    return keys;
}

void HomePage::stepStrip (int delta)
{
    if (auto* next = adjacentStrip (ctx.engine, strip, delta); next != nullptr && ctx.selectStrip)
        ctx.selectStrip (next);
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

    // The toolbar: thumbnails, keys and the black gap on Channel Home (the
    // content starts at 284 of 1280 WING pixels), only the thumbnails on the
    // slot screens (content from 112).
    const bool overviewGroup = isOverviewGroup (current);
    const int screenW = getWidth();
    const int thumbW = juce::jlimit (40, 120, screenW * 75 / 1000);
    const int colW = overviewGroup ? thumbW * 276 / 96 : thumbW;
    column->setBounds (b.removeFromLeft (colW));
    b.removeFromLeft (juce::jmax (4, screenW / 160));

    if (overviewGroup)
    {
        // The tab row sits level with the HOME key.
        // Tabs about 160 of the 988 WING pixels each, SAFE 120 at the right.
        auto top = b.removeFromTop (juce::roundToInt ((float) getHeight() / 9.45f) - juce::jmax (2, getHeight() / 80));
        safeKey->setBounds (top.removeFromRight (top.getWidth() * 120 / 988));
        const int w = top.getWidth() * 160 / 868;
        for (auto* t : topTabs)
            t->setBounds (top.removeFromLeft (w).withTrimmedRight (2));
        b.removeFromTop (juce::jmax (2, getHeight() / 80));
    }

    if (content != nullptr)
        content->setBounds (b);
}

} // namespace livedaw::ui
