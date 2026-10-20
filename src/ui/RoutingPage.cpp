#include "dsp/DspUtils.h"
#include "ui/Pages.h"

namespace livedaw::ui
{

namespace
{
    constexpr int kTilesPerRow = 8, kTargetsPerPage = 48, kPortsPerGroup = 48;

    // ROUTING's own colours [3.1]: port labels blue for mono and olive for
    // stereo, ALT orange, +1 AUTO yellow, the locked padlock red.
    const juce::Colour monoLabel = Colours::valueBlue, stereoLabel = Colours::lime, altOrange = Colours::alt, autoYellow = KnobColours::yellow,
        lockRed = Colours::tapRed,
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
        float rightColumn = 0.0f; // share of the body kept free at the right (source chips and meter)
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
            g.setFont (wingFontFit (TypeScale::body, bar.getHeight() * 0.95f));
            g.drawFittedText (t.name, bar.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.7f);
        }
        g.setColour (Colours::screenTile);
        g.fillRect (r);

        // [D14] the port label in small print in the bottom quarter.
        auto label = r.removeFromBottom (juce::jmin (screenCap (20.0f), r.getHeight() * 0.3f));
        g.setColour (t.labelColour);
        g.setFont (wingFontFit (TypeScale::body * 0.85f, label.getHeight() * 0.9f));
        g.drawFittedText (t.label, label.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.7f);

        auto body = r.reduced (3.0f, 2.0f);
        body.removeFromRight (body.getWidth() * t.rightColumn);
        auto numberArea = body.removeFromRight (body.getWidth() * 0.42f);
        if (t.icon)
            t.icon (g, body.withSizeKeepingCentre (juce::jmin (body.getWidth(), body.getHeight()), body.getHeight()).reduced (1.0f));
        g.setColour (Dark::text);
        g.setFont (wingFontFit (TypeScale::value * 0.8f, juce::jmin (numberArea.getHeight() * 0.7f, numberArea.getWidth() * 0.9f), WingWeight::SemiBold));
        g.drawFittedText (t.number, numberArea.toNearestInt(), juce::Justification::centredRight, 1, 0.6f);
        if (t.chip.isNotEmpty())
            drawChip (g, r.withHeight (screenCap (12.0f)).withTrimmedLeft (r.getWidth() - screenCap (24.0f)).translated (-2.0f, 2.0f), t.chip, t.chipColour, juce::Colours::black);

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
        // An unavailable key keeps its #303030 body with dim text, as on the console.
        g.setColour (on && enabled ? onColour : Dark::key);
        g.fillRect (r);
        g.setColour (on && enabled ? Dark::choiceInk : (enabled ? Dark::text : Dark::textDim));
        const bool twoLines = text.containsChar ('\n');
        g.setFont (wingFontFit (TypeScale::value * 0.85f, (float) r.getHeight() * (twoLines ? 0.42f : 0.5f), WingWeight::SemiBold));
        g.drawFittedText (text, r.reduced (juce::jlimit (1, 2, r.getWidth() / 20), 0), juce::Justification::centred, 2, 0.7f);
    }

    /** An outlined key [I8]: a grey frame on the panel, an optional icon at the
        left (INITIALIZE) or above the caption (48V, POL); filled when lit. */
    void drawOutlinedKey (juce::Graphics& g, juce::Rectangle<int> r, std::optional<Icon> icon, const juce::String& text, bool on, juce::Colour onColour,
                          bool iconAbove = false)
    {
        if (r.isEmpty())
            return;
        auto f = r.toFloat();
        if (on)
        {
            g.setColour (onColour);
            g.fillRect (f);
        }
        g.setColour (on ? onColour : Dark::keyOutline);
        g.drawRect (f, 1.5f);
        const auto ink = on ? juce::Colours::black : Dark::caption;
        if (iconAbove && icon.has_value())
        {
            auto label = f.removeFromBottom (f.getHeight() * 0.3f);
            drawIcon (g, *icon, f.reduced (f.getHeight() * 0.18f), ink);
            g.setColour (ink);
            g.setFont (wingFontPx (juce::jmin (TypeScale::body * getWingScreenHeight(), label.getHeight() * 0.6f)));
            g.drawFittedText (text, label.toNearestInt(), juce::Justification::centredTop, 1, 0.8f);
            return;
        }
        if (icon.has_value())
        {
            auto ib = f.removeFromLeft (f.getHeight()).reduced (f.getHeight() * 0.22f);
            drawIcon (g, *icon, ib, ink);
        }
        g.setColour (ink);
        g.setFont (wingFontPx (juce::jmin (TypeScale::body * getWingScreenHeight(), f.getHeight() * 0.36f)));
        g.drawFittedText (text, f.toNearestInt().reduced (4, 0), juce::Justification::centred, 1, 0.8f);
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
        g.setFont (wingFontFit (TypeScale::statusCaption * 1.2f, label.getHeight() * 0.85f));
        g.drawFittedText (text, label.toNearestInt(), juce::Justification::centredTop, 1);
    }

    /** A drop-down field: a small caption over a big value, and the dots key at the right. */
    void drawField (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& caption, const juce::String& value, juce::Colour valueColour)
    {
        if (r.isEmpty())
            return;
        g.setColour (Dark::key);
        g.fillRect (r);
        auto dots = r.removeFromRight (juce::jmin (r.getHeight() / 2, screenCap (24))).toFloat();
        g.setColour (Dark::text);
        const float d = juce::jlimit (3.0f, screenCap (6.0f), dots.getHeight() * 0.12f);
        for (int i = -1; i <= 1; ++i)
            g.fillEllipse (juce::Rectangle<float> (d, d).withCentre (dots.getCentre().translated (0.0f, (float) i * d * 2.0f)));
        auto t = r.reduced (juce::jlimit (3, screenCap (6), r.getWidth() / 24), 2);
        g.setColour (Dark::textDim);
        // Narrow fields (small windows) shrink the text rather than cut it.
        g.setFont (fontFittingWidth (wingFontFit (TypeScale::statusCaption * 1.2f, (float) t.getHeight() * 0.32f), caption, (float) t.getWidth(), 0.7f));
        g.drawFittedText (caption, t.removeFromTop (t.getHeight() * 40 / 100), juce::Justification::bottomLeft, 1, 0.7f);
        g.setColour (valueColour);
        g.setFont (fontFittingWidth (wingFontFit (TypeScale::value, (float) t.getHeight() * 0.85f, WingWeight::SemiBold), value, (float) t.getWidth(), 0.7f));
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
        g.setFont (fontFittingWidth (wingFontFit (TypeScale::statusCaption * 1.2f, (float) r.getHeight() * 0.3f), caption, (float) r.getWidth(), 0.7f));
        g.drawFittedText (caption, r.removeFromTop (r.getHeight() * 42 / 100), juce::Justification::bottomLeft, 1, 0.7f);
        g.setColour (Dark::text);
        g.setFont (fontFittingWidth (wingFontFit (TypeScale::value, (float) r.getHeight() * 0.8f, WingWeight::SemiBold), value, (float) r.getWidth(), 0.7f));
        g.drawFittedText (value, r, juce::Justification::centredLeft, 1);
    }
} // namespace

