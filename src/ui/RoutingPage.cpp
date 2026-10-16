#include "dsp/DspUtils.h"
#include "ui/Pages.h"

namespace livedaw::ui
{

namespace
{
    constexpr int kTilesPerRow = 8, kTargetsPerPage = 48, kPortsPerGroup = 48;

    const juce::Colour navy (0xff27344d), tileOff (0xff2b2b2b), tileOn (0xff4a4a4a), unpatched (0xff3a6ea5), monoLabel (0xff5aa0e8),
        stereoLabel (0xffc8b43a);

    const char* const signalGroups[] = { "CHANNEL", "AUX", "BUS", "MAIN", "MATRIX", "MONITOR" };

    bool hasCustomName (const Strip& s)
    {
        return stripName (s) != s.getLabel();
    }

    juce::StringArray numbered (const juce::StringArray& names, int count)
    {
        juce::StringArray items;
        for (int i = 0; i < count; ++i)
            items.add (juce::String (i + 1) + ": " + (i < names.size() ? names[i] : juce::String ("(not available)")));
        return items;
    }

    /** The monitor setup: outputs, PFL / AFL and level (SETUP > MONITORS until
        the console's MONITORS screen exists). */
    class MonitorPanel final : public juce::Component
    {
    public:
        explicit MonitorPanel (UiContext& c)
            : ctx (c),
              solo ({ "PFL", "AFL" }, [&c] { return c.engine.soloAfl.load() ? 1 : 0; }, [&c] (int i) { c.engine.soloAfl = i == 1; }, ChoiceKeys::prePost())
        {
            const auto outs = numbered (ctx.engine.getHardwareOutputNames(), juce::jmax (64, ctx.engine.getHardwareOutputNames().size()));
            left.setItems (outs, "OFF");
            right.setItems (outs, "OFF");
            left.bind (&ctx.engine.monitorOutputLeft);
            right.bind (&ctx.engine.monitorOutputRight);
            level.setBinding (bindLinear ("LEVEL", ctx.engine.monitorLevelDb, -60.0f, 10.0f, " dB", 1, 0.0f));
            for (auto* c2 : { (juce::Component*) &left, (juce::Component*) &right, (juce::Component*) &solo, (juce::Component*) &level })
                addAndMakeVisible (c2);
        }

        void paint (juce::Graphics& g) override
        {
            drawDarkPanel (g, outputBox, "MONITOR OUTPUT");
            drawDarkPanel (g, soloBox, "SOLO");
            drawDarkPanel (g, levelBox, "MONITOR LEVEL");
            g.setColour (Dark::textDim);
            g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
            g.drawText ("LEFT", leftLabel, juce::Justification::centredLeft);
            g.drawText ("RIGHT", rightLabel, juce::Justification::centredLeft);
            g.drawFittedText ("Without a solo, the monitor follows MAIN 1. The monitor can also feed any output on ROUTING > OUTPUTS (source group MONITOR).",
                              noteArea, juce::Justification::topLeft, 3);
        }

        void resized() override
        {
            auto b = getLocalBounds().reduced (12, 0);
            auto top = b.removeFromTop (b.getHeight() / 2);
            outputBox = top.removeFromLeft (top.getWidth() * 55 / 100).withTrimmedRight (8);
            soloBox = top;
            levelBox = b.withTrimmedTop (8).withWidth (juce::jmin (b.getWidth(), 280));

            auto o = darkPanelBody (outputBox, true).reduced (8, 4);
            leftLabel = o.removeFromTop (20);
            left.setBounds (o.removeFromTop (34));
            o.removeFromTop (8);
            rightLabel = o.removeFromTop (20);
            right.setBounds (o.removeFromTop (34));
            o.removeFromTop (8);
            noteArea = o;

            solo.setBounds (darkPanelBody (soloBox, true).withSizeKeepingCentre (juce::jmin (220, soloBox.getWidth() - 20), 44));
            level.setBounds (darkPanelBody (levelBox, true));
        }

    private:
        UiContext& ctx;
        IntCombo left, right;
        ChoiceKeys solo;
        DisplayKnob level;
        juce::Rectangle<int> outputBox, soloBox, levelBox, leftLabel, rightLabel, noteArea;
    };

    void drawKey (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, bool on, juce::Colour onColour, bool enabled = true)
    {
        if (r.isEmpty())
            return;
        auto fill = on ? onColour : Dark::key;
        if (! enabled)
            fill = fill.darker (0.55f);
        g.setColour (fill);
        g.fillRect (r);
        g.setColour (on ? Dark::choiceInk : (enabled ? Dark::text : Dark::textDim.darker (0.3f)));
        g.setFont (juce::FontOptions (juce::jmin (18.0f, (float) r.getHeight() * 0.5f), juce::Font::bold));
        g.drawFittedText (text, r.reduced (2, 0), juce::Justification::centred, 2, 0.7f);
    }

    /** An icon over a small caption (NONE, RENAME). */
    void drawIconKey (juce::Graphics& g, juce::Rectangle<int> r, Icon icon, const juce::String& text, bool enabled = true)
    {
        if (r.isEmpty())
            return;
        const auto ink = enabled ? Dark::text : Dark::textDim.withAlpha (0.5f);
        auto f = r.toFloat();
        auto label = f.removeFromBottom (f.getHeight() * 0.36f);
        drawIcon (g, icon, f.reduced (f.getHeight() * 0.12f), ink);
        g.setColour (ink);
        g.setFont (juce::FontOptions (juce::jlimit (9.0f, 13.0f, label.getHeight() * 0.85f), juce::Font::bold));
        g.drawFittedText (text, label.toNearestInt(), juce::Justification::centredTop, 1);
    }

