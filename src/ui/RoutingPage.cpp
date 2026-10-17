#include "dsp/DspUtils.h"
#include "ui/Pages.h"

namespace livedaw::ui
{

namespace
{
    constexpr int kTilesPerRow = 8, kTargetsPerPage = 48, kPortsPerGroup = 48;

    // ROUTING's own colours [3.1]: port labels blue for mono and olive for
    // stereo, ALT orange, +1 AUTO yellow, the locked padlock red.
    const juce::Colour monoLabel (0xff4890e0), stereoLabel (0xffa0b418), altOrange (0xffb86018), autoYellow (0xffe0d400), lockRed (0xfff04040),
        playbackStrip (0xff2f5a32);

    /** A ROUTING tile (D13, D14): a name strip in the signal's colour over a
        #303030 body with the icon at the left, the number at the right and the
        port label at the bottom. */
    struct TileSpec
    {
        juce::Colour strip;
        juce::String name, number, label;
        juce::Colour labelColour = monoLabel;
        std::function<void (juce::Graphics&, juce::Rectangle<float>)> icon;
        juce::String chip; // "ALT", top right of the body
        juce::Colour chipColour = altOrange;
        bool selected = false;
    };

    void paintTile (juce::Graphics& g, juce::Rectangle<int> area, const TileSpec& t)
    {
        auto r = area.toFloat();
        auto bar = r.removeFromTop (juce::jmax (12.0f, r.getHeight() * 0.22f));
        g.setColour (t.strip);
        g.fillRect (bar);
        if (t.name.isNotEmpty())
        {
            g.setColour (juce::Colours::black);
            g.setFont (juce::FontOptions (juce::jmin (15.0f, bar.getHeight() * 0.95f), juce::Font::bold));
            g.drawFittedText (t.name, bar.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.7f);
        }
        g.setColour (Colours::screenTile);
        g.fillRect (r);

        auto label = r.removeFromBottom (juce::jmin (20.0f, r.getHeight() * 0.36f));
        g.setColour (t.labelColour);
        g.setFont (juce::FontOptions (juce::jmin (15.0f, label.getHeight() * 0.9f), juce::Font::bold));
        g.drawFittedText (t.label, label.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.7f);

        auto body = r.reduced (3.0f, 2.0f);
        auto numberArea = body.removeFromRight (body.getWidth() * 0.42f);
        if (t.icon)
            t.icon (g, body.withSizeKeepingCentre (juce::jmin (body.getWidth(), body.getHeight()), body.getHeight()).reduced (1.0f));
        g.setColour (Dark::text);
        g.setFont (juce::FontOptions (juce::jlimit (9.0f, 18.0f, juce::jmin (numberArea.getHeight() * 0.7f, numberArea.getWidth() * 0.6f)), juce::Font::bold));
        g.drawFittedText (t.number, numberArea.toNearestInt(), juce::Justification::centredRight, 1, 0.6f);
        if (t.chip.isNotEmpty())
            drawChip (g, r.withHeight (12.0f).withTrimmedLeft (r.getWidth() - 24.0f).translated (-2.0f, 2.0f), t.chip, t.chipColour, juce::Colours::black);

        if (t.selected)
        {
            g.setColour (Colours::yellow);
            g.drawRect (area.toFloat().expanded (2.0f), 3.0f);
        }
    }

    std::function<void (juce::Graphics&, juce::Rectangle<float>)> iconOf (Icon icon, juce::Colour c = Dark::text)
    {
        return [icon, c] (juce::Graphics& g, juce::Rectangle<float> r) { drawIcon (g, icon, r, c); };
    }

    std::function<void (juce::Graphics&, juce::Rectangle<float>)> iconOf (const Strip& s)
    {
        return [&s] (juce::Graphics& g, juce::Rectangle<float> r) { drawStripIcon (g, s, r, Dark::text); };
    }

    const char* const signalGroups[] = { "CHANNEL", "AUX", "BUS", "MAIN", "MATRIX", "MONITOR" };

    bool hasCustomName (const Strip& s)
    {
        return stripName (s) != s.getLabel();
    }

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