RoutingPage::RoutingPage (UiContext& c) : ctx (c)
{
    // Where the drop-down lists of the hand-drawn fields open.
    anchor.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (anchor);
    startTimerHz (10); // the source meters
}

StatusTitle RoutingPage::getStatusTitle()
{
    // SOURCES and OUTPUTS show the selected source or output (grey tile,
    // connector icon), CHANNELS the selected channel.
    StatusTitle t;
    if (mode == Mode::Sources)
    {
        t.kind = StatusTitle::Kind::Source;
        const bool usbPlayer = UsbRecorder::isPlayerSource (selectedSource);
        const bool playback = Recorder::isPlaybackSource (selectedSource) || usbPlayer;
        t.prefix = juce::String ((usbPlayer ? UsbRecorder::playerChannelOf (selectedSource)
                                  : playback ? Recorder::playbackChannelOf (selectedSource)
                                             : selectedSource) + 1);
        const auto custom = ctx.engine.getSourceName (MixerEngine::sourceIndex (selectedSource));
        t.text = custom.isNotEmpty() ? custom.toUpperCase() : juce::String (usbPlayer ? "USB PLAYER" : playback ? "PLAYBACK" : "AUDIO IN");
        t.icon = playback ? Icon::Waveform : Icon::Xlr;
    }
    else if (mode == Mode::Outputs)
    {
        t.kind = StatusTitle::Kind::Source;
        const bool rec = isRecorderPort (selectedPort);
        t.prefix = juce::String ((rec ? selectedPort - kRecorderPortBase : selectedPort) + 1);
        t.text = rec ? "RECORDER" : "AUDIO OUT";
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

namespace
{
    /** The MONITOR source group (fig. 32): PHONES L / R (0, 1), SPEAKERS L / R (2, 3). */
    std::atomic<int32_t>& monitorOutput (MixerEngine& e, int channel)
    {
        switch (channel)
        {
            case 0: return e.phones.outputLeft;
            case 1: return e.phones.outputRight;
            case 2: return e.monitorOutputLeft;
            default: return e.monitorOutputRight;
        }
    }
    constexpr int kMonitorSignals = 4;
    juce::String monitorSignalName (int channel) { return channel < 2 ? "PHONES" : "SPEAKERS"; }
    /** "1L" ... "2R": monitor 1 is the PHONES, 2 the SPEAKERS (the WING's /cfg/mon/1, 2). */
    juce::String monitorSignalNumber (int channel) { return juce::String (channel / 2 + 1) + (channel % 2 == 0 ? "L" : "R"); }
} // namespace

std::vector<RoutingPage::Signal> RoutingPage::signals() const
{
    std::vector<Signal> list;
    if (signalGroup == 5)
        return { { nullptr, 0 }, { nullptr, 1 }, { nullptr, 2 }, { nullptr, 3 } };
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
    notifyKnobsChanged(); // SOURCES: the source's GAIN
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

    // The source meters (the page is their only reader): the group's tiles and the selection.
    if (mode != Mode::Outputs)
    {
        sourcePeaks.resize ((size_t) kPortsPerGroup, 0.0f);
        for (int i = 0; i < kPortsPerGroup; ++i)
        {
            auto* src = ctx.engine.getSource (MixerEngine::sourceIndex (sourceAt (inputGroup, i)));
            const float peak = src != nullptr ? src->peak.exchange (0.0f) : 0.0f;
            auto& shown = sourcePeaks[(size_t) i];
            shown = juce::jmax (peak, shown * 0.5f);
            if (sourceAt (inputGroup, i) == selectedSource)
                selectedSourcePeak = shown;
        }
    }
    repaint();
}

void RoutingPage::resized()
{
    auto b = getLocalBounds();
    leftPanel = b.removeFromLeft (b.getWidth() * (mode == Mode::Sources ? 52 : 46) / 100);
    b.removeFromLeft (8);
    rightPanel = b;

    const int headerH = juce::jlimit (44, screenCap (64), getHeight() / 8);
    leftHeader = leftPanel.withHeight (headerH);
    rightHeader = rightPanel.withHeight (headerH);

    for (auto* r : { &inputField, &stereoKey, &autoKey, &lockKey, &unlockKey, &prevPage, &nextPage, &leftSelector, &rightSelector,
                     &noneKey, &copyCaption, &srcToChKey, &chToSrcKey, &renameKey, &srcSoloKey })
        *r = {};

    auto lh = leftHeader.reduced (6, 6);
    if (mode == Mode::Sources)
    {
        renameKey = lh.removeFromRight (lh.getHeight() + 10);
        lh.removeFromRight (8);
        // SRC SOLO (fig. 36), while SOURCE SOLO is on (SETUP > AUDIO).
        srcSoloKey = lh.removeFromRight (juce::jmin (screenCap (90), lh.getWidth() / 4)).reduced (0, 3);
        lh.removeFromRight (8);
        leftSelector = lh.withWidth (juce::jmin (lh.getWidth(), screenCap (300)));
    }
    else
    {
        unlockKey = lh.removeFromRight (lh.getHeight());
        lh.removeFromRight (2);
        lockKey = lh.removeFromRight (lh.getHeight());
        lh.removeFromRight (8);
        autoKey = lh.removeFromRight (juce::jmin (screenCap (64), juce::jmax (lh.getWidth() / 5, lh.getHeight() * 5 / 4)));
        lh.removeFromRight (juce::jlimit (4, screenCap (8), lh.getWidth() / 40));
        if (mode == Mode::Channels)
        {
            const bool paged = numTargetPages() > 1;
            nextPage = paged ? lh.removeFromRight (lh.getHeight()) : juce::Rectangle<int>();
            if (paged)
                lh.removeFromRight (2);
            prevPage = paged ? lh.removeFromRight (lh.getHeight()) : juce::Rectangle<int>();
            auto input = lh.withWidth (juce::jmin (lh.getWidth(), screenCap (210)));
            stereoKey = input.removeFromRight (input.getHeight()).reduced (2, 1);
            input.removeFromRight (4);
            inputField = input;
        }
        else
            leftSelector = lh.withWidth (juce::jmin (lh.getWidth(), screenCap (300)));
    }

    auto rh = rightHeader.reduced (6, 6);
    if (mode == Mode::Channels)
    {
        auto copy = rh.removeFromRight (juce::jmin (screenCap (170), rh.getWidth() / 3));
        copyCaption = copy.removeFromTop (copy.getHeight() * 38 / 100);
        srcToChKey = copy.removeFromLeft (copy.getWidth() / 2).reduced (1, 1);
        chToSrcKey = copy.reduced (1, 1);
        rh.removeFromRight (8);
    }
    if (mode != Mode::Sources)
    {
        noneKey = rh.removeFromRight (rh.getHeight() + 10);
        rh.removeFromRight (8);
        rightSelector = rh.withWidth (juce::jmin (rh.getWidth(), screenCap (300)));
    }
    layoutSourcePanel();
    layoutTiles();
}

void RoutingPage::layoutTiles()
{
    layoutVersion = ctx.engine.getStructureVersion();
    targetPage = juce::jlimit (0, numTargetPages() - 1, targetPage);
    inputGroup = juce::jlimit (0, numSourceGroups() - 1, inputGroup);
    outputGroup = juce::jlimit (0, numOutputGroups() - 1, outputGroup);
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
    // The hardware inputs, the playback channels, then the USB PLAYER.
    return numPortGroups (false) + (Recorder::kNumPlaybackChannels + kPortsPerGroup - 1) / kPortsPerGroup + 1;
}

juce::String RoutingPage::sourceGroupName (int group) const
{
    const int hw = numPortGroups (false);
    if (group < hw)
        return "AUDIO IN " + juce::String (group * kPortsPerGroup + 1) + "-" + juce::String (group * kPortsPerGroup + kPortsPerGroup);
    if (group == numSourceGroups() - 1)
        return "USB PLAYER";
    const int first = (group - hw) * kPortsPerGroup;
    return "PLAY " + juce::String (first + 1) + "-" + juce::String (juce::jmin (Recorder::kNumPlaybackChannels, first + kPortsPerGroup));
}

int RoutingPage::sourceAt (int group, int index) const
{
    const int hw = numPortGroups (false);
    if (group < hw)
        return group * kPortsPerGroup + index;
    if (group == numSourceGroups() - 1)
        return index < UsbRecorder::kNumChannels ? UsbRecorder::playerSource (index) : -1;
    const int channel = (group - hw) * kPortsPerGroup + index;
    return channel < Recorder::kNumPlaybackChannels ? Recorder::playbackSource (channel) : -1;
}

juce::String RoutingPage::sourceName (int source) const
{
    if (source < 0)
        return "NONE";
    // A source renamed on SOURCES shows its name everywhere in ROUTING.
    if (const int index = MixerEngine::sourceIndex (source); index >= 0)
        if (auto name = ctx.engine.getSourceName (index); name.isNotEmpty())
            return name;
    if (Recorder::isPlaybackSource (source))
        return Recorder::playbackSourceName (Recorder::playbackChannelOf (source));
    if (UsbRecorder::isPlayerSource (source))
        return UsbRecorder::playerSourceName (UsbRecorder::playerChannelOf (source));
    return portName (source, false);
}

int RoutingPage::numOutputGroups() const
{
    return numPortGroups (true) + 1; // the hardware outputs, then the USB RECORDER
}

juce::String RoutingPage::outputGroupName (int group) const
{
    if (group >= numPortGroups (true))
        return "RECORDER";
    const int a = group * kPortsPerGroup + 1;
    return "AUDIO OUT " + juce::String (a) + "-" + juce::String (a + kPortsPerGroup - 1);
}

int RoutingPage::outputPortAt (int group, int index) const
{
    if (group >= numPortGroups (true))
        return index < UsbRecorder::kNumChannels ? kRecorderPortBase + index : -1;
    return group * kPortsPerGroup + index;
}

RoutingPage::Signal RoutingPage::feederOf (int port) const
{
    if (isRecorderPort (port))
    {
        // A RECORDER port taps a strip's output (the USB recorder's own patch).
        const auto tap = ctx.engine.getUsbRecorder().getTap (port - kRecorderPortBase);
        if (tap.kind != UsbRecorder::Tap::Kind::Strip)
            return { nullptr, -1 };
        auto& strips = ctx.engine.getStrips (tap.stripKind);
        for (size_t i = 0; i < strips.size(); ++i)
        {
            const int number = strips[i]->isBus() ? strips[i]->getId() + 1 : (int) i + 1;
            if (number == tap.stripNumber)
                return { strips[i].get(), tap.channel };
        }
        return { nullptr, -1 };
    }
    for (int k = 0; k < kNumStripKinds; ++k)
        for (auto& s : ctx.engine.getStrips (static_cast<StripKind> (k)))
        {
            if (s->params.outputLeft.load() == port)
                return { s.get(), 0 };
            if (s->params.outputRight.load() == port)
                return { s.get(), 1 };
        }
    for (int ch = 0; ch < kMonitorSignals; ++ch)
        if (monitorOutput (ctx.engine, ch).load() == port)
            return { nullptr, ch };
    return { nullptr, -1 };
}

juce::String RoutingPage::signalLabel (const Signal& sig) const
{
    if (sig.channel < 0)
        return {};
    if (sig.strip == nullptr) // PHONES L / R, SPKR L / R: separate outputs
        return juce::String (sig.channel < 2 ? "PHONES" : "SPKR") + (sig.channel % 2 == 0 ? " L" : " R");
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
        if (stereo && UsbRecorder::isPlayerSource (port))
            t.label = "PLAYER " + juce::String (UsbRecorder::playerChannelOf (port) + 1) + "/"
                      + juce::String (UsbRecorder::playerChannelOf (portRight >= 0 ? portRight : port + 1) + 1);
        else if (stereo && ! Recorder::isPlaybackSource (port))
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
    // The source's colour and icon (SOURCES > ICON/COLOR), its name, the 48
    // (phantom) and M (mute) chips, a mini meter, and a stereo / mid-side
    // pair's label in olive with the link glyph.
    const bool usbPlayer = UsbRecorder::isPlayerSource (port);
    const bool playback = Recorder::isPlaybackSource (port) || usbPlayer;
    const auto* src = sourceParams (port);
    const int colour = src != nullptr ? src->colour.load() : 1;
    const int icon = src != nullptr ? src->icon.load() : 0;
    const int pairing = src != nullptr ? src->mode.load() : (int) ld::kSourceMono;
    TileSpec t;
    t.strip = playback && colour == 1 ? playbackStrip : wingColour (colour);
    const int index = MixerEngine::sourceIndex (port);
    t.name = index >= 0 ? ctx.engine.getSourceName (index).toUpperCase() : juce::String();
    t.number = juce::String ((usbPlayer ? UsbRecorder::playerChannelOf (port) : playback ? Recorder::playbackChannelOf (port) : port) + 1);
    t.label = usbPlayer ? UsbRecorder::playerSourceName (UsbRecorder::playerChannelOf (port))
              : playback ? Recorder::playbackSourceName (Recorder::playbackChannelOf (port))
                         : portName (port, false);
    t.labelColour = pairing != ld::kSourceMono ? stereoLabel : (playback ? FileColours::wav : monoLabel);
    t.icon = [icon, playback] (juce::Graphics& gr, juce::Rectangle<float> r)
    {
        if (icon <= 0 || ! drawWingIcon (gr, icon, r, Dark::textDim))
            drawIcon (gr, playback ? Icon::Waveform : Icon::Xlr, r, Dark::textDim);
    };
    t.selected = marked;
    // [3.1, D14] the right edge of the body holds the 48 / M chips (grey when
    // off) beside a vertical mini meter, so the number moves left of them.
    // Small tiles (small windows) keep only the meter.
    const bool roomy = area.getHeight() >= 34;
    t.rightColumn = src != nullptr ? (roomy ? 0.36f : 0.1f) : 0.0f;
    paintTile (g, area, t);
    if (src == nullptr)
        return;

    auto body = area.toFloat();
    body.removeFromTop (juce::jmax (12.0f, body.getHeight() * 0.22f));
    body.removeFromBottom (juce::jmin (screenCap (20.0f), body.getHeight() * 0.3f));
    body = body.reduced (0.0f, 2.0f);
    if (pairing != ld::kSourceMono)
    {
        // The pair's link glyph (o-o; o|o for mid/side) at the top left.
        const float d = juce::jlimit (4.0f, screenCap (7.0f), body.getHeight() * 0.22f);
        auto link = juce::Rectangle<float> (d * 3.2f, d).withPosition (body.getX() + 3.0f, body.getY());
        g.setColour (stereoLabel);
        g.drawEllipse (link.withWidth (d), 1.2f);
        g.drawEllipse (link.withTrimmedLeft (link.getWidth() - d), 1.2f);
        if (pairing == ld::kSourceStereo)
            g.drawLine (link.getX() + d, link.getCentreY(), link.getRight() - d, link.getCentreY(), 1.2f);
        else
            g.drawLine (link.getCentreX(), link.getY(), link.getCentreX(), link.getBottom(), 1.2f);
    }

    // The mini meter (the tiles of the source group shown keep their peaks):
    // a thin vertical bar of segments at the right edge.
    const auto& grid = mode == Mode::Channels ? rightPorts : leftPorts;
    const int slot = (int) (std::find (grid.begin(), grid.end(), area) - grid.begin());
    const float peak = juce::isPositiveAndBelow (slot, (int) sourcePeaks.size()) ? sourcePeaks[(size_t) slot] : 0.0f;
    auto meter = body.removeFromRight (juce::jmax (3.0f, body.getWidth() * 0.07f)).translated (-2.0f, 0.0f);
    const float pos = juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (peak, -60.0f) + 60.0f) / 60.0f);
    const int segments = juce::jmax (4, (int) (meter.getHeight() / 2.5f));
    const float segH = meter.getHeight() / (float) segments;
    for (int i = 0; i < segments; ++i)
    {
        const bool lit = pos > (float) i / (float) segments;
        g.setColour (lit ? (i == segments - 1 ? Colours::meterRed : Colours::meterGreen) : Colours::screenWell);
        g.fillRect (meter.getX(), meter.getBottom() - (float) (i + 1) * segH + 0.5f, meter.getWidth(), juce::jmax (1.0f, segH - 1.0f));
    }

    if (! roomy)
        return;
    // 48 (phantom) over M (mute): grey with dark text, lit red when on.
    auto chips = body.removeFromRight (body.getWidth() * 0.26f).reduced (1.0f, 0.0f).translated (-3.0f, 0.0f);
    const float chipH = juce::jmin (screenCap (11.0f), chips.getHeight() * 0.42f);
    chips = chips.withSizeKeepingCentre (chips.getWidth(), chipH * 2.0f + 2.0f);
    drawChip (g, chips.removeFromTop (chipH), "48", src->phantom.load() ? Colours::mute : Colours::chipOff, juce::Colours::black);
    chips.removeFromTop (2.0f);
    drawChip (g, chips.removeFromTop (chipH), "M", src->mute.load() ? Colours::mute : Colours::chipOff, juce::Colours::black);
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
        g.setFont (wingFontFit (TypeScale::value * 0.85f, d * 0.48f, WingWeight::SemiBold));
        g.drawFittedText (juce::String ((isRecorderPort (port) ? port - kRecorderPortBase : port) + 1), ring.toNearestInt(), juce::Justification::centred, 1, 0.6f);
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
                                     : monitorSignalName (feeder.channel);
    t.number = juce::String ((isRecorderPort (port) ? port - kRecorderPortBase : port) + 1);
    t.label = signalLabel (feeder);
    t.labelColour = feeder.strip == nullptr || feeder.strip->isOutput() || feeder.strip->params.stereo.load() ? stereoLabel : monoLabel;
    t.icon = feeder.strip != nullptr ? iconOf (*feeder.strip) : iconOf (feeder.channel < 2 ? Icon::Headphones : Icon::Speaker);
    t.selected = selected;
    paintTile (g, area, t);
}