    /** A group selector: icon, caption over the group's name, and a down-arrow key. */
    void drawSelector (juce::Graphics& g, juce::Rectangle<int> r, Icon icon, const juce::String& caption, const juce::String& value)
    {
        g.setColour (Dark::well);
        g.fillRect (r);
        auto arrow = r.removeFromRight (r.getHeight() * 2 / 3);
        g.setColour (Dark::key);
        g.fillRect (arrow.withTrimmedLeft (3));
        juce::Path tri;
        const auto c = arrow.withTrimmedLeft (3).getCentre().toFloat();
        tri.addTriangle (c.x - 7.0f, c.y - 4.0f, c.x + 7.0f, c.y - 4.0f, c.x, c.y + 5.0f);
        g.setColour (Dark::text);
        g.fillPath (tri);
        auto ib = r.removeFromLeft (r.getHeight()).reduced (6);
        drawIcon (g, icon, ib.toFloat(), Dark::text);
        r.removeFromLeft (4);
        g.setColour (Dark::textDim);
        g.setFont (juce::FontOptions (juce::jmin (13.0f, (float) r.getHeight() * 0.3f), juce::Font::bold));
        g.drawText (caption, r.removeFromTop (r.getHeight() * 42 / 100), juce::Justification::bottomLeft);
        g.setColour (Dark::text);
        g.setFont (juce::FontOptions (juce::jmin (22.0f, (float) r.getHeight() * 0.8f), juce::Font::bold));
        g.drawFittedText (value, r, juce::Justification::centredLeft, 1);
    }
} // namespace

std::unique_ptr<juce::Component> makeMonitorPanel (UiContext& ctx)
{
    return std::make_unique<MonitorPanel> (ctx);
}

RoutingPage::RoutingPage (UiContext& c) : ctx (c)
{
    startTimerHz (5);
}

RoutingPage::~RoutingPage() = default;

std::vector<StatusKey> RoutingPage::getStatusKeys()
{
    return { { "CHANNELS", Icon::Faders, mode == Mode::Channels, [this] { setMode (Mode::Channels); } },
             { "SOURCES", Icon::SourcesIn, mode == Mode::Sources, [this] { setMode (Mode::Sources); } },
             { "OUTPUTS", Icon::OutputsOut, mode == Mode::Outputs, [this] { setMode (Mode::Outputs); } } };
}

std::vector<Strip*> RoutingPage::channelTargets() const
{
    std::vector<Strip*> list;
    for (auto k : { StripKind::Channel, StripKind::Aux })
        for (auto& s : ctx.engine.getStrips (k))
            list.push_back (s.get());
    return list;
}

std::vector<RoutingPage::Signal> RoutingPage::signals() const
{
    std::vector<Signal> list;
    if (signalGroup == 5)
        return { { nullptr, 0 }, { nullptr, 1 } };
    const StripKind kinds[] = { StripKind::Channel, StripKind::Aux, StripKind::Bus, StripKind::Main, StripKind::Matrix };
    for (auto& s : ctx.engine.getStrips (kinds[juce::jlimit (0, 4, signalGroup)]))
    {
        list.push_back ({ s.get(), 0 });
        if (s->isOutput() || s->params.stereo.load())
            list.push_back ({ s.get(), 1 });
    }
    return list;
}

int RoutingPage::numTargetPages() const
{
    return juce::jmax (1, ((int) channelTargets().size() + kTargetsPerPage - 1) / kTargetsPerPage);
}

int RoutingPage::numPorts (bool outputs) const
{
    const auto& names = outputs ? ctx.engine.getHardwareOutputNames() : ctx.engine.getHardwareInputNames();
    return juce::jmax (kPortsPerGroup, (names.size() + kPortsPerGroup - 1) / kPortsPerGroup * kPortsPerGroup);
}

int RoutingPage::numPortGroups (bool outputs) const
{
    return numPorts (outputs) / kPortsPerGroup;
}

juce::String RoutingPage::portName (int index, bool output) const
{
    const auto& names = output ? ctx.engine.getHardwareOutputNames() : ctx.engine.getHardwareInputNames();
    if (index < names.size() && names[index].isNotEmpty())
        return names[index].toUpperCase();
    return (output ? "OUT " : "IN ") + juce::String (index + 1);
}

Strip* RoutingPage::selectedStrip() const
{
    auto& strips = ctx.engine.getStrips (selectedKind);
    return juce::isPositiveAndBelow (selectedIndex, (int) strips.size()) ? strips[(size_t) selectedIndex].get() : nullptr;
}

void RoutingPage::select (Strip* s)
{
    if (s == nullptr || ! s->isInput())
        return;
    selectedKind = s->getKind();
    auto& strips = ctx.engine.getStrips (selectedKind);
    for (size_t i = 0; i < strips.size(); ++i)
        if (strips[i].get() == s)
            selectedIndex = (int) i;

    // Keep the selection on screen.
    const auto list = channelTargets();
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i] == s && (int) i / kTargetsPerPage != targetPage)
        {
            targetPage = (int) i / kTargetsPerPage;
            layoutTiles();
        }
    repaint();
}