    /** A drop-down field: a small caption over a big value, and the dots key at the right. */
    void drawField (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& caption, const juce::String& value, juce::Colour valueColour)
    {
        if (r.isEmpty())
            return;
        g.setColour (Dark::key);
        g.fillRect (r);
        auto dots = r.removeFromRight (juce::jmin (r.getHeight() / 2, 24)).toFloat();
        g.setColour (Dark::text);
        const float d = juce::jlimit (3.0f, 6.0f, dots.getHeight() * 0.12f);
        for (int i = -1; i <= 1; ++i)
            g.fillEllipse (juce::Rectangle<float> (d, d).withCentre (dots.getCentre().translated (0.0f, (float) i * d * 2.0f)));
        auto t = r.reduced (6, 2);
        g.setColour (Dark::textDim);
        g.setFont (juce::FontOptions (juce::jmin (13.0f, (float) t.getHeight() * 0.32f), juce::Font::bold));
        g.drawText (caption, t.removeFromTop (t.getHeight() * 40 / 100), juce::Justification::bottomLeft);
        g.setColour (valueColour);
        g.setFont (juce::FontOptions (juce::jmin (22.0f, (float) t.getHeight() * 0.85f), juce::Font::bold));
        g.drawFittedText (value, t, juce::Justification::centredLeft, 1);
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

RoutingPage::RoutingPage (UiContext& c) : ctx (c)
{
    // Where the drop-down lists of the hand-drawn fields open.
    anchor.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (anchor);
    startTimerHz (5);
}

StatusTitle RoutingPage::getStatusTitle()
{
    // SOURCES and OUTPUTS show the selected source or output (grey tile,
    // connector icon), CHANNELS the selected channel.
    StatusTitle t;
    if (mode == Mode::Sources)
    {
        t.kind = StatusTitle::Kind::Source;
        const bool playback = Recorder::isPlaybackSource (selectedSource);
        t.prefix = juce::String ((playback ? Recorder::playbackChannelOf (selectedSource) : selectedSource) + 1);
        t.text = playback ? "PLAYBACK" : "AUDIO IN";
        t.icon = playback ? Icon::Waveform : Icon::Xlr;
    }
    else if (mode == Mode::Outputs)
    {
        t.kind = StatusTitle::Kind::Source;
        t.prefix = juce::String (selectedPort + 1);
        t.text = "AUDIO OUT";
        t.icon = Icon::Speaker;
    }
    return t;
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

    for (auto* r : { &inputField, &stereoKey, &autoKey, &lockKey, &unlockKey, &prevPage, &nextPage, &leftSelector, &rightSelector,
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
            stereoKey = input.removeFromRight (input.getHeight()).reduced (2, 1);
            input.removeFromRight (4);
            inputField = input;
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
    inputGroup = juce::jlimit (0, numSourceGroups() - 1, inputGroup);
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
        const auto name = hasCustomName (*s) ? s->getLabel() + " " + stripName (*s).toUpperCase() : s->getLabel();
        const bool stereo = s->params.stereo.load();
        const int l = s->getInputLeft(), r = s->getInputRight();
        if (l == port || (stereo && (r == port || (r < 0 && l >= 0 && l + 1 == port))))
            users.add (name);
        const int al = s->getAltInputLeft(), ar = s->getAltInputRight();
        if (al == port || (stereo && (ar == port || (ar < 0 && al >= 0 && al + 1 == port))))
            users.add (name + " (ALT)");
    }
    return users.joinIntoString (", ");
}

int RoutingPage::numSourceGroups() const
{
    return numPortGroups (false) + (Recorder::kNumPlaybackChannels + kPortsPerGroup - 1) / kPortsPerGroup;
}

juce::String RoutingPage::sourceGroupName (int group) const
{
    const int hw = numPortGroups (false);
    if (group < hw)
        return "AUDIO IN " + juce::String (group * kPortsPerGroup + 1) + "-" + juce::String (group * kPortsPerGroup + kPortsPerGroup);
    const int first = (group - hw) * kPortsPerGroup;
    return "PLAY " + juce::String (first + 1) + "-" + juce::String (juce::jmin (Recorder::kNumPlaybackChannels, first + kPortsPerGroup));
}

int RoutingPage::sourceAt (int group, int index) const
{
    const int hw = numPortGroups (false);
    if (group < hw)
        return group * kPortsPerGroup + index;
    const int channel = (group - hw) * kPortsPerGroup + index;
    return channel < Recorder::kNumPlaybackChannels ? Recorder::playbackSource (channel) : -1;
}

juce::String RoutingPage::sourceName (int source) const
{
    if (source < 0)
        return "NONE";
    if (Recorder::isPlaybackSource (source))
        return Recorder::playbackSourceName (Recorder::playbackChannelOf (source));
    return portName (source, false);
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
    // The input being patched (MAIN or ALT); a channel listening to its ALT
    // input is marked "ALT" in orange. Without any source the colour is dim.
    const int port = editAlt ? s.getAltInputLeft() : s.getInputLeft();
    const int portRight = editAlt ? s.getAltInputRight() : s.getInputRight();
    const bool stereo = s.params.stereo.load();

    TileSpec t;
    t.strip = stripDisplayColour (s);
    t.name = hasCustomName (s) ? stripName (s).toUpperCase() : juce::String();
    t.number = shortStripLabel (s);
    t.icon = iconOf (s);
    t.selected = selected;
    t.chip = s.isUsingAltInput() ? "ALT" : "";
    if (port >= 0)
    {
        // Mono sources in blue, stereo in olive.
        t.labelColour = stereo ? stereoLabel : monoLabel;
        t.label = sourceName (port);
        if (stereo && ! Recorder::isPlaybackSource (port))
            t.label = juce::String (port + 1) + "/" + juce::String ((portRight >= 0 ? portRight : port + 1) + 1);
        else if (stereo)
            t.label = "PLAY " + juce::String (Recorder::playbackChannelOf (port) + 1) + "/"
                      + juce::String (Recorder::playbackChannelOf (portRight >= 0 ? portRight : port + 1) + 1);
    }
    paintTile (g, area, t);
}

void RoutingPage::paintInputPort (juce::Graphics& g, juce::Rectangle<int> area, int port, bool marked) const
{
    if (port < 0)
        return;
    // Sources have no colour of their own yet: the default blue (W32).
    const bool playback = Recorder::isPlaybackSource (port);
    TileSpec t;
    t.strip = playback ? playbackStrip : wingColour (1);
    t.number = juce::String ((playback ? Recorder::playbackChannelOf (port) : port) + 1);
    t.label = sourceName (port);
    t.labelColour = playback ? FileColours::wav : monoLabel;
    t.icon = iconOf (playback ? Icon::Waveform : Icon::Xlr, Dark::textDim);
    t.selected = marked;
    paintTile (g, area, t);
}

void RoutingPage::paintOutputPort (juce::Graphics& g, juce::Rectangle<int> area, int port, bool selected) const
{
    const auto feeder = feederOf (port);
    if (feeder.channel < 0)
    {
        // An unpatched output: only its number in a ring.
        auto r = area.toFloat();
        g.setColour (Colours::screenTile);
        g.fillRect (r);
        const float d = juce::jmin (r.getWidth(), r.getHeight()) * 0.56f;
        auto ring = juce::Rectangle<float> (d, d).withCentre (r.getCentre());
        g.setColour (Dark::textDim);
        g.drawEllipse (ring, 2.0f);
        g.setFont (juce::FontOptions (juce::jmin (18.0f, d * 0.48f), juce::Font::bold));
        g.drawFittedText (juce::String (port + 1), ring.toNearestInt(), juce::Justification::centred, 1, 0.6f);
        if (selected)
        {
            g.setColour (Colours::yellow);
            g.drawRect (r.expanded (2.0f), 3.0f);
        }
        return;
    }

    TileSpec t;
    t.strip = feeder.strip != nullptr ? stripDisplayColour (*feeder.strip) : Colours::lime;
    t.name = feeder.strip != nullptr ? (hasCustomName (*feeder.strip) ? stripName (*feeder.strip).toUpperCase() : juce::String())
                                     : (feeder.channel == 0 ? juce::String ("PHONES") : juce::String ("SPEAKERS"));
    t.number = juce::String (port + 1);
    t.label = signalLabel (feeder);
    t.labelColour = feeder.strip == nullptr || feeder.strip->isOutput() || feeder.strip->params.stereo.load() ? stereoLabel : monoLabel;
    t.icon = feeder.strip != nullptr ? iconOf (*feeder.strip) : iconOf (feeder.channel == 0 ? Icon::Headphones : Icon::Speakers);
    t.selected = selected;
    paintTile (g, area, t);
}

void RoutingPage::paintSignal (juce::Graphics& g, juce::Rectangle<int> area, const Signal& sig, bool marked) const
{
    TileSpec t;
    t.strip = sig.strip != nullptr ? stripDisplayColour (*sig.strip) : Colours::lime;
    t.name = sig.strip != nullptr ? (hasCustomName (*sig.strip) ? stripName (*sig.strip).toUpperCase() : juce::String())
                                  : (sig.channel == 0 ? juce::String ("PHONES") : juce::String ("SPEAKERS"));
    t.number = sig.strip != nullptr ? shortStripLabel (*sig.strip) : juce::String();
    t.label = signalLabel (sig);
    t.labelColour = sig.strip == nullptr || sig.strip->isOutput() || sig.strip->params.stereo.load() ? stereoLabel : monoLabel;
    t.icon = sig.strip != nullptr ? iconOf (*sig.strip) : iconOf (sig.channel == 0 ? Icon::Headphones : Icon::Speakers);
    t.selected = marked;
    paintTile (g, area, t);
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
    g.drawText (sourceName (selectedSource), left.removeFromTop (20), juce::Justification::centredLeft);
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

    // Each side: a header box, a black gap, then the tile box.
    g.setColour (Dark::header);
    g.fillRect (leftHeader);
    if (mode != Mode::Sources)
        g.fillRect (rightHeader);
    g.setColour (Dark::body);
    for (auto panel : { leftPanel, rightPanel })
        if (mode != Mode::Sources || panel == leftPanel)
            g.fillRect (panel.withTrimmedTop (leftHeader.getHeight() + 6));

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
        // The channel's MAIN / ALT input: which one the source grid patches.
        // ST pairs the input with the next one.
        drawField (g, inputField, "CHANNEL INPUT", editAlt ? "ALT" : "MAIN", editAlt ? altOrange : Dark::text);
        drawKey (g, stereoKey, "ST", sel != nullptr && sel->params.stereo.load(), Dark::choice);
        drawKey (g, prevPage, "<", false, Dark::choice);
        drawKey (g, nextPage, ">", false, Dark::choice);
    }
    if (mode == Mode::Sources)
    {
        drawSelector (g, leftSelector, Icon::Xlr, "SOURCE GROUP",
                      sourceGroupName (inputGroup));
        drawIconKey (g, renameKey, Icon::Edit, "RENAME", false);
    }
    else
    {
        if (mode == Mode::Outputs)
            drawSelector (g, leftSelector, Icon::Speaker, "OUTPUT GROUP",
                          "AUDIO OUT " + juce::String (outputGroup * kPortsPerGroup + 1) + "-" + juce::String (outputGroup * kPortsPerGroup + kPortsPerGroup));
        drawKey (g, autoKey, "+1\nAUTO", autoAdvance, autoYellow);
        // Locked: the closed lock lit red; unlocked: the open lock lit green
        // (as on the console); the other one black.
        g.setColour (locked ? lockRed : juce::Colours::black);
        g.fillRect (lockKey);
        drawIcon (g, Icon::LockClosed, lockKey.toFloat().reduced ((float) lockKey.getHeight() * 0.2f), locked ? juce::Colours::black : Dark::text);
        g.setColour (! locked ? Dark::post : juce::Colours::black);
        g.fillRect (unlockKey);
        drawIcon (g, Icon::LockOpen, unlockKey.toFloat().reduced ((float) unlockKey.getHeight() * 0.2f), ! locked ? juce::Colours::black : Dark::text);
    }

    // Right header.
    if (mode == Mode::Channels)
    {
        drawSelector (g, rightSelector, Icon::Xlr, "SOURCE GROUP",
                      sourceGroupName (inputGroup));
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
    if (! separator.isEmpty())
    {
        g.setColour (Dark::textDim);
        g.fillRect (separator);
    }

    if (mode == Mode::Channels)
    {
        for (auto& [area, strip] : channelTiles)
            paintChannelTile (g, area, *strip, strip == sel);
        for (size_t i = 0; i < rightPorts.size(); ++i)
        {
            const int port = sourceAt (inputGroup, (int) i);
            bool mine = false;
            if (sel != nullptr)
            {
                const int l = editAlt ? sel->getAltInputLeft() : sel->getInputLeft(), r = editAlt ? sel->getAltInputRight() : sel->getInputRight();
                mine = port >= 0 && (l == port || (sel->params.stereo.load() && (r == port || (r < 0 && l >= 0 && l + 1 == port))));
            }
            paintInputPort (g, rightPorts[i], port, mine);
        }
    }
    else if (mode == Mode::Sources)
    {
        for (size_t i = 0; i < leftPorts.size(); ++i)
            paintInputPort (g, leftPorts[i], sourceAt (inputGroup, (int) i), sourceAt (inputGroup, (int) i) == selectedSource);
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
    if (editAlt)
    {
        // The ALT input (WING INPUT SELECT), e.g. a recorder playback channel.
        if (right)
            s->setAltInput (s->getAltInputLeft(), port);
        else
            s->setAltInput (port);
    }
    else if (right)
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

juce::Component& RoutingPage::anchorAt (juce::Rectangle<int> area)
{
    anchor.setBounds (area);
    return anchor;
}

void RoutingPage::showGroupMenu (bool rightSide)
{
    juce::StringArray items;
    int current = 0, selected = 0;
    if (mode == Mode::Outputs && rightSide)
    {
        for (auto* name : signalGroups)
            items.add (name);
        current = 1;
        selected = signalGroup;
    }
    else
    {
        const bool outputs = mode == Mode::Outputs;
        if (outputs)
            for (int i = 0; i < numPortGroups (true); ++i)
            {
                const int a = i * kPortsPerGroup + 1;
                items.add ("AUDIO OUT " + juce::String (a) + "-" + juce::String (a + kPortsPerGroup - 1));
            }
        else
            for (int i = 0; i < numSourceGroups(); ++i)
                items.add (sourceGroupName (i));
        current = outputs ? 2 : 0;
        selected = outputs ? outputGroup : inputGroup;
    }
    juce::Component::SafePointer<RoutingPage> safe (this);
    popups::chooseFrom (anchorAt (rightSide ? rightSelector : leftSelector), items, selected, [safe, current] (int r)
    {
        if (safe == nullptr)
            return;
        if (current == 1)
        {
            safe->signalGroup = r;
            safe->signalPage = 0;
        }
        else if (current == 2)
            safe->outputGroup = r;
        else
            safe->inputGroup = r;
        safe->layoutTiles();
        safe->repaint();
        safe->notifyStatusKeysChanged();
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
    else if (inputField.contains (pos))
    {
        // CHANNEL INPUT: which input of the channel the source grid patches.
        juce::Component::SafePointer<RoutingPage> safe (this);
        popups::chooseFrom (anchorAt (inputField), { "MAIN", "ALT" }, editAlt ? 1 : 0, [safe] (int i)
        {
            if (safe != nullptr)
            {
                safe->editAlt = i == 1;
                safe->repaint();
            }
        });
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
            if (editAlt)
            {
                s->setAltInput (-1);
            }
            else
            {
                s->params.inputLeft = -1;
                s->params.inputRight = -1;
            }
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
                if (const int source = sourceAt (inputGroup, (int) i); source >= 0)
                    assignInput (source, e.mods.isPopupMenu() || e.mods.isShiftDown());
                return;
            }
        for (size_t i = 0; i < leftPorts.size(); ++i)
            if (leftPorts[i].contains (pos))
            {
                if (mode == Mode::Sources)
                    selectedSource = juce::jmax (0, sourceAt (inputGroup, (int) i));
                else
                    selectedPort = outputGroup * kPortsPerGroup + (int) i;
                repaint();
                notifyStatusKeysChanged(); // the title tile shows the selection
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