void RoutingPage::paintSignal (juce::Graphics& g, juce::Rectangle<int> area, const Signal& sig, bool marked) const
{
    TileSpec t;
    t.strip = sig.strip != nullptr ? stripDisplayColour (*sig.strip) : Colours::lime;
    t.name = sig.strip != nullptr ? (hasCustomName (*sig.strip) ? stripName (*sig.strip).toUpperCase() : juce::String())
                                  : monitorSignalName (sig.channel);
    t.number = sig.strip != nullptr ? shortStripLabel (*sig.strip) : monitorSignalNumber (sig.channel);
    t.label = signalLabel (sig);
    t.labelColour = sig.strip == nullptr || sig.strip->isOutput() || sig.strip->params.stereo.load() ? stereoLabel : monoLabel;
    t.icon = sig.strip != nullptr ? iconOf (*sig.strip) : iconOf (sig.channel < 2 ? Icon::Headphones : Icon::Speaker);
    t.selected = marked;
    paintTile (g, area, t);
}

ld::SourceParams* RoutingPage::sourceParams (int port) const
{
    // A stereo or mid/side pair runs on its first source's settings.
    const int index = MixerEngine::sourceIndex (port);
    auto* src = ctx.engine.getSource (index);
    if (src != nullptr && src->mode.load() != ld::kSourceMono)
        return ctx.engine.getSource (index & ~1);
    return src;
}