void RoutingPage::setMode (Mode m)
{
    mode = m;
    if (auto* s = selectedStrip(); s == nullptr || ! s->isInput())
    {
        selectedKind = StripKind::Channel;
        selectedIndex = 0;
    }
    resized();
    repaint();
    notifyStatusKeysChanged();
}

void RoutingPage::refresh()
{
    if (auto* s = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr; s != nullptr && s->isInput())
        select (s);
    layoutTiles();
    repaint();
}

void RoutingPage::timerCallback()
{
    if (ctx.engine.getStructureVersion() != layoutVersion)
        layoutTiles();
    repaint();
}

void RoutingPage::resized()
{
    auto b = getLocalBounds();
    leftPanel = b.removeFromLeft (b.getWidth() * (mode == Mode::Sources ? 52 : 46) / 100);
    b.removeFromLeft (8);
    rightPanel = b;

    const int headerH = juce::jlimit (44, 64, getHeight() / 8);
    leftHeader = leftPanel.withHeight (headerH);
    rightHeader = rightPanel.withHeight (headerH);

    for (auto* r : { &inputCaption, &mainKey, &altKey, &stereoKey, &autoKey, &lockKey, &unlockKey, &prevPage, &nextPage, &leftSelector, &rightSelector,
                     &noneKey, &copyCaption, &srcToChKey, &chToSrcKey, &renameKey })
        *r = {};

    auto lh = leftHeader.reduced (6, 6);
    if (mode == Mode::Sources)
    {
        renameKey = lh.removeFromRight (lh.getHeight() + 10);
        lh.removeFromRight (8);
        leftSelector = lh.withWidth (juce::jmin (lh.getWidth(), 300));
    }
    else
    {
        unlockKey = lh.removeFromRight (lh.getHeight());
        lh.removeFromRight (2);
        lockKey = lh.removeFromRight (lh.getHeight());
        lh.removeFromRight (8);
        autoKey = lh.removeFromRight (juce::jmin (64, lh.getWidth() / 5));
        lh.removeFromRight (8);
        if (mode == Mode::Channels)
        {
            const bool paged = numTargetPages() > 1;
            nextPage = paged ? lh.removeFromRight (lh.getHeight()) : juce::Rectangle<int>();
            if (paged)
                lh.removeFromRight (2);
            prevPage = paged ? lh.removeFromRight (lh.getHeight()) : juce::Rectangle<int>();
            auto input = lh.withWidth (juce::jmin (lh.getWidth(), 210));
            inputCaption = input.removeFromTop (input.getHeight() * 38 / 100);
            mainKey = input.removeFromLeft (input.getWidth() * 4 / 10).reduced (1, 1);
            altKey = input.removeFromLeft (input.getWidth() * 4 / 6).reduced (1, 1);
            stereoKey = input.reduced (4, 1);
        }
        else
            leftSelector = lh.withWidth (juce::jmin (lh.getWidth(), 300));
    }

    auto rh = rightHeader.reduced (6, 6);
    if (mode == Mode::Channels)
    {
        auto copy = rh.removeFromRight (juce::jmin (170, rh.getWidth() / 3));
        copyCaption = copy.removeFromTop (copy.getHeight() * 38 / 100);
        srcToChKey = copy.removeFromLeft (copy.getWidth() / 2).reduced (1, 1);
        chToSrcKey = copy.reduced (1, 1);
        rh.removeFromRight (8);
    }
    if (mode != Mode::Sources)
    {
        noneKey = rh.removeFromRight (rh.getHeight() + 10);
        rh.removeFromRight (8);
        rightSelector = rh.withWidth (juce::jmin (rh.getWidth(), 300));
    }
    layoutTiles();
}

