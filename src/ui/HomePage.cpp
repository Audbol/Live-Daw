#include "ui/HomePage.h"
#include "dsp/DspUtils.h"
#include "ui/Faceplate.h"
#include "ui/FaderStrip.h"
#include "ui/HomeIcons.h"
#include "ui/Icons.h"
#include "ui/MeterBallistics.h"
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
            // The source as ROUTING draws it, in a black frame [I1, D16].
            auto r = getLocalBounds().toFloat();
            g.setColour (juce::Colours::black);
            g.fillRect (r);
            const int left = strip.params.inputLeft.load();
            const bool st = strip.params.stereo.load();
            const int right = strip.params.inputRight.load() >= 0 ? strip.params.inputRight.load() : left + 1;
            paintInputSourceTile (g, r.reduced (juce::jmax (1.5f, wingPx (4.0f))), ctx.engine, left, right, st, hasSignal());
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (getLocalBounds().contains (e.getPosition()) && onOpen)
                onOpen();
        }

    private:
        void timerCallback() override
        {
            const int index = strip.params.inputLeft.load() >= 0 ? MixerEngine::sourceIndex (strip.params.inputLeft.load()) : -1;
            const auto now = portName (ctx, strip) + (strip.params.stereo.load() ? "S" : "") + (hasSignal() ? "s" : "")
                             + (index >= 0 ? ctx.engine.getSourceName (index) : juce::String());
            if (now != shown)
            {
                shown = now;
                shownPort = portName (ctx, strip);
                repaint();
            }
        }

        bool hasSignal() const { return ctx.meters.get (&strip, true, 0) > juce::Decibels::decibelsToGain (-50.0f); }

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
            // [I1] stacked under TRIM, 160 x 50 WING pixels, 14 apart, from the right.
            auto r = getLocalBounds().toFloat();
            const int n = (int) tabs.size();
            const float pitch = juce::jmin (r.getHeight() / (float) n, r.getWidth() * 64.0f / 160.0f);
            for (int i = 0; i < n; ++i)
            {
                const bool curve = (tabs[(size_t) i] == HomeTab::Gate && isNativeDynamics (strip.params.gate.model.load()))
                                   || (tabs[(size_t) i] == HomeTab::Dynamics && isNativeDynamics (strip.params.dynamics.model.load()));
                auto cell = juce::Rectangle<float> (r.getX(), r.getY() + (float) i * pitch, r.getWidth(), pitch * 50.0f / 64.0f);
                cell = cell.withSizeKeepingCentre (juce::jmin (cell.getWidth(), cell.getHeight() * (curve ? 1.5f : 3.2f)), cell.getHeight());
                items.push_back ({ tabs[(size_t) i], cell.withX (r.getRight() - cell.getWidth()) });
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
        main; click to switch the send. Wider than tall it is a row with a PRE
        badge over each main (SOLO MODE STUDIO, manual 5.5 p. 46, Fig. 38),
        lit while that send is pre-fader. */
    class MainSquares final : public juce::Component, private juce::Timer
    {
    public:
        explicit MainSquares (StripParams& p) : params (p) { startTimerHz (6); }

        bool isRow() const { return getWidth() > getHeight(); }

        void paint (juce::Graphics& g) override
        {
            const bool row = isRow();
            const float h = row ? (float) getHeight() * 0.55f : (float) getHeight() / (float) kNumMains;
            const float w = row ? (float) getWidth() / (float) kNumMains : (float) getWidth();
            const float s = juce::jmin (h * 0.9f, w * 0.9f);
            for (int i = 0; i < kNumMains; ++i)
            {
                auto cell = row ? juce::Rectangle<float> ((float) i * w, (float) getHeight() - h, w, h) : juce::Rectangle<float> (0.0f, (float) i * h, w, h);
                auto r = cell.withSizeKeepingCentre (s, s);
                if (row)
                {
                    const bool pre = params.mains[i].pre.load();
                    auto badge = juce::Rectangle<float> (cell.getX(), 0.0f, w, (float) getHeight() - h).withSizeKeepingCentre (s, ((float) getHeight() - h) * 0.7f);
                    g.setColour (colours::well);
                    g.fillRoundedRectangle (badge, 2.0f);
                    g.setColour (pre ? Colours::text : Colours::textDim);
                    g.setFont (wingFontFit (TypeScale::caption, badge.getHeight() * 0.85f, WingWeight::SemiBold));
                    g.drawText ("PRE", badge.toNearestInt(), juce::Justification::centred);
                }
                const bool on = params.mains[i].on.load();
                g.setColour (on ? juce::Colour (0xff00a4f8) : colours::well); // WING 3.1: a lit main in light blue
                g.fillRect (r);
                g.setColour (on ? juce::Colours::black : Colours::textDim);
                g.setFont (wingFontFit (TypeScale::body, s * 0.8f, WingWeight::SemiBold));
                g.drawText (juce::String (i + 1), r.toNearestInt(), juce::Justification::centred);
            }
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            const int i = juce::jlimit (0, kNumMains - 1, isRow() ? e.x * kNumMains / juce::jmax (1, getWidth()) : e.y * kNumMains / juce::jmax (1, getHeight()));
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
/** The parameter bar of the four HOME tabs [I1, I12, I13]: the source GAIN
    (input and aux channels), TRIM, WIDTH and PAN. The targets are the
    controls the knobs' thumbs point at (none on ICON/COLOR, NAME, TAGS). */
std::vector<HomeKnob> homeTabKnobs (UiContext& ctx, Strip& strip, juce::Component* gainTarget = nullptr, juce::Component* trimTarget = nullptr,
                                    juce::Component* panTarget = nullptr)
{
    auto& p = strip.params;
    HomeKnob gain;
    if (strip.isInput())
        gain = { sourceGainBinding (ctx, strip), gainStyle (gainTarget) };
    return { gain, { wingTrim ("TRIM", p.input.trimDb), centreStyle (trimTarget) }, {}, {},
             { wingWidth ("WIDTH", p.width), widthStyle (panTarget) }, { wingPan ("PAN", p.pan), thumbStyle (panTarget) } };
}

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
            // The source mute belongs to the Sources (manual 5.4): the source on
            // the strip's MAIN input, both sources of a pair, as ROUTING > SOURCES.
            sourceMute = &key ("MUTE", std::nullopt, colours::mute);
            sourceMute->setTooltip ("Mutes the channel's source (ROUTING > SOURCES); the channel MUTE is in the LEVEL panel");
            sourceMute->isOn = [this]
            {
                const auto* src = sourceOf (strip);
                return src != nullptr && src->mute.load();
            };
            sourceMute->onClick = [this]
            {
                const auto* src = sourceOf (strip);
                if (src == nullptr)
                    return;
                const bool mute = ! src->mute.load();
                forEachSourceOf (strip, [mute] (ld::SourceParams& each) { each.mute = mute; });
            };
        }
        if (! outputKind)
        {
            // [I1] the MAIN source's +48V and GAIN beside its tile.
            phantom = &key ({}, Icon::Power, colours::mute);
            phantom->setTooltip ("+48 V phantom power of the channel's source (ROUTING > SOURCES): hold 2 s to switch");
            phantom->isOn = [this]
            {
                const auto* src = sourceOf (strip);
                return src != nullptr && src->phantom.load();
            };
            // As the console's, the key switches only when held (kPhantomHoldSeconds).
            phantom->switchOnHold ([this]
            {
                const auto* src = sourceOf (strip);
                if (src == nullptr)
                    return;
                const bool on = ! src->phantom.load();
                forEachSourceOf (strip, [on] (ld::SourceParams& each) { each.phantom = on; });
            });
            phantomLabel = &label ("+48V", 12.0f, colours::dim);
            phantomLabel->setJustificationType (juce::Justification::centredTop);
            gain = &make<KnobBox> (sourceGainBinding (ctx, strip));
            gain->ringColour = KnobColours::gold;
            gain->valueColour = KnobColours::gold;
        }
        trim = &make<KnobBox> (wingTrim ("TRIM", p.input.trimDb));
        trim->centredRange = true;
        processors = &make<ProcessorThumbs> (ctx, strip);

        if (strip.getKind() == StripKind::Channel)
        {
            loCut = &key ("LO CUT", Icon::LowCut, colours::eqOn);
            bindKey (*loCut, p.filter.lowCut);
            hiCut = &key ("HI CUT", Icon::HighCut, colours::eqOn);
            bindKey (*hiCut, p.filter.highCut);
            tool = &key (toolFilterName (p.filter.toolModel.load()), toolFilterIcon (p.filter.toolModel.load()), toolFilterColour (p.filter.toolModel.load()));
            bindKey (*tool, p.filter.tool);
            for (auto* k : { loCut, hiCut, tool })
                k->labelBeside = true;
        }

        level = &make<LevelPanel> (ctx, strip);

        graph = &make<ResponseGraph>();
        graph->setStrip (&strip, ResponseGraph::Eq);
        graph->setOverview (true);
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
        soloBusLabel = &label ("SOLO BUS", 14.0f, Colours::caption);
        soloBusLabel->setJustificationType (juce::Justification::centred);
        // SOLO SAFE (manual 5.5 p. 46, Fig. 39): with SOLO MODE SIP the input
        // and aux channels (the strips SIP mutes) show an open and a closed
        // lock in place of SOLO BUS; closed, SIP leaves the channel unmuted.
        if (strip.isInput())
        {
            safeOff = &key ({}, Icon::LockOpen, colours::soloBus);
            safeOff->isOn = [&p] { return ! p.soloSafe.load(); };
            safeOff->onClick = [&p] { p.soloSafe = false; };
            safeOff->setTooltip ("Not solo safe: SIP mutes the channel while another one is soloed");
            safeOn = &key ({}, Icon::LockClosed, colours::soloBus);
            safeOn->isOn = [&p] { return p.soloSafe.load(); };
            safeOn->onClick = [&p] { p.soloSafe = true; };
            safeOn->setTooltip ("Solo safe: SIP never mutes the channel");
            safeLabel = &label ("SOLO SAFE", 14.0f, Colours::caption);
            safeLabel->setJustificationType (juce::Justification::centred);
        }
        showSoloMode();
        startTimer (400);
    }

    bool hasSettings() const override { return true; }

    std::vector<HomeKnob> knobs() override { return homeTabKnobs (ctx, strip, gain, trim, pan); }

    void resized() override
    {
        clearBoxes();
        // WING 3.1 pixels: INPUT 284-884 and FILTER 892-1048 (y 152-448), EQ
        // and MAIN under them (456-752), LEVEL 1056-1272 the full height;
        // headers 64 tall, 8 px gaps.
        inputArea = panelAt (284.0f, 152.0f, 600.0f, 296.0f);
        filterArea = panelAt (892.0f, 152.0f, 156.0f, 296.0f);
        levelArea = panelAt (1056.0f, 152.0f, 216.0f, 600.0f);
        eqArea = panelAt (284.0f, 456.0f, 600.0f, 296.0f);
        mainArea = panelAt (892.0f, 456.0f, 156.0f, 296.0f);

        // INPUT: the source tile 112 px square at (300, 240) with MUTE (84 x
        // 56) under it at (314, 372); +48V (48 px) at (428, 272) with its
        // caption under it; GAIN and TRIM (176 x 64) at x 496 and 692, y 240.
        box (inputArea, "INPUT", true);
        if (feed != nullptr)
            feed->setBounds (wingRect (300.0f, 240.0f, 280.0f, 192.0f));
        else
        {
            source->setBounds (wingRect (300.0f, 240.0f, 112.0f, 112.0f));
            sourceMute->setBounds (wingRect (314.0f, 372.0f, 84.0f, 56.0f));
        }
        if (phantom != nullptr)
        {
            phantom->setBounds (wingRect (428.0f, 272.0f, 48.0f, 48.0f));
            phantomLabel->setBounds (wingRect (416.0f, 324.0f, 72.0f, 28.0f));
        }
        trim->setBounds (wingRect (692.0f, 240.0f, 176.0f, 64.0f));
        if (gain != nullptr)
            gain->setBounds (wingRect (496.0f, 240.0f, 176.0f, 64.0f));
        processors->setBounds (wingRect (gain != nullptr ? 496.0f : 692.0f, 316.0f, gain != nullptr ? 364.0f : 168.0f, 116.0f));

        // FILTER: 52 px keys at x 908, y 240 / 310 / 380, the name beside each.
        if (loCut != nullptr)
        {
            box (filterArea, "FILTER", true);
            float y = 240.0f;
            for (auto* k : { loCut, hiCut, tool })
            {
                k->setBounds (wingRect (908.0f, y, 132.0f, 52.0f));
                y += 70.0f;
            }
        }

        // LEVEL: MUTE top left, the meter, SOLO bottom left, the fader.
        box (levelArea, "LEVEL");
        level->setBounds (sectionBody (levelArea, true));

        // EQ graph: the whole panel body (I1, Fig. 21).
        box (eqArea, "EQ", true);
        graph->setBounds (sectionBody (eqArea, true));

        // MAIN: the pan / width ring 79 px across at (911, 547), the mains 1-4
        // as 18 px squares 20 apart at (1010, 548), SOLO BUS (centred at y
        // 656) over two 56 px keys at (914, 674).
        // STUDIO (Fig. 38): no per-channel solo bus; the ring larger, the mains
        // in a row under it with their PRE badges. SIP (Fig. 39): SOLO SAFE's
        // locks where SOLO BUS is.
        box (mainArea, "MAIN", true);
        const bool studio = shownSoloMode == ld::kSoloStudio;
        pan->setBounds (studio ? wingRect (912.0f, 532.0f, 116.0f, 116.0f) : wingRect (907.5f, 543.5f, 86.0f, 86.0f));
        if (mains != nullptr)
            mains->setBounds (studio ? wingRect (908.0f, 664.0f, 124.0f, 66.0f) : wingRect (1010.0f, 548.0f, 18.0f, 80.0f));
        soloBusLabel->setBounds (wingRect (900.0f, 642.0f, 140.0f, 28.0f));
        soloA->setBounds (wingRect (914.0f, 674.0f, 57.0f, 56.0f));
        soloB->setBounds (wingRect (969.0f, 674.0f, 57.0f, 56.0f));
        if (safeLabel != nullptr)
        {
            safeLabel->setBounds (soloBusLabel->getBounds());
            safeOff->setBounds (soloA->getBounds());
            safeOn->setBounds (soloB->getBounds());
        }
    }

    void paint (juce::Graphics& g) override
    {
        HomeScreen::paint (g);
        // An analog EQ shows its faceplate at the right end of the EQ header.
        const auto& eqModel = models::model (static_cast<models::Table> (strip.getEqTable()), strip.params.eq.model.load());
        if (eqModel.token != "STD")
        {
            auto header = sectionHeader (eqArea).toFloat();
            auto plate = header.removeFromRight (header.getWidth() * 0.32f).withTrimmedRight (26.0f).reduced (0.0f, 3.0f);
            Faceplate::paintThumbnail (g, plate, static_cast<models::Table> (strip.getEqTable()), strip.params.eq.model.load(), {});
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        // Panel headers with a chevron open their screen.
        const auto pos = e.getPosition();
        auto header = [] (juce::Rectangle<int> r) { return sectionHeader (r); };
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
    /** Applies `set` to the source on the strip's MAIN input and, for a pair, its partner. */
    void forEachSourceOf (const Strip& s, const std::function<void (ld::SourceParams&)>& set) const
    {
        const int index = s.getInputLeft() >= 0 ? MixerEngine::sourceIndex (s.getInputLeft()) : -1;
        auto* src = ctx.engine.getSource (index);
        if (src == nullptr)
            return;
        const bool paired = src->mode.load() != ld::kSourceMono;
        for (int i : { paired ? (index & ~1) : index, paired ? (index | 1) : -1 })
            if (auto* each = ctx.engine.getSource (i))
                set (*each);
    }

    /** The settings of the source on the strip's MAIN input (a pair runs on its first source's). */
    ld::SourceParams* sourceOf (const Strip& s) const
    {
        const int index = s.getInputLeft() >= 0 ? MixerEngine::sourceIndex (s.getInputLeft()) : -1;
        auto* src = ctx.engine.getSource (index);
        if (src != nullptr && src->mode.load() != ld::kSourceMono)
            return ctx.engine.getSource (index & ~1);
        return src;
    }

    /** The MAIN panel follows SETUP > AUDIO > SOLO MODE (manual 5.5 p. 46,
        Fig. 37-39): LIVE shows SOLO BUS, SIP the input and aux channels'
        SOLO SAFE, STUDIO neither. */
    void showSoloMode()
    {
        const int mode = ctx.engine.soloMode.load();
        const bool changed = mode != shownSoloMode;
        shownSoloMode = mode;
        for (juce::Component* c : { (juce::Component*) soloA, (juce::Component*) soloB, (juce::Component*) soloBusLabel })
            c->setVisible (mode == ld::kSoloLive);
        for (juce::Component* c : { (juce::Component*) safeOff, (juce::Component*) safeOn, (juce::Component*) safeLabel })
            if (c != nullptr)
                c->setVisible (mode == ld::kSoloSip);
        if (changed && ! getLocalBounds().isEmpty())
            resized();
    }

    void timerCallback() override
    {
        showSoloMode();
        // Without a source there is nothing to mute or power; ROUTING can change both.
        for (auto* k : { sourceMute, phantom })
            if (k != nullptr)
            {
                k->setEnabled (sourceOf (strip) != nullptr);
                k->repaint();
            }
        if (gain != nullptr)
            gain->setEnabled (sourceOf (strip) != nullptr);
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
    IconKey *sourceMute = nullptr, *phantom = nullptr, *loCut = nullptr, *hiCut = nullptr, *tool = nullptr;
    IconKey *soloA = nullptr, *soloB = nullptr, *safeOff = nullptr, *safeOn = nullptr;
    juce::Label* phantomLabel = nullptr;
    juce::Label* safeLabel = nullptr;
    int shownSoloMode = -1;
    KnobBox *gain = nullptr, *trim = nullptr;
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
    and ALL keys, which unlink or link every input and aux channel. While
    linked the screen edits the source's name, colour and icon. */
class LinkCustomizationRow final : public juce::Component, private juce::Timer
{
public:
    LinkCustomizationRow (UiContext& c, Strip& s) : ctx (c), strip (s)
    {
        setEnabled (strip.isInput());
        startTimerHz (4);
    }

    std::function<void()> onChange;

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (colours::panel);
        g.fillRect (r);
        // WING 3.1: the 52 px key 13 px in, 6 px down (of a 64 px row).
        const float s = r.getHeight() / 64.0f;
        auto key = juce::Rectangle<float> (r.getX() + 13.0f * s, r.getY() + 6.0f * s, 52.0f * s, 52.0f * s);
        r = r.withTrimmedLeft (13.0f * s).reduced (0.0f, 6.0f * s);
        r.removeFromLeft (52.0f * s);
        linkKey = key.toNearestInt();
        const bool linked = isLinked();
        const auto ink = isEnabled() ? colours::caption : colours::dim;
        paintKeyBody (g, key, linked ? colours::accent : colours::keyOff);
        drawIcon (g, Icon::KeyboardEdit, key.reduced (key.getHeight() * 0.18f), linked ? juce::Colours::black : ink);
        r.removeFromLeft (14.0f * s);
        // NONE and ALL: list icons with their captions under them.
        for (int i = 1; i >= 0; --i)
        {
            auto k = r.removeFromRight (r.getHeight() * 1.1f);
            (i == 1 ? allKey : noneKey) = k.toNearestInt();
            auto caption = k.removeFromBottom (k.getHeight() * 0.34f);
            drawIcon (g, i == 1 ? Icon::ListAll : Icon::ListNone, k.withSizeKeepingCentre (k.getHeight() * 0.9f, k.getHeight() * 0.9f), ink);
            g.setColour (ink);
            g.setFont (wingFontFit (TypeScale::caption, caption.getHeight() * 0.95f));
            g.drawText (i == 1 ? "ALL" : "NONE", caption.toNearestInt(), juce::Justification::centred);
            r.removeFromRight (r.getHeight() * 0.2f);
        }
        g.setColour (isEnabled() ? colours::ink : colours::dim);
        g.setFont (homeFont (TypeScale::body, r.getHeight() * 0.46f));
        g.drawFittedText ("LINK CUSTOMIZATION TO SOURCE", r.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! isEnabled())
            return;
        const auto pos = e.getPosition();
        if (linkKey.contains (pos))
            strip.params.custLink = ! strip.params.custLink.load();
        else if (noneKey.contains (pos) || allKey.contains (pos))
            for (int k : { (int) StripKind::Channel, (int) StripKind::Aux })
                for (auto& each : ctx.engine.getStrips (static_cast<StripKind> (k)))
                    each->params.custLink = allKey.contains (pos);
        else
            return;
        timerCallback();
    }

    bool isLinked() const { return strip.isInput() && strip.params.custLink.load(); }

private:
    void timerCallback() override
    {
        if (isLinked() != shown)
        {
            shown = isLinked();
            repaint();
            if (onChange)
                onChange();
        }
    }

    UiContext& ctx;
    Strip& strip;
    juce::Rectangle<int> linkKey, noneKey, allKey;
    bool shown = false;
};

/** The footnote under ICON/COLOR and NAME while customization is linked. */
void paintLinkFootnote (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& what)
{
    g.setColour (colours::dim);
    g.setFont (wingFontFit (10.0f / 800.0f, (float) area.getHeight() * 0.9f)); // 10 px capitals [3.1]
    g.drawFittedText ("* CUSTOMIZATION LINK IS ENABLED, " + what + " ASSIGNED TO THE SELECTED SOURCE", area, juce::Justification::centredLeft, 1, 0.6f);
}

/** The icon a HOME screen shows for the strip: its WING icon, or the XLR
    connector for a linked hardware source without one. */
int shownIcon (const Strip& s)
{
    const int icon = s.getIcon();
    return icon == 0 && s.linkedSource() != nullptr && sourceIcon (s.getActiveInputLeft()) == Icon::Xlr ? 1 : icon;
}

/** NAME (firmware 3.1): the link row; the name in a black field with the
    channel's icon, a gold caret and the circled clear key; five name
    suggestions for the channel's icon; the on-screen keyboard (in capitals,
    as the console starts it), whose NEXT key keeps the name and moves on to
    the next channel. While customization is linked the name is the source's. */
class NameScreen final : public HomeScreen, private juce::Timer
{
public:
    NameScreen (UiContext& c, Strip& s) : HomeScreen (c, s)
    {
        link = &make<LinkCustomizationRow> (ctx, strip);
        link->onChange = [this] { keyboard->setText (scribbleName (strip)); repaint(); };
        // The keyboard's own field is hidden: this screen's field shows the
        // channel's WING icon. Its suggestion row and keys follow.
        keyboard = &make<popups::WingKeyboard>();
        keyboard->setFieldVisible (false);
        keyboard->setMaxLength (kMaxNameLength);
        keyboard->setCapsLock (true);
        keyboard->setText (scribbleName (strip));
        keyboard->setActionText ("NEXT");
        keyboard->onChange = [this] (const juce::String& t)
        {
            setName (t.substring (0, kMaxNameLength));
            repaint (field);
        };
        keyboard->onAction = [this] (const juce::String& t)
        {
            setName (t.substring (0, kMaxNameLength));
            if (auto* next = adjacentStrip (ctx.engine, &strip, 1); next != nullptr && ctx.selectStrip)
                juce::MessageManager::callAsync ([&c = ctx, next] { if (c.selectStrip) c.selectStrip (next); });
        };
        keyboard->setSuggestionRowAlwaysShown (true);
        keyboard->setSuggestions (nameSuggestions (shownIcon (strip)));
        addLevelPanel();
        startTimerHz (4);
    }

    static constexpr int kMaxNameLength = 16;

    bool hasSettings() const override { return true; }
    std::vector<HomeKnob> knobs() override { return homeTabKnobs (ctx, strip); }

    /** Names the strip, or its source while linked. */
    void setName (const juce::String& name)
    {
        if (const int index = strip.linkedSourceIndex(); index >= 0)
            ctx.engine.setSourceName (index, name);
        else
            strip.setName (name);
    }

    void timerCallback() override
    {
        // The default name ("Ch 2") reads as an empty field, so typing replaces
        // it. Only a change made elsewhere resets the field: the text being
        // typed (a space at its end, say) stays as it is.
        const auto typed = keyboard->getText().trim();
        const bool typedDefault = typed.removeCharacters (" ").equalsIgnoreCase (strip.getLabel().removeCharacters (" "));
        if ((typedDefault ? juce::String() : typed) != scribbleName (strip))
        {
            keyboard->setText (scribbleName (strip));
            repaint (field);
        }
        if (shownIcon (strip) != suggestedFor)
        {
            suggestedFor = shownIcon (strip);
            keyboard->setSuggestions (nameSuggestions (suggestedFor));
            repaint (field);
        }
        // The caret blinks.
        caretOn = ! caretOn;
        repaint (field);
    }

    void resized() override
    {
        clearBoxes();
        layoutLevelPanel (getLocalBounds());
        // WING 3.1 pixels: the link row (284, 168, 752 x 64), the field panel
        // (y 236, 88 tall) with the field (442, 244, 434 x 72), the
        // suggestions and the keyboard (y 328-732), the footnote at y 745.
        link->setBounds (wingRect (284.0f, 168.0f, 752.0f, 64.0f));
        fieldPanel = wingRect (284.0f, 236.0f, 752.0f, 88.0f);
        field = wingRect (442.0f, 244.0f, 434.0f, 72.0f);
        clearArea = field.withLeft (field.getRight() - field.getHeight());
        keyboardPanel = {};
        keyboard->setBounds (wingRect (284.0f, 328.0f, 752.0f, 404.0f));
        footnote = wingRect (288.0f, 734.0f, 740.0f, 22.0f);
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
        g.drawRect (f, wingPx (2.0f));
        auto inner = f.reduced (f.getHeight() * 0.18f);
        drawChannelIcon (g, shownIcon (strip), strip.getKind(), inner.removeFromLeft (inner.getHeight() * 1.3f), colours::ink);
        inner.removeFromLeft (inner.getHeight() * 0.5f);
        auto clear = clearArea.toFloat().reduced (clearArea.getHeight() * 0.24f);
        inner.removeFromRight (clearArea.getWidth());
        // The text as typed (a space at its end included), the caret where
        // typing goes (the arrow keys move it).
        const auto name = keyboard->getText();
        g.setColour (colours::ink);
        const juce::Font font (homeFont (TypeScale::value * 1.2f, inner.getHeight() * 0.92f));
        g.setFont (font);
        g.drawText (name, inner.toNearestInt(), juce::Justification::centredLeft, true);
        if (caretOn)
        {
            const auto beforeCaret = name.substring (0, keyboard->getCaretPosition());
            const float x = inner.getX() + juce::jmin (inner.getWidth(), juce::GlyphArrangement::getStringWidth (font, beforeCaret)) + 2.0f;
            g.setColour (colours::accent);
            g.fillRect (x, inner.getY(), 2.0f, inner.getHeight());
        }
        g.setColour (colours::caption);
        g.drawEllipse (clear, 1.5f);
        const auto cc = clear.getCentre();
        const float s = clear.getWidth() * 0.2f;
        g.drawLine (cc.x - s, cc.y - s, cc.x + s, cc.y + s, 2.0f);
        g.drawLine (cc.x - s, cc.y + s, cc.x + s, cc.y - s, 2.0f);
        if (link->isLinked())
            paintLinkFootnote (g, footnote, "NAME EDIT IS");
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (clearArea.contains (e.getPosition()))
        {
            setName ({});
            keyboard->setText ({});
            repaint (field);
        }
        focusKeyboard();
    }

    // Typing on the computer keyboard names the channel as soon as the
    // screen shows, without a click on the keyboard first.
    void visibilityChanged() override { focusKeyboard(); }
    void parentHierarchyChanged() override { focusKeyboard(); }

private:
    void focusKeyboard()
    {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::Component> (keyboard)]
        {
            if (safe != nullptr && safe->isShowing() && ! safe->hasKeyboardFocus (false))
                safe->grabKeyboardFocus();
        });
    }

    LinkCustomizationRow* link = nullptr;
    popups::WingKeyboard* keyboard = nullptr;
    juce::Rectangle<int> fieldPanel, field, clearArea, keyboardPanel, footnote;
    bool caretOn = true;
    int suggestedFor = -1;
};

/** ICON/COLOR (I13): the link row over the icon gallery (seven category keys
    and a grid of five columns, the selected icon framed gold); LIGHT (the
    light bar above the scribble strip) over the colour grid (3 x 6, in the
    console's layout); the LEVEL panel at the right. While
    customization is linked the icon and colour are the source's. */
class IconColorScreen final : public HomeScreen, private juce::Timer
{
public:
    IconColorScreen (UiContext& c, Strip& s) : HomeScreen (c, s)
    {
        link = &make<LinkCustomizationRow> (ctx, strip);
        link->onChange = [this] { category = wingIconCategory (shownIcon (strip)); repaint(); };
        category = wingIconCategory (shownIcon (strip));
        addLevelPanel();
        startTimerHz (6);
    }

    bool hasSettings() const override { return true; }
    std::vector<HomeKnob> knobs() override { return homeTabKnobs (ctx, strip); }

    void setIcon (int icon)
    {
        if (auto* src = strip.linkedSource())
            src->icon = icon;
        else
            strip.params.icon = icon;
    }

    void setColour (int colour)
    {
        if (auto* src = strip.linkedSource())
            src->colour = juce::jlimit (1, 18, colour);
        else
            strip.setColourIndex (colour);
    }

    void timerCallback() override
    {
        const auto now = std::make_tuple (shownIcon (strip), strip.getColourIndex(), strip.params.led.load());
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
        layoutLevelPanel (getLocalBounds());
        // WING 3.1 pixels: the link row (284, 168, 506 x 64) over the gallery
        // frame (284, 240, 506 x 448): seven 52 x 56 category keys at x 297
        // (61 apart from y 253), 76 px icon cells at a pitch of 86 from (357,
        // 255); LIGHT (798, 168, 232 x 64) over the colours (798, 240): 64 px
        // swatches at a pitch of 72 from (810, 252); the footnote at y 701.
        link->setBounds (wingRect (284.0f, 168.0f, 506.0f, 64.0f));
        lightRow = wingRect (798.0f, 168.0f, 232.0f, 64.0f);
        galleryFrame = wingRect (284.0f, 240.0f, 506.0f, 448.0f);
        colourFrame = wingRect (798.0f, 240.0f, 232.0f, 448.0f);
        categoryColumn = wingRect (297.0f, 253.0f, 52.0f, 422.0f);
        grid = wingRect (357.0f, 255.0f, 420.0f, 420.0f);
        colourGrid = wingRect (810.0f, 252.0f, 208.0f, 424.0f);
        footnote = wingRect (288.0f, 690.0f, 740.0f, 22.0f);
    }

    void paint (juce::Graphics& g) override
    {
        HomeScreen::paint (g);
        // [I13] #202020 frames round black wells.
        for (auto frame : { galleryFrame, colourFrame })
        {
            g.setColour (colours::panel);
            g.fillRect (frame);
            g.setColour (juce::Colours::black);
            g.fillRect (frame.reduced (juce::roundToInt (wingPx (7.0f))));
        }

        // The category keys: the selected one light grey with a dark glyph.
        const auto& cats = wingIconCategories();
        categoryKeys.clear();
        const float kh = wingPx (61.0f);
        for (int i = 0; i < (int) cats.size(); ++i)
        {
            const auto k = juce::Rectangle<float> ((float) categoryColumn.getX(), (float) categoryColumn.getY() + kh * (float) i, (float) categoryColumn.getWidth(), wingPx (56.0f));
            categoryKeys.push_back (k.toNearestInt());
            const bool sel = i == category;
            g.setColour (sel ? colours::caption : colours::panel);
            g.fillRect (k);
            drawIconCategory (g, i, k.reduced (k.getHeight() * 0.18f), sel ? colours::panel : colours::caption);
        }

        // The icon grid: grey line icons on #202020 cells, black between them.
        const auto& icons = cats[(size_t) juce::jlimit (0, (int) cats.size() - 1, category)].icons;
        // 76 px cells at a pitch of 86, a 2 px grey line midway between them.
        const float pitch = wingPx (86.0f), side = wingPx (76.0f);
        g.setColour (colours::panel);
        for (int i = 1; i < 5; ++i)
        {
            const float at = side + (pitch - side) * 0.5f - wingPx (1.0f) + pitch * (float) (i - 1);
            g.fillRect ((float) grid.getX() + at, (float) grid.getY(), wingPx (2.0f), pitch * 4.0f + side);
            g.fillRect ((float) grid.getX(), (float) grid.getY() + at, pitch * 4.0f + side, wingPx (2.0f));
        }
        cells.clear();
        const int current = shownIcon (strip);
        for (int i = 0; i < 25; ++i)
        {
            const auto face = juce::Rectangle<float> ((float) grid.getX() + pitch * (float) (i % 5), (float) grid.getY() + pitch * (float) (i / 5), side, side);
            const auto cell = face.expanded ((pitch - side) * 0.5f);
            g.setColour (colours::panel);
            g.fillRect (face);
            if (i >= (int) icons.size())
                continue;
            cells.push_back ({ cell.toNearestInt(), icons[(size_t) i].number });
            if (icons[(size_t) i].number != 0)
                drawWingIcon (g, icons[(size_t) i].number, face.reduced (face.getWidth() * 0.14f), colours::caption);
            if (icons[(size_t) i].number == current)
            {
                // A 3 px gold frame 1 px outside the cell.
                g.setColour (colours::accent);
                g.drawRect (face.expanded (wingPx (4.0f)), wingPx (3.0f));
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
        g.setColour (lit ? colours::ink : colours::dim);
        g.setFont (homeFont (TypeScale::body, lr.getHeight() * 0.46f));
        g.drawText ("LIGHT", lr.withTrimmedLeft (lr.getHeight() * 0.3f).toNearestInt(), juce::Justification::centredLeft);

        // The colours: three columns of six, laid out as the console's.
        swatches.clear();
        auto cf = colourGrid.toFloat();
        const float pitch72 = wingPx (72.0f), sw = wingPx (64.0f);
        for (int i = 0; i < 18; ++i)
        {
            const int colour = wingColourAtGrid (i);
            auto swatch = juce::Rectangle<float> (cf.getX() + pitch72 * (float) (i % 3), cf.getY() + pitch72 * (float) (i / 3), sw, sw);
            swatches.push_back ({ swatch.expanded (wingPx (4.0f)).toNearestInt(), colour });
            const bool selected = strip.getColourIndex() == colour;
            if (selected)
            {
                // The selected colour: a 3 px gold frame, 3 px of black, the swatch 2 px smaller.
                g.setColour (colours::accent);
                g.drawRect (swatch.expanded (wingPx (4.0f)), wingPx (3.0f));
                swatch = swatch.reduced (wingPx (2.0f));
            }
            g.setColour (wingColour (colour));
            g.fillRect (swatch);
        }

        if (link->isLinked())
            paintLinkFootnote (g, footnote, "ICON/COLOR SELECT ARE");
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
                setIcon (icon);
                repaint();
                return;
            }
        for (auto& [area, colour] : swatches)
            if (area.contains (pos))
            {
                setColour (colour);
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
    juce::Rectangle<int> lightRow, galleryFrame, colourFrame, categoryColumn, grid, colourGrid, lightKey, footnote;
    std::vector<juce::Rectangle<int>> categoryKeys;
    std::vector<std::pair<juce::Rectangle<int>, int>> cells, swatches;
    std::tuple<int, int, bool> shown { -1, -1, false };
};

/** TAGS (I12): the DCA tiles (4 x 4: a strip in the DCA's colour, with its
    name once it has one, over a big D1), the mute group tiles (4 x 2: the
    group's name on a red strip over MGRP1...), the TALK tag tiles (TALK ON /
    TALK SOLO, for both talk keys, A or B) on olive strips, the strip's tag
    chips with ADD TAG, and the LEVEL panel. A tile lights while assigned. */
class TagsScreen final : public HomeScreen, private juce::Timer
{
public:
    TagsScreen (UiContext& c, Strip& s) : HomeScreen (c, s)
    {
        addTag = &key ("ADD TAG", Icon::Plus);
        addTag->offInk = colours::dim; // WING 3.1: grey on the dark key
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

    bool hasSettings() const override { return true; }
    std::vector<HomeKnob> knobs() override { return homeTabKnobs (ctx, strip); }

    void resized() override
    {
        clearBoxes();
        layoutLevelPanel (getLocalBounds());
        // WING 3.1 pixels: the frames 478 wide at x 284 (DCAs y 168-488, mute
        // groups 500-660, TALK 672-732), the chips column 824-976 from y 168.
        dcaFrame = wingRect (284.0f, 168.0f, 478.0f, 320.0f);
        muteFrame = wingRect (284.0f, 500.0f, 478.0f, 160.0f);
        talkFrame = wingRect (284.0f, 672.0f, 478.0f, 60.0f);
        chipColumn = wingRect (824.0f, 168.0f, 152.0f, 560.0f);
        chipH = juce::roundToInt (wingPx (60.0f));
        addTag->setBounds (chipColumn.withHeight (chipH).translated (0, (chipH + juce::roundToInt (wingPx (8.0f))) * stripTags (strip).size()));
    }

    void paint (juce::Graphics& g) override
    {
        HomeScreen::paint (g);
        tiles.clear();
        auto& engine = ctx.engine;
        const auto& p = strip.params;
        // WING 3.1: each group in a black well with a 4 px #202020 rim; tiles
        // of 96 x 64 on a 122 x 80 pitch, 8 px in (TALK: 64 x 44, 80 apart).
        auto frame = [&g] (juce::Rectangle<int> r)
        {
            g.setColour (colours::panel);
            g.fillRect (r);
            g.setColour (juce::Colours::black);
            g.fillRect (r.reduced (juce::roundToInt (wingPx (4.0f))));
            return r.toFloat().reduced (wingPx (8.0f));
        };
        // A tile: the strip (a third of the tile) with its name, the body with
        // the big label; lit while on.
        auto tile = [&g] (juce::Rectangle<float> r, const juce::String& name, const juce::String& big, juce::Colour stripColour,
                          juce::Colour body, bool on, bool small = false)
        {
            auto top = r.removeFromTop (r.getHeight() * (small ? 0.5f : 22.0f / 64.0f));
            g.setColour (on ? stripColour.brighter (0.4f) : stripColour);
            g.fillRect (top);
            g.setColour (juce::Colours::black);
            g.setFont (wingFontPx (top.getHeight() * 0.55f));
            g.drawFittedText (name, top.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.6f);
            g.setColour (on ? body.brighter (0.6f) : body);
            g.fillRect (r);
            g.setColour (on ? colours::ink : colours::dim);
            g.setFont (wingFontPx (r.getHeight() * (small ? 0.6f : 20.0f / 42.0f)));
            g.drawFittedText (big, r.toNearestInt().reduced (juce::roundToInt (r.getHeight() * 0.15f), 0),
                              small || big.startsWith ("MGRP") ? juce::Justification::centred : juce::Justification::centredLeft, 1, 0.6f);
        };
        // The pitch and size of a tile in a frame of `columns` x `rows` cells.
        auto cell = [] (juce::Rectangle<float> f, int i, int columns, int, float pitchX, float pitchY, float w, float h)
        {
            return juce::Rectangle<float> (f.getX() + wingPx (pitchX) * (float) (i % columns), f.getY() + wingPx (pitchY) * (float) (i / columns), wingPx (w), wingPx (h));
        };

        auto dcas = frame (dcaFrame);
        for (int i = 0; i < kNumDcas; ++i)
        {
            auto r = cell (dcas, i, 4, 4, 122.0f, 80.0f, 96.0f, 64.0f);
            const bool on = (p.dcaMask.load() >> i) & 1u;
            // Unnamed (or an older show's default "DCA.3"): a blank strip, as on the console.
            const auto name = engine.getDcaName (i);
            const auto title = name == "DCA." + juce::String (i + 1) ? juce::String() : name.toUpperCase();
            const auto colour = wingColour (engine.getDca (i).colour.load());
            tile (r, title, "D" + juce::String (i + 1), colour.withMultipliedBrightness (0.7f), colours::tagBody, on);
            tiles.push_back ({ r.toNearestInt(), i });
        }
        auto mutes = frame (muteFrame);
        for (int i = 0; i < kNumMuteGroups; ++i)
        {
            auto r = cell (mutes, i, 4, 2, 122.0f, 80.0f, 96.0f, 64.0f);
            const bool on = (p.muteGroupMask.load() >> i) & 1u;
            tile (r, engine.getMuteGroupName (i).toUpperCase(), "MGRP" + juce::String (i + 1), colours::muteTag, colours::tagBody, on);
            tiles.push_back ({ r.toNearestInt(), 100 + i });
        }
        // TALK tags (manual p. 31): TALK ON unmutes and TALK SOLO solos the
        // strip while talkback is on (A / B: only for that talk key). The
        // tiles' order is the tag bits' (ld::TalkTag).
        auto talk = frame (talkFrame);
        static const char* talkNames[][2] = { { "TALK", "SOLO" }, { "TALK", "ON" }, { "TALK A", "SOLO" }, { "TALK A", "ON" }, { "TALK B", "SOLO" }, { "TALK B", "ON" } };
        const auto talkTags = strip.getTalkTags();
        for (int i = 0; i < 6; ++i)
        {
            auto r = cell (talk, i, 6, 1, 80.0f, 0.0f, 64.0f, 44.0f);
            tile (r, talkNames[i][0], talkNames[i][1], colours::talkOff, colours::talkBody, (talkTags >> i) & 1u, true);
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
        // On Channel Home (WING pixels, the column from 8): thumbnails 8-104,
        // the key's pointer at 116-124, keys 124-192, then the black gap up to
        // the content at 284 (the tag chips and, in wrench mode, the TAP flag).
        // The slot screens show only the thumbnails.
        const int unitW = keys ? 268 : 101; // the slot screens: 96 + the tap line's 5
        const float kx = (float) b.getWidth() / (float) unitW;
        const int thumbW = juce::roundToInt (96.0f * kx);
        const int keyW = keys ? juce::roundToInt (68.0f * kx) : 0;
        const int keyX = b.getX() + juce::roundToInt (116.0f * kx);
        gapArea = keys ? b.withLeft (keyX + keyW) : juce::Rectangle<int>();
        keyLeft = keyX;
        // WING 3.1 (of the column's 672 px): HOME 64 px tall, the blocks 65
        // tall at a pitch of 73, the fader bar 16 tall with 8 px either side.
        const float ky = (float) b.getHeight() / 672.0f;
        const float rowH = 73.0f * ky;
        const float gapF = 8.0f * ky;
        const int gap = juce::jmax (2, juce::roundToInt (gapF));
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
        auto addRow = [&] (std::optional<HomeTab> tab, bool block, float height = 65.0f)
        {
            const auto cell = juce::Rectangle<int> (b.getX(), juce::roundToInt (y), thumbW, juce::roundToInt (y + height * ky) - juce::roundToInt (y));
            if (tab.has_value())
            {
                Row r;
                r.tab = *tab;
                r.thumb = cell;
                if (keys && block)
                    r.key = juce::Rectangle<int> (keyX, cell.getY(), keyW, cell.getHeight());
                rows.push_back (r);
            }
            y += (height + 8.0f) * ky;
        };

        addRow (HomeTab::Home, false, 64.0f);
        addRow (HomeTab::Input, false);
        const float inputBottom = y - gapF;
        std::vector<float> above; // the boundary above each slot
        float pfl = 0.0f, afl = 0.0f;
        for (int i = 0; i < (int) slots.size(); ++i)
        {
            if (i == pre)
            {
                pfl = y - gapF * 0.5f;
                faderBar = juce::Rectangle<float> ((float) b.getX(), y, (float) thumbW, 16.0f * ky);
                y += 24.0f * ky;
                afl = y - gapF * 0.5f;
            }
            above.push_back (y - gapF * 0.5f);
            addRow (slots[(size_t) i], true);
        }
        // IN, FILTER, TAP 3, TAP 4, TAP 5, PRE FDR, POST FDR, POST PROC.
        tapY = { inputBottom - rowH * 0.3f, above[0], above[1], above[2], above[3], pfl, afl, y - gapF * 0.5f };
        addRow (HomeTab::Main, false);
        if (kind != StripKind::Matrix)
            addRow (HomeTab::Sends, false);
        else
            y += rowH;

        // The wrench (input channels: the slot order and TAP point can be dragged).
        wrench = keys && strip->hasSelectableTap() ? juce::Rectangle<int> (keyX, juce::roundToInt (y - rowH), keyW, juce::roundToInt (rowH) - gap) : juce::Rectangle<int>();
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
            const float h = juce::jlimit (9.0f, screenCap (16.0f), (float) home.getHeight() * 0.26f);
            auto area = juce::Rectangle<float> ((float) keyLeft, (float) home.getY() + 2.0f, (float) (getWidth() - keyLeft) - 2.0f, h);
            for (auto& tag : stripTags (*strip))
            {
                if (area.getY() + h > (float) home.getBottom())
                    break;
                paintTagChip (g, area, tag.toUpperCase());
                area.translate (0.0f, h + 2.0f);
            }
        }

        // The fader bar [I1]: a grey line with a tick at 0 dB, the fader level
        // over it (red when muted), and the TAP point.
        g.setColour (colours::keyOff);
        g.fillRect (faderBar);
        const auto rail = faderBar.reduced (faderBar.getWidth() * 0.06f, 0.0f);
        g.setColour (colours::chipOff);
        g.fillRect (rail.withSizeKeepingCentre (rail.getWidth(), juce::jmax (1.0f, faderBar.getHeight() * 0.12f)));
        g.fillRect (juce::Rectangle<float> (juce::jmax (1.0f, faderBar.getHeight() * 0.12f), faderBar.getHeight() * 0.45f)
                        .withCentre ({ rail.getX() + rail.getWidth() * fader::dbToPos (0.0f), rail.getCentreY() }));
        const float pos = fader::dbToPos (p.faderDb.load());
        g.setColour (p.mute.load() ? Colours::mute : colours::fader);
        g.fillRect (faderBar.withWidth (faderBar.getWidth() * pos));
        if (const int tap = tapIndex(); tap >= 0 && tap < (int) tapY.size())
        {
            const float d = juce::jmax (5.0f, wingPx (8.0f)); // WING 3.1: an 8 px dot
            const float ty = draggingTap ? (float) dragY : tapY[(size_t) tap];
            const float dotX = (float) thumbWidth * 0.5f;
            // "A red dot with a gray line connected to the bus send slot": a
            // 2 px line in the thumbnails' grey, 3 px right of them, down to
            // 3 px over SENDS, then left to SENDS' middle and into its top.
            if (auto sends = rowOf (HomeTab::Sends); ! sends.isEmpty())
            {
                const float w = juce::jmax (1.0f, wingPx (2.0f));
                const float edge = (float) sends.getRight() + wingPx (3.0f) + w * 0.5f;
                const float turn = (float) sends.getY() - wingPx (3.0f) - w * 0.5f;
                juce::Path line;
                line.startNewSubPath (dotX, ty);
                line.lineTo (edge, ty);
                line.lineTo (edge, turn);
                line.lineTo ((float) sends.getCentreX(), turn);
                line.lineTo ((float) sends.getCentreX(), (float) sends.getY());
                g.setColour (colours::thumb);
                g.strokePath (line, juce::PathStrokeType (w));
            }
            if (editing && ! gapArea.isEmpty())
                paintTapFlag (g, dotX, ty);
            g.setColour (colours::tap);
            g.fillRoundedRectangle (dotX - d * 0.5f, ty - d * 0.5f, d, d, d * 0.3f); // a rounded square on the console
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
        // WING 3.1: the frame as tall as the thumbnail (68 x 65), a 44 px
        // square key in a 2 px black rim at its middle.
        g.setColour (colours::keyFrame);
        g.fillRect (k);
        const float side = k.getWidth() * 48.0f / 68.0f;
        auto inner = juce::Rectangle<float> (side, side).withCentre (k.getCentre());
        g.setColour (juce::Colours::black);
        g.fillRect (inner);
        inner = inner.reduced (side * 2.0f / 48.0f);
        g.setColour (fill);
        g.fillRect (inner);
        paintBlockIcon (g, icon, inner, on ? juce::Colours::black : kIconInk);
    }

    static void paintBlockIcon (juce::Graphics& g, Icon icon, juce::Rectangle<float> key, juce::Colour ink)
    {
        if (! paintHomeBlockIcon (g, icon, key, ink))
            drawIcon (g, icon, key.reduced (key.getHeight() * 0.2f), ink);
    }

    static inline const juce::Colour kIconInk { 0xff707070 };
    static inline const juce::Colour kMainOn { 0xff00a4f8 }; // a main assignment on the MAIN thumbnail

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
        const float h = juce::jlimit (12.0f, screenCap (22.0f), gap.getWidth() * 0.3f);
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
        g.setFont (wingFontFit (TypeScale::caption, flag.getHeight() * 0.72f, WingWeight::SemiBold));
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
        // The pointer joining the key to its thumbnail: 8 px long, 18 tall.
        juce::Path tri;
        const float s = k.getWidth() * 8.0f / 68.0f, h = k.getWidth() * 9.0f / 68.0f;
        tri.addTriangle (k.getX() - s, k.getCentreY(), k.getX(), k.getCentreY() - h, k.getX(), k.getCentreY() + h);
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
            g.setFont (wingFontFit (TypeScale::tab, t.getHeight() * 0.6f));
            g.drawFittedText ("HOME", t.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.7f);
        }
        else
        {
            g.setColour (colours::thumb);
            g.fillRoundedRectangle (t, 2.0f);
            paintThumb (g, r.tab, t);
        }
        if (selected)
        {
            g.setColour (colours::accent);
            g.drawRoundedRectangle (t.reduced (1.0f), 2.0f, 2.0f);
        }
    }

    /** A thumbnail chip: grey (black text), lit in `onColour`. */
    static void chip (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, bool on, juce::Colour onColour = colours::caption)
    {
        g.setColour (on ? onColour : colours::chipOff);
        g.fillRoundedRectangle (r, juce::jmin (2.0f, r.getHeight() * 0.15f));
        // [D5] the text fills the chip: about 60 % of its height in capitals.
        g.setColour (juce::Colours::black);
        g.setFont (wingFontPx (juce::jmax (4.0f, r.getHeight() * 0.6f), WingWeight::SemiBold));
        g.drawFittedText (text, r.toNearestInt(), juce::Justification::centred, 1, 0.6f);
    }

    /** WING 3.1: 11 segments 3 px tall, 2 px apart, unlit near black. */
    static void miniMeter (juce::Graphics& g, juce::Rectangle<float> r, float level, juce::Colour c)
    {
        constexpr int segments = 11;
        const float pitch = r.getHeight() / ((float) segments - 0.4f), h = pitch * 0.6f;
        for (int i = 0; i < segments; ++i)
        {
            const bool lit = level > (float) i / (float) segments;
            g.setColour (lit ? c : kMeterOff);
            g.fillRect (r.getX(), r.getBottom() - (float) i * pitch - h, r.getWidth(), h);
        }
    }

    static inline const juce::Colour kMeterOff { 0xff101010 };

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
        // WING 3.1 thumbnail geometry (of 96 x 64): the meter 10 px wide at
        // x 80, y 6-59; chips 25 x 15 at x 5 / 35, y 5 / 25 / 45; the model
        // or slot chip 56 x 22.
        auto meterArea = at (80.0f, 6.0f, 10.0f, 53.0f);
        auto r = t;

        switch (tab)
        {
            case HomeTab::Input:
            {
                const int n = strip->getNumChannels();
                const float mw = meterArea.getWidth() / (float) n;
                for (int ch = 0; ch < n; ++ch)
                {
                    const float level = meters::scalePos (dsp::gainToDb (page.ctx.meters.get (strip, true, ch)));
                    miniMeter (g, meterArea.removeFromLeft (mw).withTrimmedRight (n > 1 ? 1.0f : 0.0f), level, Colours::meterGreen);
                }
                auto cell = [&at] (int col, int row) { return at (5.0f + 30.0f * (float) col, 5.0f + 20.0f * (float) row, 25.0f, 15.0f); };
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
                chip (g, at (5.0f, 5.0f, 56.0f, 22.0f), model.token, on, colours::accent);
                // The state LED: the gate open, the compressor over its threshold.
                const bool lit = on && (gate ? grDb < 3.0f && page.ctx.meters.get (strip, true, 0) > 0.001f : grDb > 0.5f);
                g.setColour (lit ? juce::Colour (0xffe0e800) : juce::Colours::black);
                g.fillRect (at (68.0f, 5.0f, 8.0f, 8.0f));
                chip (g, at (5.0f, 45.0f, 25.0f, 15.0f), "S/C", slot.scSource.load() >= 0);
                if (! gate)
                    chip (g, at (35.0f, 45.0f, 25.0f, 15.0f), "X/O", slot.xoKind.load() > 0, KnobColours::blue); // blue, as the crossover's curve
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
                chip (g, at (5.0f, 5.0f, 56.0f, 22.0f), text, isOn (tab) || (mode > 0 && ins.on.load()), colours::accent);
                // The slot's device, or NONE in grey (3.1).
                juce::String device;
                if (mode > 0)
                    device = mode == 1 ? "X" : "Y";
                else if (auto* plugin = slot >= 0 ? fx.getPlugin (slot) : nullptr)
                    device = plugin->getName().toUpperCase();
                else if (slot >= 0 && fx.getModel (slot) > ld::kFxExt)
                    device = models::model (models::Fx, fx.getModel (slot)).token;
                if (mode == 0 && slot >= 0 && device.isEmpty())
                {
                    g.setColour (colours::dim);
                    g.setFont (wingFontPx (13.0f * v)); // [3.1] capitals 13 of the 64 px
                    g.drawFittedText ("NONE", at (6.0f, 38.0f, 84.0f, 22.0f).toNearestInt(), juce::Justification::centredRight, 1, 0.6f);
                    return;
                }
                // A premium device (slots 1-8) adds a small diamond (3.1); an
                // insert without a slot shows only its chip.
                if (mode == 0 && slot < 0)
                    return;
                if (mode == 0 && slot < 8)
                    drawIcon (g, Icon::Premium, at (66.0f, 5.0f, 18.0f, 18.0f), isOn (tab) ? colours::accent : colours::dim);
                g.setColour (colours::ink);
                g.setFont (wingFontPx (13.0f * v));
                g.drawFittedText (device, at (6.0f, 38.0f, 84.0f, 22.0f).toNearestInt(), juce::Justification::centredRight, 1, 0.6f);
                return;
            }

            case HomeTab::Eq:
            {
                // WING 3.1: the curve in a black box 4 px inside the thumbnail.
                r = r.reduced (4.0f * u, 4.0f * v);
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
                    g.setColour (on ? colours::eqCurve : kIconInk);
                    g.strokePath (curve, juce::PathStrokeType (juce::jmax (1.5f, 2.0f * v)));
                }
                return;
            }

            case HomeTab::Main:
            {
                // WING 3.1: the ring 34 px across at (9, 6), the mains as 11 px
                // squares 14 apart at x 50 (the lit one with its number), the
                // stereo meter at x 67 and 80.
                PanWidthDisplay::paintRing (g, at (6.5f, 4.5f, 37.0f, 37.0f), p.pan.load(), p.width.load(), false);
                if (strip->getKind() != StripKind::Main && strip->getKind() != StripKind::Matrix)
                {
                    for (int i = 0; i < kNumMains; ++i)
                    {
                        auto sq = at (50.0f, 6.0f + 14.0f * (float) i, 11.0f, 11.0f);
                        const bool assigned = p.mains[i].on.load();
                        g.setColour (assigned ? kMainOn : colours::panel);
                        g.fillRect (sq);
                        if (assigned)
                        {
                            g.setColour (juce::Colours::black);
                            g.setFont (wingFontPx (sq.getHeight() * 0.8f, WingWeight::SemiBold));
                            g.drawText (juce::String (i + 1), sq, juce::Justification::centred, false);
                        }
                    }
                }
                else
                    chip (g, at (5.0f, 45.0f, 56.0f, 15.0f), "DLY", p.input.delayOn.load());
                for (int ch = 0; ch < 2; ++ch)
                {
                    const float level = meters::scalePos (dsp::gainToDb (page.ctx.meters.get (strip, false, ch)));
                    miniMeter (g, at (67.0f + 13.0f * (float) ch, 6.0f, 10.0f, 53.0f), level, Colours::meterGreen);
                }
                return;
            }

            case HomeTab::Sends:
            {
                // WING 3.1: the 8 matrix sends at the top right, a black line,
                // the bus sends 1-8 and 9-16 below: bars 4 px wide at a pitch
                // of 5, filled with the level in the send mode's colour (TAP
                // red, POST green).
                auto bars = [&g, &at] (float x0, float y0, int n, std::function<std::pair<float, juce::Colour> (int)> level)
                {
                    for (int i = 0; i < n; ++i)
                    {
                        auto bar = at (x0 + 5.0f * (float) (i % 8) + (i >= 8 ? 47.0f : 0.0f), y0, 4.0f, 26.0f);
                        g.setColour (colours::outline);
                        g.fillRect (bar);
                        const auto [l, c] = level (i);
                        if (l > 0.0f)
                        {
                            g.setColour (c);
                            g.fillRect (bar.withTop (bar.getBottom() - bar.getHeight() * l));
                        }
                    }
                };
                g.setColour (juce::Colours::black);
                g.fillRect (at (4.0f, 32.0f, 88.0f, 1.5f));
                if (strip->getKind() != StripKind::Matrix)
                    bars (52.0f, 4.0f, kNumMatrices, [&p] (int i)
                    {
                        const auto c = p.matrix[i].pre.load() ? colours::tap : colours::post;
                        return std::make_pair (p.matrix[i].on.load() ? fader::dbToPos (p.matrix[i].levelDb.load()) : 0.0f, c);
                    });
                if (strip->getKind() != StripKind::Main)
                {
                    const int n = juce::jmin (16, (int) page.ctx.engine.getBuses().size());
                    bars (5.0f, 35.0f, n, [this] (int i)
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

int HomePage::tapSlot() const
{
    if (strip == nullptr || overlay != nullptr || (current != HomeTab::PreInsert && current != HomeTab::PostInsert))
        return -1;
    const auto& ins = current == HomeTab::PreInsert ? strip->params.preInsert : strip->params.postInsert;
    // INS 2 in automix mode has no FX.
    if (current == HomeTab::PostInsert && ins.mode.load() != 0)
        return -1;
    const int slot = ins.fx.load();
    if (slot < 0 || TapTempo::timeParam (ctx.engine.getFx().getModel (slot)) < 0)
        return -1;
    return slot;
}

void HomePage::blankKeyPressed()
{
    // Tap tempo, as on EFFECTS: the time between two taps (0.1 to 3 s) becomes the delay time.
    const int slot = tapSlot();
    if (slot < 0)
        return;
    const auto interval = tapTempo.tap (juce::Time::getMillisecondCounterHiRes());
    if (! interval)
        return;
    const int model = ctx.engine.getFx().getModel (slot);
    const int param = TapTempo::timeParam (model);
    ctx.engine.getFx().params (slot).values[param].store (TapTempo::valueForMs (model, param, *interval));
}

std::optional<bool> HomePage::blankKeyBlink (double nowMs) const
{
    const int slot = tapSlot();
    if (slot < 0)
        return std::nullopt;
    const int model = ctx.engine.getFx().getModel (slot);
    const int param = TapTempo::timeParam (model);
    return tapTempo.ledLit (nowMs, TapTempo::msOf (model, param, ctx.engine.getFx().params (slot).values[param].load()));
}

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
        content->statusKeysChanged = [this] { notifyStatusKeysChanged(); };
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
    notifyKnobsChanged();
}

bool HomePage::usesKnobs() const
{
    // 3.1: CHANNEL INIT / COPY leave the parameter bar blank.
    return overlayKind != OverlayKind::Init && overlayKind != OverlayKind::Copy;
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
    notifyKnobsChanged();
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
    if (strip == nullptr || (content != nullptr && content->hidesStatusKeys()))
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
    if (strip != nullptr && ! tabBar.isEmpty())
    {
        g.setColour (Colours::screenHeader);
        g.fillRect (tabBar);
        g.fillRect (tabLine);
    }
    if (strip == nullptr)
    {
        g.setColour (Colours::textDim);
        g.setFont (wingFont (TypeScale::value));
        g.drawText ("PRESS SEL ON A STRIP", getLocalBounds(), juce::Justification::centred);
    }
}

void HomePage::resized()
{
    auto b = getLocalBounds();
    if (overlay != nullptr)
        overlay->setBounds (b);

    // The toolbar, 8 of 1280 WING pixels in: thumbnails, keys and the black
    // gap on Channel Home (the content starts at 284), only the thumbnails on
    // the slot screens (content from 112); the content ends 8 px from the
    // right edge.
    const bool overviewGroup = isOverviewGroup (current);
    // WING 3.1 pixels (the page is 1280 x 672 from y 80 of the screen; 716
    // while INIT / COPY hide the parameter bar).
    const float kx = (float) getWidth() / 1280.0f, ky = getWingScreenHeight() / 800.0f;
    auto wr = [kx, ky] (float x, float y, float w, float h)
    {
        const int x0 = juce::roundToInt (x * kx), y0 = juce::roundToInt (y * ky);
        return juce::Rectangle<int> (x0, y0, juce::roundToInt ((x + w) * kx) - x0, juce::roundToInt ((y + h) * ky) - y0);
    };
    // On the slot screens the column reaches 108 + 1, so the insert tap's
    // grey line (3 px right of the thumbnails) shows, as on the console.
    column->setBounds (wr (8.0f, 0.0f, overviewGroup ? 268.0f : 101.0f, 672.0f));

    if (overviewGroup)
    {
        // The tab bar 284-1272, 64 tall: tabs 157 wide at a pitch of 160,
        // 55 tall over a black line; SAFE (in its black frame) at the right.
        for (int i = 0; i < topTabs.size(); ++i)
            topTabs[i]->setBounds (wr (284.0f + 160.0f * (float) i, 0.0f, 157.0f, 55.0f));
        safeKey->setBounds (wr (1148.0f, 4.0f, 120.0f, 56.0f));
        tabBar = wr (924.0f, 0.0f, 348.0f, 64.0f);   // right of the tabs, round SAFE
        tabLine = wr (284.0f, 58.0f, 988.0f, 6.0f);  // under the black line below the tabs
        // The panels from y 152 (the content's half gaps around them).
        b = wr (280.0f, 68.0f, 996.0f, 608.0f);
    }
    else
    {
        tabBar = tabLine = {};
        // The slot screens: from x 112 (their panels flush with the area).
        b = wr (112.0f, 0.0f, 1160.0f, 672.0f);
    }

    if (content != nullptr)
        content->setBounds (b);
}

} // namespace livedaw::ui