namespace
{
    /** Applies `set` to the source of `port` and, when paired, its partner. */
    void forSourcePair (MixerEngine& engine, int port, const std::function<void (ld::SourceParams&)>& set)
    {
        const int index = MixerEngine::sourceIndex (port);
        auto* src = engine.getSource (index);
        if (src == nullptr)
            return;
        const bool paired = src->mode.load() != ld::kSourceMono;
        for (int i : { paired ? (index & ~1) : index, paired ? (index | 1) : -1 })
            if (auto* s = engine.getSource (i))
                set (*s);
    }
} // namespace

void RoutingPage::layoutSourcePanel()
{
    // The SOURCES settings (fig. 31): the tab row, the connections banner, then
    // INITIALIZE, the source box and MONO / STEREO / MID/SIDE at the left, the
    // meter in the middle, +48V, POL, MUTE and the GAIN fader at the right.
    sourceColourCells.clear();
    sourceIconCells.clear();
    auto r = rightPanel;
    auto tabs = r.removeFromTop (juce::jlimit (30, screenCap (40), r.getHeight() / 11));
    const int tw = tabs.getWidth() / 3;
    for (auto& t : sourceTabKeys)
        t = tabs.removeFromLeft (tw).reduced (1, 0);
    r.removeFromTop (6);
    sourceBanner = r.removeFromTop (juce::jlimit (30, screenCap (46), r.getHeight() / 9)).reduced (6, 0);
    r.removeFromTop (8);
    sourceBody = r.reduced (6, 0);

    auto body = sourceBody;
    const int kh = juce::jlimit (16, screenCap (40), body.getHeight() / 9);
    auto left = body.removeFromLeft (body.getWidth() * 46 / 100);
    sourceInitKey = left.removeFromTop (kh).withWidth (juce::jmin (left.getWidth(), screenCap (170)));
    left.removeFromTop (10);
    // The box takes what INITIALIZE and the three mode keys leave (small screens).
    sourceBox = left.removeFromTop (juce::jmax (kh, juce::jmin (left.getHeight() - 3 * (kh + 4) - 6, kh * 3))).withWidth (juce::jmin (left.getWidth(), screenCap (170)));
    left.removeFromTop (6);
    for (auto& k : sourceModeKeys)
    {
        k = left.removeFromTop (kh).withWidth (juce::jmin (left.getWidth(), screenCap (170)));
        left.removeFromTop (4);
    }
    body.removeFromLeft (6);
    sourceMeter = body.removeFromLeft (juce::jlimit (36, screenCap (60), body.getWidth() / 4)).withTrimmedBottom (kh / 2);
    body.removeFromLeft (10);
    auto keys = body.removeFromLeft (juce::jmin (body.getWidth() / 2, screenCap (96)));
    // [I8] 48V and POL square icon keys at the top, MUTE at the bottom.
    for (auto* k : { &sourcePhantomKey, &sourcePolKey })
    {
        *k = keys.removeFromTop (juce::jmin (keys.getWidth() * 4 / 5, kh * 2)).withSizeKeepingCentre (juce::jmin (keys.getWidth(), kh * 2) * 4 / 5, juce::jmin (keys.getWidth() * 4 / 5, kh * 2));
        keys.removeFromTop (8);
    }
    keys.removeFromBottom (kh / 2);
    sourceMuteKey = keys.removeFromBottom (kh + 6);
    body.removeFromLeft (8);
    sourceGainFader = body.withTrimmedBottom (kh / 2);

    // TAGS: the 8 mute groups the source belongs to (2 x 4).
    sourceTagCells.clear();
    {
        auto tags = sourceBody.withTrimmedTop (juce::jlimit (24, screenCap (34), sourceBody.getHeight() / 9) + 6);
        tags = tags.withHeight (juce::jmin (tags.getHeight(), screenCap (150)));
        const int cw = tags.getWidth() / 4, chh = tags.getHeight() / 2;
        for (int i = 0; i < kNumMuteGroups; ++i)
            sourceTagCells.push_back ({ juce::Rectangle<int> (tags.getX() + (i % 4) * cw, tags.getY() + (i / 4) * chh, cw, chh).reduced (4), i });
    }

    // ICON/COLOR: the 18 colours (6 x 3) and a row of icons.
    auto look = sourceBody;
    auto colours = look.removeFromTop (look.getHeight() / 2);
    const int cw = colours.getWidth() / 6, ch = colours.getHeight() / 3;
    for (int i = 0; i < 18; ++i)
        sourceColourCells.push_back ({ juce::Rectangle<int> (colours.getX() + (i % 6) * cw, colours.getY() + (i / 6) * ch, cw, ch).reduced (4), i + 1 });
    look.removeFromTop (10);
    static constexpr int icons[] = { 0, 1, 2, 100, 102, 103, 107, 113, 114, 200, 202, 205, 300, 304, 308, 400, 402, 506, 602, 605 };
    const int iw = look.getWidth() / 10, ih = juce::jmin (look.getHeight() / 2, iw);
    for (int i = 0; i < (int) std::size (icons); ++i)
        sourceIconCells.push_back ({ juce::Rectangle<int> (look.getX() + (i % 10) * iw, look.getY() + (i / 10) * ih, iw, ih).reduced (3), icons[i] });
}