void RoutingPage::layoutTiles()
{
    layoutVersion = ctx.engine.getStructureVersion();
    targetPage = juce::jlimit (0, numTargetPages() - 1, targetPage);
    inputGroup = juce::jlimit (0, numPortGroups (false) - 1, inputGroup);
    outputGroup = juce::jlimit (0, numPortGroups (true) - 1, outputGroup);
    channelTiles.clear();
    leftPorts.clear();
    rightPorts.clear();
    signalTiles.clear();
    separator = {};
    if (leftPanel.isEmpty())
        return;

    auto grid = leftPanel.withTrimmedTop (leftHeader.getHeight() + 6).reduced (6);
    auto rightGrid = rightPanel.withTrimmedTop (rightHeader.getHeight() + 6).reduced (6);
    auto portGrid = [] (juce::Rectangle<int> area, int count, int rows, std::vector<juce::Rectangle<int>>& out)
    {
        const int w = area.getWidth() / kTilesPerRow, h = juce::jmin (area.getHeight() / rows, w * 5 / 4);
        for (int i = 0; i < count; ++i)
            out.push_back (juce::Rectangle<int> (area.getX() + (i % kTilesPerRow) * w, area.getY() + (i / kTilesPerRow) * h, w, h).reduced (3));
    };

    if (mode == Mode::Channels)
    {
        // Up to 48 channels, with a gap before the first aux.
        const auto list = channelTargets();
        const int first = targetPage * kTargetsPerPage;
        const int count = juce::jmin (kTargetsPerPage, (int) list.size() - first);
        const int sepH = 14;
        const int rows = juce::jmax (6, (count + kTilesPerRow - 1) / kTilesPerRow);
        const int tileW = grid.getWidth() / kTilesPerRow;
        const int tileH = juce::jmin ((grid.getHeight() - sepH) / rows, tileW * 5 / 4);
        int row = 0, col = 0;
        bool separated = false;
        for (int i = 0; i < count; ++i)
        {
            auto* s = list[(size_t) (first + i)];
            if (! separated && s->getKind() == StripKind::Aux && i > 0)
            {
                if (col != 0)
                {
                    col = 0;
                    ++row;
                }
                separator = juce::Rectangle<int> (grid.getX() + 8, grid.getY() + row * tileH + sepH / 2 - 1, grid.getWidth() - 16, 2);
                separated = true;
            }
            const int y = grid.getY() + row * tileH + (separated ? sepH : 0);
            channelTiles.push_back ({ juce::Rectangle<int> (grid.getX() + col * tileW, y, tileW, tileH).reduced (3), s });
            if (++col == kTilesPerRow)
            {
                col = 0;
                ++row;
            }
        }
        portGrid (rightGrid, kPortsPerGroup, kPortsPerGroup / kTilesPerRow, rightPorts);
    }
    else if (mode == Mode::Sources)
        portGrid (grid, kPortsPerGroup, kPortsPerGroup / kTilesPerRow, leftPorts);
    else
    {
        portGrid (grid, kPortsPerGroup, kPortsPerGroup / kTilesPerRow, leftPorts);
        const auto list = signals();
        const int pages = juce::jmax (1, ((int) list.size() + kTargetsPerPage - 1) / kTargetsPerPage);
        signalPage = juce::jlimit (0, pages - 1, signalPage);
        std::vector<juce::Rectangle<int>> areas;
        portGrid (rightGrid, kTargetsPerPage, kTargetsPerPage / kTilesPerRow, areas);
        for (int i = 0; i < kTargetsPerPage && signalPage * kTargetsPerPage + i < (int) list.size(); ++i)
            signalTiles.push_back ({ areas[(size_t) i], list[(size_t) (signalPage * kTargetsPerPage + i)] });
    }
}

juce::String RoutingPage::inputUsers (int port) const
{
    juce::StringArray users;
    for (auto* s : channelTargets())
    {
        const int l = s->params.inputLeft.load(), r = s->params.inputRight.load();
        const bool stereo = s->params.stereo.load();
        if (l == port || (stereo && (r == port || (r < 0 && l >= 0 && l + 1 == port))))
            users.add (hasCustomName (*s) ? s->getLabel() + " " + stripName (*s).toUpperCase() : s->getLabel());
    }
    return users.joinIntoString (", ");
}

RoutingPage::Signal RoutingPage::feederOf (int port) const
{
    for (int k = 0; k < kNumStripKinds; ++k)
        for (auto& s : ctx.engine.getStrips (static_cast<StripKind> (k)))
        {
            if (s->params.outputLeft.load() == port)
                return { s.get(), 0 };
            if (s->params.outputRight.load() == port)
                return { s.get(), 1 };
        }
    if (ctx.engine.monitorOutputLeft.load() == port)
        return { nullptr, 0 };
    if (ctx.engine.monitorOutputRight.load() == port)
        return { nullptr, 1 };
    return { nullptr, -1 };
}

juce::String RoutingPage::signalLabel (const Signal& sig) const
{
    if (sig.channel < 0)
        return {};
    if (sig.strip == nullptr)
        return sig.channel == 0 ? "MON L" : "MON R";
    const bool stereo = sig.strip->isOutput() || sig.strip->params.stereo.load();
    return shortStripLabel (*sig.strip) + (stereo ? (sig.channel == 0 ? " L" : " R") : juce::String());
}

void RoutingPage::paintChannelTile (juce::Graphics& g, juce::Rectangle<int> area, Strip& s, bool selected) const
{
    auto r = area.toFloat();
    auto& p = s.params;
    const int port = p.inputLeft.load();
    const bool patched = port >= 0;

    auto bar = r.removeFromTop (juce::jmax (12.0f, r.getHeight() * 0.22f));
    g.setColour (patched || hasCustomName (s) ? s.getColour() : unpatched);
    g.fillRect (bar);
    if (hasCustomName (s))
    {
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (juce::jmin (15.0f, bar.getHeight() * 0.95f), juce::Font::bold));
        g.drawFittedText (stripName (s).toUpperCase(), bar.toNearestInt().reduced (2, 0), juce::Justification::centred, 1);
    }
    g.setColour (patched ? tileOn : tileOff);
    g.fillRect (r);

    auto label = r.removeFromBottom (juce::jmin (20.0f, r.getHeight() * 0.36f));
    if (patched)
    {
        drawIcon (g, stripIcon (s.getKind()), r.reduced (r.getWidth() * 0.25f, 2.0f), juce::Colours::white);
        // Mono sources in blue, stereo in olive.
        const bool stereo = p.stereo.load();
        g.setColour (stereo ? stereoLabel : monoLabel);
        g.setFont (juce::FontOptions (juce::jmin (15.0f, label.getHeight() * 0.9f), juce::Font::bold));
        juce::String text = portName (port, false);
        if (stereo)
            text = juce::String (port + 1) + "/" + juce::String ((p.inputRight.load() >= 0 ? p.inputRight.load() : port + 1) + 1);
        g.drawFittedText (text, label.toNearestInt().reduced (2, 0), juce::Justification::centred, 1);
    }
    else
    {
        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (juce::jmin (16.0f, label.getHeight() * 0.95f), juce::Font::bold));
        g.drawFittedText (shortStripLabel (s), label.toNearestInt(), juce::Justification::centred, 1);
    }

    if (selected)
    {
        g.setColour (Colours::yellow);
        g.drawRect (area.toFloat().expanded (2.0f), 3.0f);
    }
}