void RoutingPage::paintSourcePanel (juce::Graphics& g) const
{
    g.setColour (Dark::body);
    g.fillRect (rightPanel);
    const char* names[] = { "SETTINGS", "ICON/COLOR", "TAGS" };
    for (int i = 0; i < 3; ++i)
    {
        auto t = sourceTabKeys[(size_t) i];
        const bool on = i == sourceTab, enabled = true;
        g.setColour (on ? Dark::tabOn : Dark::tabOff);
        g.fillRect (t);
        if (on)
        {
            g.setColour (Colours::yellow);
            g.fillRect (t.withHeight (3));
        }
        g.setColour (on ? Colours::yellow : (enabled ? Dark::textDim : Dark::textDim.withAlpha (0.4f)));
        g.setFont (wingFontFit (TypeScale::value * 0.85f, (float) t.getHeight() * 0.5f, WingWeight::SemiBold));
        g.drawText (names[i], t, juce::Justification::centred);
    }

    // Where the source goes: "NO CONNECTIONS" or the channels patched to it.
    g.setColour (Dark::header);
    g.fillRect (sourceBanner);
    auto users = inputUsers (selectedSource);
    // A soloed source also goes to the SOURCE SOLO channel ("-> 39 SRC SOLO").
    if (auto* host = ctx.engine.getSourceSoloHost(); host != nullptr && ctx.engine.isSourceSoloed (selectedSource))
        users = (users.isEmpty() ? juce::String() : users + ", ") + host->getLabel() + " SRC SOLO";
    g.setColour (users.isEmpty() ? Dark::textDim : Dark::text);
    g.setFont (wingFontFit (TypeScale::value * 0.85f, (float) sourceBanner.getHeight() * 0.5f, WingWeight::SemiBold));
    g.drawFittedText (users.isEmpty() ? juce::String ("NO CONNECTIONS") : "-> " + users, sourceBanner.reduced (8, 0), juce::Justification::centred, 2);

    if (sourceTab == 1)
        paintSourceLook (g);
    else if (sourceTab == 2)
        paintSourceTags (g);
    else if (sourceTab == 0)
        paintSourceSettings (g, sourceBody);
}

void RoutingPage::paintSourceSettings (juce::Graphics& g, juce::Rectangle<int>) const
{
    const auto* src = sourceParams (selectedSource);
    if (src == nullptr)
    {
        g.setColour (Dark::textDim);
        g.setFont (wingFont (TypeScale::body));
        g.drawFittedText ("THIS PORT HAS NO SOURCE SETTINGS", sourceBody, juce::Justification::centredTop, 2);
        return;
    }

    drawOutlinedKey (g, sourceInitKey, Icon::Warning, "INITIALIZE", false, Dark::choice);

    // The source box: the colour strip with the name, the connector and number.
    {
        auto box = sourceBox;
        g.setColour (Dark::well);
        g.fillRect (box);
        auto strip = box.removeFromTop (juce::jmax (16, box.getHeight() / 4));
        g.setColour (wingColour (src->colour.load()));
        g.fillRect (strip);
        g.setColour (juce::Colours::black);
        g.setFont (wingFontFit (TypeScale::body, (float) strip.getHeight() * 0.8f));
        g.drawFittedText (sourceName (selectedSource).toUpperCase(), strip.reduced (4, 0), juce::Justification::centred, 1, 0.7f);
        const bool playback = Recorder::isPlaybackSource (selectedSource) || UsbRecorder::isPlayerSource (selectedSource);
        const auto ring = box.reduced (6).toFloat().withSizeKeepingCentre ((float) juce::jmin (box.getHeight() - 12, screenCap (64)), (float) juce::jmin (box.getHeight() - 12, screenCap (64)));
        g.setColour (Colours::yellow);
        g.drawEllipse (ring, 2.0f);
        if (src->icon.load() <= 0 || ! drawWingIcon (g, src->icon.load(), ring.reduced (ring.getWidth() * 0.22f), Dark::text))
            drawIcon (g, playback ? Icon::Waveform : Icon::Xlr, ring.reduced (ring.getWidth() * 0.22f), Dark::text);
        g.setColour (Dark::text);
        g.setFont (wingFont (TypeScale::body));
        const int shownNumber = UsbRecorder::isPlayerSource (selectedSource) ? UsbRecorder::playerChannelOf (selectedSource)
                                : playback                                   ? Recorder::playbackChannelOf (selectedSource)
                                                                             : selectedSource;
        g.drawText (juce::String (shownNumber + 1),
                    box.withTrimmedLeft (box.getWidth() * 3 / 4).reduced (2), juce::Justification::bottomRight);
    }

    // MONO / STEREO / MID-SIDE (a pair: ports 1-2, 3-4, ...).
    // [I8] in a black box, the chosen one lit blue.
    const char* modes[] = { "MONO", "STEREO", "MID/SIDE" };
    g.setColour (juce::Colours::black);
    g.fillRect (sourceModeKeys[0].getUnion (sourceModeKeys[2]).expanded (0, 2));
    for (int i = 0; i < 3; ++i)
    {
        const bool on = src->mode.load() == i;
        auto k = sourceModeKeys[(size_t) i];
        if (on)
        {
            g.setColour (Colours::valueBlue);
            g.fillRect (k);
        }
        g.setColour (on ? juce::Colours::black : Dark::text);
        g.setFont (wingFontPx (juce::jmin (TypeScale::body * getWingScreenHeight(), (float) k.getHeight() * 0.36f)));
        g.drawText (modes[i], k, juce::Justification::centred);
    }

    // The meter (after gain, mute and polarity).
    {
        auto m = sourceMeter.toFloat();
        g.setColour (Dark::well);
        g.fillRect (m);
        const float db = juce::Decibels::gainToDecibels (selectedSourcePeak, -60.0f);
        const float pos = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
        // [I8] a narrow bar at the right with the scale (CLIP .. -60) beside it.
        auto inner = m.reduced (3.0f, 8.0f);
        auto bar = inner.removeFromRight (juce::jmax (5.0f, inner.getWidth() * 0.32f));
        g.setColour (Dark::key);
        g.fillRect (bar);
        g.setColour (db > -0.5f ? Colours::meterRed : db > -12.0f ? Colours::meterYellow : Colours::meterGreen);
        g.fillRect (bar.withTop (bar.getBottom() - bar.getHeight() * pos));
        g.setColour (Dark::textDim);
        g.setFont (wingFontPx (juce::jmin (TypeScale::meterScale * getWingScreenHeight(), inner.getWidth() * 0.3f)));
        for (const auto& [label, at] : { std::pair { "CLIP", 0.0f }, { "-6", 6.0f }, { "-12", 12.0f }, { "-24", 24.0f }, { "-36", 36.0f }, { "-48", 48.0f }, { "-60", 60.0f } })
        {
            const float y = bar.getY() + bar.getHeight() * at / 60.0f;
            g.drawText (label, juce::Rectangle<float> (inner.getX(), y - 6.0f, inner.getWidth() - 2.0f, 12.0f), juce::Justification::centredRight, false);
        }
    }

    drawOutlinedKey (g, sourcePhantomKey, Icon::Power, "48V", src->phantom.load(), Colours::mute, true);
    drawOutlinedKey (g, sourcePolKey, Icon::Invert, "POL", src->invert.load(), Colours::yellow, true);
    drawOutlinedKey (g, sourceMuteKey, std::nullopt, "MUTE", src->mute.load(), Colours::mute);

    // GAIN: a fader with the value flag (-12 .. +60 dB).
    {
        auto f = sourceGainFader.toFloat();
        g.setColour (Dark::well);
        g.fillRect (f);
        const float gain = src->gainDb.load();
        const float pos = juce::jlimit (0.0f, 1.0f, (gain + 12.0f) / 72.0f);
        auto slot = f.withSizeKeepingCentre (juce::jmin (screenCap (10.0f), f.getWidth() * 0.25f), f.getHeight() - 24.0f);
        g.setColour (Dark::key);
        g.fillRect (slot);
        g.setColour (KnobColours::orange);
        g.fillRect (slot.withTop (slot.getBottom() - slot.getHeight() * pos));
        const float y = slot.getBottom() - slot.getHeight() * pos;
        auto flag = juce::Rectangle<float> (juce::jmin (f.getWidth() - 4.0f, screenCap (64.0f)), 18.0f).withCentre ({ f.getCentreX(), juce::jlimit (f.getY() + 9.0f, f.getBottom() - 9.0f, y) });
        g.setColour (juce::Colours::white);
        g.fillRect (flag);
        g.setColour (juce::Colours::black);
        g.setFont (wingFont (TypeScale::statusCaption));
        g.drawFittedText (juce::String (gain, 1) + " dB", flag.toNearestInt(), juce::Justification::centred, 1, 0.6f);
        g.setColour (Dark::textDim);
        g.drawText ("GAIN", f.withY (f.getBottom() + 2.0f).withHeight (14.0f), juce::Justification::centred);
    }
}