void RoutingPage::paintInputPort (juce::Graphics& g, juce::Rectangle<int> area, int port, bool marked) const
{
    auto r = area.toFloat();
    g.setColour (navy);
    g.fillRect (r.removeFromTop (juce::jmax (10.0f, r.getHeight() * 0.18f)));
    g.setColour (tileOff);
    g.fillRect (r);
    auto label = r.removeFromBottom (juce::jmin (20.0f, r.getHeight() * 0.38f));
    drawIcon (g, Icon::Xlr, r.reduced (r.getWidth() * 0.3f, 2.0f), Dark::textDim);
    g.setColour (monoLabel);
    g.setFont (juce::FontOptions (juce::jmin (15.0f, label.getHeight() * 0.9f), juce::Font::bold));
    g.drawFittedText (portName (port, false), label.toNearestInt().reduced (2, 0), juce::Justification::centred, 1);
    if (marked)
    {
        g.setColour (Colours::yellow);
        g.drawRect (area.toFloat().expanded (2.0f), 3.0f);
    }
}

void RoutingPage::paintOutputPort (juce::Graphics& g, juce::Rectangle<int> area, int port, bool selected) const
{
    const auto feeder = feederOf (port);
    const bool fed = feeder.channel >= 0;
    auto r = area.toFloat();
    auto bar = r.removeFromTop (juce::jmax (12.0f, r.getHeight() * 0.22f));
    g.setColour (feeder.strip != nullptr ? feeder.strip->getColour() : (fed ? Colours::lime : navy));
    g.fillRect (bar);
    if (feeder.strip != nullptr && hasCustomName (*feeder.strip))
    {
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (juce::jmin (15.0f, bar.getHeight() * 0.95f), juce::Font::bold));
        g.drawFittedText (stripName (*feeder.strip).toUpperCase(), bar.toNearestInt().reduced (2, 0), juce::Justification::centred, 1);
    }
    g.setColour (fed ? tileOn : tileOff);
    g.fillRect (r);
    auto label = r.removeFromBottom (juce::jmin (20.0f, r.getHeight() * 0.38f));
    auto icon = r.reduced (r.getWidth() * 0.28f, 2.0f);
    drawIcon (g, Icon::Speaker, icon, fed ? juce::Colours::white : Dark::textDim);
    g.setColour (Dark::textDim);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText (juce::String (port + 1), r.reduced (3.0f, 1.0f).toNearestInt(), juce::Justification::topLeft);
    g.setColour (fed ? monoLabel : Dark::textDim);
    g.setFont (juce::FontOptions (juce::jmin (15.0f, label.getHeight() * 0.9f), juce::Font::bold));
    g.drawFittedText (fed ? signalLabel (feeder) : portName (port, true), label.toNearestInt().reduced (2, 0), juce::Justification::centred, 1);
    if (selected)
    {
        g.setColour (Colours::yellow);
        g.drawRect (area.toFloat().expanded (2.0f), 3.0f);
    }
}

void RoutingPage::paintSignal (juce::Graphics& g, juce::Rectangle<int> area, const Signal& sig, bool marked) const
{
    auto r = area.toFloat();
    auto bar = r.removeFromTop (juce::jmax (12.0f, r.getHeight() * 0.22f));
    g.setColour (sig.strip != nullptr ? sig.strip->getColour() : Colours::lime);
    g.fillRect (bar);
    g.setColour (juce::Colours::black);
    g.setFont (juce::FontOptions (juce::jmin (15.0f, bar.getHeight() * 0.95f), juce::Font::bold));
    const juce::String name = sig.strip != nullptr ? (hasCustomName (*sig.strip) ? stripName (*sig.strip).toUpperCase() : juce::String())
                                                   : (sig.channel == 0 ? juce::String ("PHONES") : juce::String ("SPEAKERS"));
    g.drawFittedText (name, bar.toNearestInt().reduced (2, 0), juce::Justification::centred, 1);
    g.setColour (tileOn);
    g.fillRect (r);
    auto label = r.removeFromBottom (juce::jmin (20.0f, r.getHeight() * 0.38f));
    drawIcon (g, sig.strip != nullptr ? stripIcon (sig.strip->getKind()) : (sig.channel == 0 ? Icon::Headphones : Icon::Speakers),
              r.reduced (r.getWidth() * 0.28f, 2.0f), juce::Colours::white);
    const bool stereo = sig.strip == nullptr || sig.strip->isOutput() || sig.strip->params.stereo.load();
    g.setColour (stereo ? stereoLabel : monoLabel);
    g.setFont (juce::FontOptions (juce::jmin (15.0f, label.getHeight() * 0.9f), juce::Font::bold));
    g.drawFittedText (signalLabel (sig), label.toNearestInt().reduced (2, 0), juce::Justification::centred, 1);
    if (marked)
    {
        g.setColour (Colours::yellow);
        g.drawRect (area.toFloat().expanded (2.0f), 3.0f);
    }
}