void RoutingPage::paintSourceLook (juce::Graphics& g) const
{
    const auto* src = sourceParams (selectedSource);
    if (src == nullptr)
        return;
    for (auto& [cell, colour] : sourceColourCells)
    {
        g.setColour (wingColour (colour));
        g.fillRect (cell);
        if (colour == src->colour.load())
        {
            g.setColour (Colours::yellow);
            g.drawRect (cell.expanded (3), 3);
        }
    }
    for (auto& [cell, icon] : sourceIconCells)
    {
        const bool on = icon == src->icon.load();
        g.setColour (on ? Dark::choice : Dark::key);
        g.fillRect (cell);
        const auto ink = on ? Dark::choiceInk : Dark::text;
        if (icon == 0)
        {
            g.setColour (ink);
            g.setFont (wingFont (TypeScale::statusCaption));
            g.drawText ("NONE", cell, juce::Justification::centred);
        }
        else
            drawWingIcon (g, icon, cell.toFloat().reduced ((float) cell.getHeight() * 0.15f), ink);
    }
}

void RoutingPage::paintSourceTags (juce::Graphics& g) const
{
    // The mute groups (manual p. 40): while one of them is engaged, the source is muted.
    const auto* src = sourceParams (selectedSource);
    if (src == nullptr)
        return;
    auto title = sourceBody.withHeight (juce::jlimit (24, screenCap (34), sourceBody.getHeight() / 9));
    g.setColour (Dark::header);
    g.fillRect (title);
    g.setColour (Dark::caption);
    g.setFont (panelTitleFont (title.getHeight()));
    g.drawText ("MUTE GROUPS", title, juce::Justification::centred);
    const auto mask = src->muteGroups.load();
    const auto engaged = ctx.engine.muteGroups.load();
    for (auto& [cell, i] : sourceTagCells)
    {
        const bool on = (mask >> i) & 1u;
        g.setColour (on ? Dark::muteGroup : Dark::key);
        g.fillRoundedRectangle (cell.toFloat(), 2.0f);
        if ((engaged >> i) & 1u)
        {
            g.setColour (Dark::muteGroup);
            g.drawRoundedRectangle (cell.toFloat().reduced (1.0f), 2.0f, 2.0f);
        }
        auto r = cell.reduced (4);
        g.setColour (on ? juce::Colours::black : Dark::caption);
        g.setFont (wingFontFit (TypeScale::statusCaption, (float) r.getHeight() * 0.3f));
        g.drawText ("MGRP " + juce::String (i + 1), r.removeFromTop (r.getHeight() / 3), juce::Justification::centred);
        g.setColour (on ? juce::Colours::black : Dark::text);
        g.setFont (wingFontFit (TypeScale::body, (float) r.getHeight() * 0.45f));
        g.drawFittedText (ctx.engine.getMuteGroupName (i).toUpperCase(), r, juce::Justification::centred, 1, 0.7f);
    }
    // The source's state.
    g.setColour (Dark::textDim);
    g.setFont (wingFont (TypeScale::statusCaption * 1.2f));
    const auto state = src->mute.load() ? "MUTED" : ((mask & engaged) != 0 ? "MUTED BY A MUTE GROUP" : "");
    g.drawText (state, sourceBody.withTrimmedTop (sourceTagCells.empty() ? 0 : sourceTagCells.back().first.getBottom() - sourceBody.getY() + 10).withHeight (20),
                juce::Justification::centred);
}

void RoutingPage::selectSource (int port)
{
    selectedSource = port;
    // A source solo follows the selection (firmware 1.12).
    if (ctx.engine.isSourceSoloActive())
        ctx.engine.setSourceSoloPort (port);
}

void RoutingPage::renameSource()
{
    const int index = MixerEngine::sourceIndex (selectedSource);
    if (index < 0)
        return;
    popups::TextOptions options;
    options.actionText = "RENAME";
    juce::Component::SafePointer<RoutingPage> safe (this);
    popups::editText (*this, "RENAME SOURCE", ctx.engine.getSourceName (index), [safe, index] (const juce::String& text)
    {
        if (safe == nullptr)
            return;
        safe->ctx.engine.setSourceName (index, text.trim());
        safe->repaint();
        safe->notifyStatusKeysChanged();
    }, options);
}

void RoutingPage::setSourceGainFromY (int y)
{
    auto slot = sourceGainFader.toFloat().withSizeKeepingCentre (10.0f, (float) sourceGainFader.getHeight() - 24.0f);
    const float pos = juce::jlimit (0.0f, 1.0f, (slot.getBottom() - (float) y) / juce::jmax (1.0f, slot.getHeight()));
    const float gain = std::round ((pos * 72.0f - 12.0f) * 2.0f) / 2.0f; // 0.5 dB steps
    forSourcePair (ctx.engine, selectedSource, [gain] (ld::SourceParams& s) { s.gainDb = gain; });
    repaint();
}

std::vector<KnobBinding> RoutingPage::getKnobs()
{
    if (mode != Mode::Sources)
        return {};
    auto* src = sourceParams (selectedSource);
    if (src == nullptr)
        return {};
    // A stereo / mid-side pair has one gain (the engine plays the first
    // source's): the knob sets both, as the fader and the wheel do.
    auto gain = bindLinear ("GAIN", src->gainDb, -12.0f, 60.0f, " dB", 1, 0.0f);
    gain.set = [&engine = ctx.engine, port = selectedSource] (float p)
    {
        const float g = -12.0f + juce::jlimit (0.0f, 1.0f, p) * 72.0f;
        forSourcePair (engine, port, [g] (ld::SourceParams& s) { s.gainDb = g; });
    };
    gain.steps = 145.0f; // 0.5 dB steps, as the fader
    gain.reset = [&engine = ctx.engine, port = selectedSource] { forSourcePair (engine, port, [] (ld::SourceParams& s) { s.gainDb = 0.0f; }); };
    return { gain };
}

void RoutingPage::mouseDown (const juce::MouseEvent& e)
{
    draggingGain = mode == Mode::Sources && sourceTab == 0 && sourceGainFader.contains (e.getPosition()) && sourceParams (selectedSource) != nullptr;
    if (draggingGain)
        setSourceGainFromY (e.y);
}

void RoutingPage::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingGain)
        setSourceGainFromY (e.y);
}