void RoutingPage::paintSourcePanel (juce::Graphics& g) const
{
    // The selected source's settings. Sources (names, gain, +48V, polarity,
    // mono/stereo grouping) don't exist yet: channels patch hardware inputs
    // directly, so these are drawn disabled.
    auto r = rightPanel;
    g.setColour (Dark::body);
    g.fillRect (r);
    auto tabs = r.removeFromTop (juce::jlimit (30, 40, r.getHeight() / 11));
    const char* names[] = { "SETTINGS", "ICON/COLOR", "TAGS" };
    const int tw = tabs.getWidth() / 3;
    for (int i = 0; i < 3; ++i)
    {
        auto t = tabs.removeFromLeft (tw).reduced (1, 0);
        g.setColour (i == 0 ? Dark::tabOn : Dark::tabOff);
        g.fillRect (t);
        if (i == 0)
        {
            g.setColour (Colours::yellow);
            g.fillRect (t.withHeight (3));
        }
        g.setColour (i == 0 ? Colours::yellow : Dark::textDim);
        g.setFont (juce::FontOptions (juce::jmin (17.0f, (float) t.getHeight() * 0.5f), juce::Font::bold));
        g.drawText (names[i], t, juce::Justification::centred);
    }
    r.removeFromTop (6);
    auto banner = r.removeFromTop (juce::jlimit (30, 46, r.getHeight() / 9)).reduced (6, 0);
    g.setColour (Dark::header);
    g.fillRect (banner);
    const auto users = inputUsers (selectedSource);
    g.setColour (users.isEmpty() ? Dark::textDim : Dark::text);
    g.setFont (juce::FontOptions (juce::jmin (18.0f, (float) banner.getHeight() * 0.5f), juce::Font::bold));
    g.drawFittedText (users.isEmpty() ? juce::String ("NO CONNECTIONS") : users, banner.reduced (8, 0), juce::Justification::centred, 2);
    r.removeFromTop (8);

    auto body = r.reduced (6, 0);
    auto left = body.removeFromLeft (body.getWidth() / 2);
    const float dim = 0.4f;
    auto key = [&g, dim] (juce::Rectangle<int> k, const juce::String& text, bool lit)
    {
        g.setColour ((lit ? Dark::choice : Dark::key).withMultipliedAlpha (dim + 0.2f));
        g.fillRect (k);
        g.setColour (Dark::text.withAlpha (dim));
        g.setFont (juce::FontOptions (juce::jmin (16.0f, (float) k.getHeight() * 0.45f), juce::Font::bold));
        g.drawFittedText (text, k.reduced (4, 0), juce::Justification::centred, 1);
    };
    const int kh = juce::jlimit (26, 40, body.getHeight() / 8);
    key (left.removeFromTop (kh).withWidth (juce::jmin (left.getWidth(), 160)), "INITIALIZE", false);
    left.removeFromTop (10);
    g.setColour (Dark::text.withAlpha (dim));
    g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    g.drawText (portName (selectedSource, false), left.removeFromTop (20), juce::Justification::centredLeft);
    for (auto [text, lit] : { std::pair { "MONO", true }, std::pair { "STEREO", false }, std::pair { "MID/SIDE", false } })
    {
        key (left.removeFromTop (kh).withWidth (juce::jmin (left.getWidth(), 160)), text, lit);
        left.removeFromTop (4);
    }
    auto level = body.reduced (8, 0);
    for (auto* text : { "+48V", "POL", "MUTE" })
    {
        key (level.removeFromTop (kh).withWidth (juce::jmin (level.getWidth(), 110)), text, false);
        level.removeFromTop (4);
    }
    level.removeFromTop (6);
    g.setColour (Dark::textDim);
    g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    g.drawFittedText ("SOURCE SETTINGS ARE NOT AVAILABLE YET. CHANNELS USE THE INPUTS DIRECTLY: SET MONO / STEREO ON THE CHANNEL'S INPUT SCREEN.", level,
                      juce::Justification::topLeft, 5);
}

void RoutingPage::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);

    for (auto panel : { leftPanel, rightPanel })
    {
        g.setColour (Dark::body);
        g.fillRect (panel);
    }
    g.setColour (Dark::header);
    g.fillRect (leftHeader);
    if (mode != Mode::Sources)
        g.fillRect (rightHeader);

    auto* sel = selectedStrip();
    auto caption = [&g] (juce::Rectangle<int> r, const juce::String& text)
    {
        if (r.isEmpty())
            return;
        g.setColour (juce::Colours::black);
        g.fillRect (r);
        g.setColour (Dark::text);
        g.setFont (juce::FontOptions (juce::jmin (12.0f, (float) r.getHeight() * 0.8f), juce::Font::bold));
        g.drawText (text, r, juce::Justification::centred);
    };

    // Left header.
    if (mode == Mode::Channels)
    {
        // The channel's MAIN / ALT input: no ALT inputs yet (engine E-ALT). ST pairs the input with the next one.
        caption (inputCaption, "CHANNEL INPUT");
        drawKey (g, mainKey, "MAIN", true, Dark::choice);
        drawKey (g, altKey, "ALT", false, Dark::choice, false);
        drawKey (g, stereoKey, "ST", sel != nullptr && sel->params.stereo.load(), Dark::choice);
        drawKey (g, prevPage, "<", false, Dark::choice);
        drawKey (g, nextPage, ">", false, Dark::choice);
    }
    if (mode == Mode::Sources)
    {
        drawSelector (g, leftSelector, Icon::Xlr, "SOURCE GROUP",
                      "AUDIO IN " + juce::String (inputGroup * kPortsPerGroup + 1) + "-" + juce::String (inputGroup * kPortsPerGroup + kPortsPerGroup));
        drawIconKey (g, renameKey, Icon::Edit, "RENAME", false);
    }
    else
    {
        if (mode == Mode::Outputs)
            drawSelector (g, leftSelector, Icon::Speaker, "OUTPUT GROUP",
                          "AUDIO OUT " + juce::String (outputGroup * kPortsPerGroup + 1) + "-" + juce::String (outputGroup * kPortsPerGroup + kPortsPerGroup));
        drawKey (g, autoKey, "+1\nAUTO", autoAdvance, Colours::yellow);
        // Locked: the closed lock lit red; unlocked: the open lock lit green (as on the console).
        g.setColour (locked ? juce::Colour (0xffe8344a) : Dark::key);
        g.fillRect (lockKey);
        drawIcon (g, Icon::LockClosed, lockKey.toFloat().reduced ((float) lockKey.getHeight() * 0.2f), locked ? juce::Colours::black : Dark::text);
        g.setColour (! locked ? Colours::green : Dark::key);
        g.fillRect (unlockKey);
        drawIcon (g, Icon::LockOpen, unlockKey.toFloat().reduced ((float) unlockKey.getHeight() * 0.2f), ! locked ? juce::Colours::black : Dark::text);
    }

    // Right header.
    if (mode == Mode::Channels)
    {
        drawSelector (g, rightSelector, Icon::Xlr, "SOURCE GROUP",
                      "AUDIO IN " + juce::String (inputGroup * kPortsPerGroup + 1) + "-" + juce::String (inputGroup * kPortsPerGroup + kPortsPerGroup));
        // Source customization doesn't exist yet.
        caption (copyCaption, "COPY CUSTOMIZATION");
        drawKey (g, srcToChKey, "SRC->CH", false, Dark::choice, false);
        drawKey (g, chToSrcKey, "CH->SRC", false, Dark::choice, false);
    }
    else if (mode == Mode::Outputs)
        drawSelector (g, rightSelector, Icon::Bus, "SOURCE GROUP", signalGroups[juce::jlimit (0, 5, signalGroup)]);
    if (mode != Mode::Sources)
        drawIconKey (g, noneKey, Icon::ClearCircle, "NONE");

    // Grids.
    for (auto panel : { leftPanel, rightPanel })
        if (mode != Mode::Sources || panel == leftPanel)
        {
            g.setColour (juce::Colour (0xff141414));
            g.fillRect (panel.withTrimmedTop (leftHeader.getHeight() + 6).reduced (2));
        }
    if (! separator.isEmpty())
    {
        g.setColour (juce::Colour (0xff6a6a6a));
        g.fillRect (separator);
    }

    if (mode == Mode::Channels)
    {
        for (auto& [area, strip] : channelTiles)
            paintChannelTile (g, area, *strip, strip == sel);
        for (size_t i = 0; i < rightPorts.size(); ++i)
        {
            const int port = inputGroup * kPortsPerGroup + (int) i;
            bool mine = false;
            if (sel != nullptr)
            {
                const int l = sel->params.inputLeft.load(), r = sel->params.inputRight.load();
                mine = l == port || (sel->params.stereo.load() && (r == port || (r < 0 && l >= 0 && l + 1 == port)));
            }
            paintInputPort (g, rightPorts[i], port, mine);
        }
    }
    else if (mode == Mode::Sources)
    {
        for (size_t i = 0; i < leftPorts.size(); ++i)
            paintInputPort (g, leftPorts[i], inputGroup * kPortsPerGroup + (int) i, inputGroup * kPortsPerGroup + (int) i == selectedSource);
        paintSourcePanel (g);
    }
    else
    {
        for (size_t i = 0; i < leftPorts.size(); ++i)
            paintOutputPort (g, leftPorts[i], outputGroup * kPortsPerGroup + (int) i, outputGroup * kPortsPerGroup + (int) i == selectedPort);
        const auto feeder = feederOf (selectedPort);
        for (auto& [area, sig] : signalTiles)
            paintSignal (g, area, sig, feeder.channel >= 0 && feeder.strip == sig.strip && feeder.channel == sig.channel);
    }

    if (locked && mode != Mode::Sources)
    {
        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.fillRect (rightPanel.withTrimmedTop (rightHeader.getHeight() + 6));
    }
}

void RoutingPage::assignInput (int port, bool right)
{
    auto* s = selectedStrip();
    if (s == nullptr || locked)
        return;
    auto& p = s->params;
    if (right)
        p.inputRight = port;
    else
    {
        p.inputLeft = port;
        p.inputRight = -1; // stereo channels take the next input
    }

    if (autoAdvance && ! right)
    {
        const auto list = channelTargets();
        for (size_t i = 0; i + 1 < list.size(); ++i)
            if (list[i] == s)
            {
                select (list[i + 1]);
                break;
            }
    }
    repaint();
}