void RoutingPage::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (mode == Mode::Sources && sourceTab == 0 && sourceGainFader.contains (e.getPosition()))
        if (auto* src = sourceParams (selectedSource))
        {
            const float gain = juce::jlimit (-12.0f, 60.0f, src->gainDb.load() + (w.deltaY > 0.0f ? 0.5f : -0.5f));
            forSourcePair (ctx.engine, selectedSource, [gain] (ld::SourceParams& s) { s.gainDb = gain; });
            repaint();
        }
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
        g.setColour (Dark::caption);
        g.setFont (wingFontFit (TypeScale::statusCaption, (float) r.getHeight() * 0.8f));
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
        drawIconKey (g, renameKey, Icon::Edit, "RENAME", sourceParams (selectedSource) != nullptr);
        if (ctx.engine.monitor.sourceSolo.load() != ld::kSourceSoloOff)
            drawKey (g, srcSoloKey, "SRC SOLO", ctx.engine.isSourceSoloed (selectedSource), Colours::gold);
    }
    else
    {
        if (mode == Mode::Outputs)
            drawSelector (g, leftSelector, Icon::Speaker, "OUTPUT GROUP", outputGroupName (outputGroup));
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
        drawKey (g, srcToChKey, "SRC -> CH", false, Dark::choice, false);
        drawKey (g, chToSrcKey, "CH -> SRC", false, Dark::choice, false);
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
            if (const int port = outputPortAt (outputGroup, (int) i); port >= 0)
                paintOutputPort (g, leftPorts[i], port, port == selectedPort);
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
    else if (auto* src = sourceParams (port); src != nullptr && src->mode.load() != ld::kSourceMono)
    {
        // A stereo or mid/side source: the channel takes the pair, in stereo.
        const int first = MixerEngine::sourcePort (MixerEngine::sourceIndex (port) & ~1);
        p.inputLeft = first;
        p.inputRight = first + 1;
        p.stereo = true;
    }
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
    if (isRecorderPort (port))
    {
        ctx.engine.getUsbRecorder().setTap (port - kRecorderPortBase, {});
        ctx.engine.getUsbRecorder().saveSettings (ctx.settings);
        return;
    }
    for (int k = 0; k < kNumStripKinds; ++k)
        for (auto& s : ctx.engine.getStrips (static_cast<StripKind> (k)))
        {
            if (s->params.outputLeft.load() == port)
                s->params.outputLeft = -1;
            if (s->params.outputRight.load() == port)
                s->params.outputRight = -1;
        }
    for (int ch = 0; ch < kMonitorSignals; ++ch)
        if (monitorOutput (ctx.engine, ch).load() == port)
            monitorOutput (ctx.engine, ch) = -1;
}

void RoutingPage::assignOutput (int port, const Signal& sig)
{
    if (locked)
        return;
    // One signal per output. A strip has one output pair, so patching it to a
    // new output moves it there (engine item E4: many outputs per signal).
    // The RECORDER ports tap the signal instead, leaving its outputs alone.
    if (isRecorderPort (port))
    {
        UsbRecorder::Tap tap;
        if (sig.strip != nullptr)
        {
            tap.kind = UsbRecorder::Tap::Kind::Strip;
            tap.stripKind = sig.strip->getKind();
            auto& strips = ctx.engine.getStrips (tap.stripKind);
            for (size_t i = 0; i < strips.size(); ++i)
                if (strips[i].get() == sig.strip)
                    tap.stripNumber = sig.strip->isBus() ? sig.strip->getId() + 1 : (int) i + 1;
            tap.channel = sig.channel;
        }
        ctx.engine.getUsbRecorder().setTap (port - kRecorderPortBase, tap);
        ctx.engine.getUsbRecorder().saveSettings (ctx.settings);
    }
    else
    {
        clearOutput (port);
        if (sig.strip == nullptr)
            monitorOutput (ctx.engine, sig.channel) = port;
        else
            (sig.channel == 0 ? sig.strip->params.outputLeft : sig.strip->params.outputRight) = port;
    }
    if (autoAdvance)
        selectedPort = isRecorderPort (selectedPort) ? juce::jmin (kRecorderPortBase + UsbRecorder::kNumChannels - 1, selectedPort + 1)
                                                     : juce::jmin (numPorts (true) - 1, selectedPort + 1);
    if (const int group = isRecorderPort (selectedPort) ? numOutputGroups() - 1 : selectedPort / kPortsPerGroup; group != outputGroup)
    {
        outputGroup = group;
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
            for (int i = 0; i < numOutputGroups(); ++i)
                items.add (outputGroupName (i));
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
    if (draggingGain)
    {
        draggingGain = false;
        return;
    }
    if (mode == Mode::Sources && rightPanel.contains (pos))
    {
        // The SOURCES settings panel.
        for (int i = 0; i < 3; ++i)
            if (sourceTabKeys[(size_t) i].contains (pos))
            {
                sourceTab = i;
                repaint();
                return;
            }
        const int index = MixerEngine::sourceIndex (selectedSource);
        auto* src = sourceParams (selectedSource);
        if (src == nullptr)
            return;
        // A stereo or mid/side pair shares its settings: set both sources.
        auto flip = [this, src] (std::atomic<bool> ld::SourceParams::*field)
        {
            const bool on = ! (src->*field).load();
            forSourcePair (ctx.engine, selectedSource, [field, on] (ld::SourceParams& s) { (s.*field) = on; });
        };
        if (sourceTab == 0)
        {
            if (sourceInitKey.contains (pos))
                ctx.engine.initializeSource (index);
            else if (sourcePhantomKey.contains (pos))
                flip (&ld::SourceParams::phantom);
            else if (sourcePolKey.contains (pos))
                flip (&ld::SourceParams::invert);
            else if (sourceMuteKey.contains (pos))
                flip (&ld::SourceParams::mute);
            for (int m = 0; m < 3; ++m)
                if (sourceModeKeys[(size_t) m].contains (pos))
                    ctx.engine.setSourceMode (index, m);
        }
        else if (sourceTab == 2)
        {
            for (auto& [cell, group] : sourceTagCells)
                if (cell.contains (pos))
                {
                    const auto bit = 1u << group;
                    const auto mask = src->muteGroups.load() ^ bit;
                    forSourcePair (ctx.engine, selectedSource, [mask] (ld::SourceParams& s) { s.muteGroups = mask; });
                }
        }
        else
        {
            for (auto& [cell, colour] : sourceColourCells)
                if (cell.contains (pos))
                    forSourcePair (ctx.engine, selectedSource, [c = colour] (ld::SourceParams& s) { s.colour = c; });
            for (auto& [cell, icon] : sourceIconCells)
                if (cell.contains (pos))
                    forSourcePair (ctx.engine, selectedSource, [i = icon] (ld::SourceParams& s) { s.icon = i; });
        }
        repaint();
        notifyStatusKeysChanged();
        return;
    }
    if (renameKey.contains (pos))
        renameSource();
    else if (srcSoloKey.contains (pos) && mode == Mode::Sources && ctx.engine.monitor.sourceSolo.load() != ld::kSourceSoloOff)
        ctx.engine.setSourceSolo (selectedSource, ! ctx.engine.isSourceSoloed (selectedSource));
    else if (autoKey.contains (pos))
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
                    selectSource (juce::jmax (0, sourceAt (inputGroup, (int) i)));
                else if (const int port = outputPortAt (outputGroup, (int) i); port >= 0)
                    selectedPort = port;
                repaint();
                notifyStatusKeysChanged(); // the title tile shows the selection
                notifyKnobsChanged();      // SOURCES: its GAIN
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