void RoutingPage::clearOutput (int port)
{
    for (int k = 0; k < kNumStripKinds; ++k)
        for (auto& s : ctx.engine.getStrips (static_cast<StripKind> (k)))
        {
            if (s->params.outputLeft.load() == port)
                s->params.outputLeft = -1;
            if (s->params.outputRight.load() == port)
                s->params.outputRight = -1;
        }
    if (ctx.engine.monitorOutputLeft.load() == port)
        ctx.engine.monitorOutputLeft = -1;
    if (ctx.engine.monitorOutputRight.load() == port)
        ctx.engine.monitorOutputRight = -1;
}

void RoutingPage::assignOutput (int port, const Signal& sig)
{
    if (locked)
        return;
    // One signal per output. A strip has one output pair, so patching it to a
    // new output moves it there (engine item E4: many outputs per signal).
    clearOutput (port);
    if (sig.strip == nullptr)
        (sig.channel == 0 ? ctx.engine.monitorOutputLeft : ctx.engine.monitorOutputRight) = port;
    else
        (sig.channel == 0 ? sig.strip->params.outputLeft : sig.strip->params.outputRight) = port;
    if (autoAdvance)
        selectedPort = juce::jmin (numPorts (true) - 1, selectedPort + 1);
    if (selectedPort / kPortsPerGroup != outputGroup)
    {
        outputGroup = selectedPort / kPortsPerGroup;
        layoutTiles();
    }
    repaint();
}

void RoutingPage::showGroupMenu (bool rightSide)
{
    juce::PopupMenu menu;
    int current = 0;
    if (mode == Mode::Outputs && rightSide)
    {
        for (int i = 0; i < (int) std::size (signalGroups); ++i)
            menu.addItem (i + 1, signalGroups[i], true, i == signalGroup);
        current = 1;
    }
    else
    {
        const bool outputs = mode == Mode::Outputs;
        for (int i = 0; i < numPortGroups (outputs); ++i)
        {
            const int a = i * kPortsPerGroup + 1;
            menu.addItem (i + 1, (outputs ? "AUDIO OUT " : "AUDIO IN ") + juce::String (a) + "-" + juce::String (a + kPortsPerGroup - 1), true,
                          i == (outputs ? outputGroup : inputGroup));
        }
        current = outputs ? 2 : 0;
    }
    juce::Component::SafePointer<RoutingPage> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withTargetScreenArea (localAreaToGlobal (rightSide ? rightSelector : leftSelector)),
                        [safe, current] (int r)
                        {
                            if (safe == nullptr || r <= 0)
                                return;
                            if (current == 1)
                            {
                                safe->signalGroup = r - 1;
                                safe->signalPage = 0;
                            }
                            else if (current == 2)
                                safe->outputGroup = r - 1;
                            else
                                safe->inputGroup = r - 1;
                            safe->layoutTiles();
                            safe->repaint();
                        });
}

void RoutingPage::mouseUp (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();
    if (autoKey.contains (pos))
        autoAdvance = ! autoAdvance;
    else if (lockKey.contains (pos))
        locked = true;
    else if (unlockKey.contains (pos))
        locked = false;
    else if (prevPage.contains (pos) || nextPage.contains (pos))
    {
        targetPage = (targetPage + (nextPage.contains (pos) ? 1 : numTargetPages() - 1)) % numTargetPages();
        layoutTiles();
    }
    else if (leftSelector.contains (pos))
        showGroupMenu (false);
    else if (rightSelector.contains (pos))
        showGroupMenu (true);
    else if (stereoKey.contains (pos))
    {
        if (auto* s = selectedStrip(); s != nullptr && ! locked)
            s->params.stereo = ! s->params.stereo.load();
    }
    else if (noneKey.contains (pos))
    {
        if (locked)
            return;
        if (mode == Mode::Outputs)
            clearOutput (selectedPort);
        else if (auto* s = selectedStrip())
        {
            s->params.inputLeft = -1;
            s->params.inputRight = -1;
        }
    }
    else
    {
        for (auto& [area, strip] : channelTiles)
            if (area.contains (pos))
            {
                select (strip);
                if (ctx.selectStrip)
                    ctx.selectStrip (strip);
                return;
            }
        for (size_t i = 0; i < rightPorts.size(); ++i)
            if (rightPorts[i].contains (pos))
            {
                // Right click or shift: the right input of a stereo channel.
                assignInput (inputGroup * kPortsPerGroup + (int) i, e.mods.isPopupMenu() || e.mods.isShiftDown());
                return;
            }
        for (size_t i = 0; i < leftPorts.size(); ++i)
            if (leftPorts[i].contains (pos))
            {
                if (mode == Mode::Sources)
                    selectedSource = inputGroup * kPortsPerGroup + (int) i;
                else
                    selectedPort = outputGroup * kPortsPerGroup + (int) i;
                repaint();
                return;
            }
        for (auto& [area, sig] : signalTiles)
            if (area.contains (pos))
            {
                assignOutput (selectedPort, sig);
                return;
            }
    }
    repaint();
}

} // namespace livedaw::ui
