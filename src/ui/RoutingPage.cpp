#include "ui/Pages.h"

namespace livedaw::ui
{

namespace
{
    constexpr int kTilesPerRow = 8, kTargetsPerPage = 48, kPortsPerGroup = 48;

    const juce::Colour navy (0xff27344d), tileOff (0xff2b2b2b), tileOn (0xff5c5c5c), lime (0xffc8d72b);

    juce::String shortLabel (const Strip& s)
    {
        const auto n = juce::String (s.getNumber());
        switch (s.getKind())
        {
            case StripKind::Channel: return n;
            case StripKind::Aux:     return "A" + n;
            case StripKind::Bus:     return "B" + n;
            case StripKind::Main:    return "M" + n;
            case StripKind::Matrix:  return "MX" + n;
        }
        return n;
    }

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

    /** MONITOR mode: outputs, PFL / AFL and level, in the console's panels. */
    class MonitorPanel final : public juce::Component
    {
    public:
        explicit MonitorPanel (UiContext& c) : ctx (c)
        {
            const auto outs = numbered (ctx.engine.getHardwareOutputNames(), juce::jmax (64, ctx.engine.getHardwareOutputNames().size()));
            left.setItems (outs, "OFF");
            right.setItems (outs, "OFF");
            left.bind (&ctx.engine.monitorOutputLeft);
            right.bind (&ctx.engine.monitorOutputRight);
            pfl.onClick = [this] { ctx.engine.soloAfl = false; };
            afl.onClick = [this] { ctx.engine.soloAfl = true; };
            for (auto* b : { &pfl, &afl })
                b->setColour (juce::TextButton::buttonOnColourId, Colours::yellow);
            level.setBinding (bindLinear ("LEVEL", ctx.engine.monitorLevelDb, -60.0f, 10.0f, " dB", 1, 0.0f));
            for (auto* c2 : { (juce::Component*) &left, (juce::Component*) &right, (juce::Component*) &pfl, (juce::Component*) &afl,
                              (juce::Component*) &level })
                addAndMakeVisible (c2);
        }

        void paint (juce::Graphics& g) override
        {
            pfl.setToggleState (! ctx.engine.soloAfl.load(), juce::dontSendNotification);
            afl.setToggleState (ctx.engine.soloAfl.load(), juce::dontSendNotification);
            drawSectionBox (g, outputBox, "MONITOR OUTPUT");
            drawSectionBox (g, soloBox, "SOLO MODE");
            drawSectionBox (g, levelBox, "MONITOR LEVEL");
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
            g.drawText ("LEFT", leftLabel, juce::Justification::centredLeft);
            g.drawText ("RIGHT", rightLabel, juce::Justification::centredLeft);
            g.drawFittedText ("Without a solo, the monitor follows MAIN 1.", noteArea, juce::Justification::topLeft, 2);
        }

        void resized() override
        {
            auto b = getLocalBounds();
            auto top = b.removeFromTop (b.getHeight() / 2);
            outputBox = top.removeFromLeft (top.getWidth() * 55 / 100).reduced (0, 0).withTrimmedRight (8);
            soloBox = top;
            levelBox = b.withTrimmedTop (8).withWidth (juce::jmin (b.getWidth(), 280));

            auto o = sectionContent (outputBox, true);
            leftLabel = o.removeFromTop (20);
            left.setBounds (o.removeFromTop (34));
            o.removeFromTop (8);
            rightLabel = o.removeFromTop (20);
            right.setBounds (o.removeFromTop (34));
            o.removeFromTop (8);
            noteArea = o;

            auto s = sectionContent (soloBox, true).withSizeKeepingCentre (juce::jmin (260, soloBox.getWidth() - 20), 48);
            pfl.setBounds (s.removeFromLeft (s.getWidth() / 2).reduced (2, 0));
            afl.setBounds (s.reduced (2, 0));
            level.setBounds (sectionContent (levelBox, true));
        }

    private:
        UiContext& ctx;
        IntCombo left, right;
        juce::TextButton pfl { "PFL" }, afl { "AFL" };
        DisplayKnob level;
        juce::Rectangle<int> outputBox, soloBox, levelBox, leftLabel, rightLabel, noteArea;
    };
} // namespace

RoutingPage::RoutingPage (UiContext& c) : ctx (c)
{
    monitorPanel = std::make_unique<MonitorPanel> (ctx);
    addChildComponent (*monitorPanel);
    startTimerHz (5);
}

RoutingPage::~RoutingPage() = default;

std::vector<Strip*> RoutingPage::allTargets() const
{
    std::vector<Strip*> list;
    auto add = [this, &list] (StripKind k)
    {
        for (auto& s : ctx.engine.getStrips (k))
            list.push_back (s.get());
    };
    if (mode == Mode::Outputs)
        for (auto k : { StripKind::Main, StripKind::Matrix, StripKind::Bus, StripKind::Channel, StripKind::Aux })
            add (k);
    else
        for (auto k : { StripKind::Channel, StripKind::Aux })
            add (k);
    return list;
}

int RoutingPage::numTargetPages() const
{
    return juce::jmax (1, ((int) allTargets().size() + kTargetsPerPage - 1) / kTargetsPerPage);
}

int RoutingPage::numPorts() const
{
    const auto& names = mode == Mode::Outputs ? ctx.engine.getHardwareOutputNames() : ctx.engine.getHardwareInputNames();
    return juce::jmax (kPortsPerGroup, (names.size() + kPortsPerGroup - 1) / kPortsPerGroup * kPortsPerGroup);
}

int RoutingPage::numPortGroups() const
{
    return numPorts() / kPortsPerGroup;
}

juce::String RoutingPage::portName (int index) const
{
    const auto& names = mode == Mode::Outputs ? ctx.engine.getHardwareOutputNames() : ctx.engine.getHardwareInputNames();
    if (index < names.size() && names[index].isNotEmpty())
        return names[index].toUpperCase();
    return (mode == Mode::Outputs ? "OUT " : "IN ") + juce::String (index + 1);
}

Strip* RoutingPage::selectedStrip() const
{
    auto& strips = ctx.engine.getStrips (selectedKind);
    return juce::isPositiveAndBelow (selectedIndex, (int) strips.size()) ? strips[(size_t) selectedIndex].get() : nullptr;
}

void RoutingPage::select (Strip* s)
{
    if (s == nullptr)
        return;
    selectedKind = s->getKind();
    auto& strips = ctx.engine.getStrips (selectedKind);
    for (size_t i = 0; i < strips.size(); ++i)
        if (strips[i].get() == s)
            selectedIndex = (int) i;

    // Keep the selection on screen.
    const auto list = allTargets();
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
    targetPage = portGroup = 0;
    if (auto* s = selectedStrip(); s == nullptr || (m == Mode::Channels && ! s->isInput()))
    {
        selectedKind = StripKind::Channel;
        selectedIndex = 0;
    }
    monitorPanel->setVisible (m == Mode::Monitor);
    resized();
    repaint();
}

void RoutingPage::refresh()
{
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
    auto top = b.removeFromTop (juce::jmin (44, b.getHeight() / 10));
    auto keys = top.removeFromRight (juce::jmin (420, top.getWidth() / 2));
    for (int i = 2; i >= 0; --i)
        modeKeys[i] = keys.removeFromRight (keys.getWidth() / (i + 1)).reduced (3, 2);
    b.removeFromTop (6);
    monitorPanel->setBounds (b);

    leftPanel = b.removeFromLeft (b.getWidth() * 44 / 100);
    b.removeFromLeft (8);
    rightPanel = b;

    const int headerH = juce::jmin (64, leftPanel.getHeight() / 7);
    leftHeader = leftPanel.withHeight (headerH);
    rightHeader = rightPanel.withHeight (headerH);

    // Left header: MONO/ST (channels), +1 AUTO, locks, pages.
    auto lh = leftHeader.reduced (8, 7);
    unlockKey = lh.removeFromRight (lh.getHeight());
    lh.removeFromRight (2);
    lockKey = lh.removeFromRight (lh.getHeight());
    lh.removeFromRight (8);
    autoKey = lh.removeFromRight (juce::jmin (72, lh.getWidth() / 4));
    lh.removeFromRight (8);
    const bool paged = numTargetPages() > 1;
    nextPage = paged ? lh.removeFromRight (lh.getHeight()) : juce::Rectangle<int>();
    if (paged)
        lh.removeFromRight (2);
    prevPage = paged ? lh.removeFromRight (lh.getHeight()) : juce::Rectangle<int>();
    lh.removeFromRight (6);
    auto modeArea = lh.withTrimmedTop (16).withWidth (juce::jmin (lh.getWidth(), 170));
    monoKey = mode == Mode::Channels ? modeArea.removeFromLeft (modeArea.getWidth() / 2) : juce::Rectangle<int>();
    stereoKey = mode == Mode::Channels ? modeArea : juce::Rectangle<int>();

    // Right header: group selector and OFF.
    auto rh = rightHeader.reduced (8, 7);
    offKey = rh.removeFromRight (juce::jmin (90, rh.getWidth() / 4));
    rh.removeFromRight (8);
    groupSelector = rh.withWidth (juce::jmin (rh.getWidth(), 300));

    layoutTiles();
}

void RoutingPage::layoutTiles()
{
    layoutVersion = ctx.engine.getStructureVersion();
    targetPage = juce::jlimit (0, numTargetPages() - 1, targetPage);
    portGroup = juce::jlimit (0, numPortGroups() - 1, portGroup);
    targetTiles.clear();
    portTiles.clear();
    separator = {};
    if (mode == Mode::Monitor || leftPanel.isEmpty())
        return;

    // Left grid: up to 48 targets, with a gap before the first aux (channels mode).
    const auto list = allTargets();
    const int first = targetPage * kTargetsPerPage;
    const int count = juce::jmin (kTargetsPerPage, (int) list.size() - first);
    auto grid = leftPanel.withTrimmedTop (leftHeader.getHeight() + 6).reduced (6);
    const int sepH = 14;
    const int rowsNeeded = (count + kTilesPerRow - 1) / kTilesPerRow;
    const int rows = juce::jmax (6, rowsNeeded);
    const int tileW = grid.getWidth() / kTilesPerRow;
    const int tileH = (grid.getHeight() - sepH) / rows;
    int row = 0, col = 0;
    bool separated = false;
    for (int i = 0; i < count; ++i)
    {
        auto* s = list[(size_t) (first + i)];
        if (! separated && mode == Mode::Channels && s->getKind() == StripKind::Aux && i > 0)
        {
            if (col != 0)
            {
                col = 0;
                ++row;
            }
            separator = juce::Rectangle<int> (grid.getX() + 8, grid.getY() + row * tileH + sepH / 2 - 1, grid.getWidth() - 16, 3);
            separated = true;
        }
        const int y = grid.getY() + row * tileH + (separated ? sepH : 0);
        targetTiles.push_back ({ juce::Rectangle<int> (grid.getX() + col * tileW, y, tileW, tileH).reduced (3), s });
        if (++col == kTilesPerRow)
        {
            col = 0;
            ++row;
        }
    }

    // Right grid: one group of 48 ports.
    auto ports = rightPanel.withTrimmedTop (rightHeader.getHeight() + 6).reduced (6);
    const int pw = ports.getWidth() / kTilesPerRow, ph = ports.getHeight() / (kPortsPerGroup / kTilesPerRow);
    for (int i = 0; i < kPortsPerGroup; ++i)
        portTiles.push_back (juce::Rectangle<int> (ports.getX() + (i % kTilesPerRow) * pw, ports.getY() + (i / kTilesPerRow) * ph, pw, ph).reduced (3));
}

juce::String RoutingPage::portUsers (int port, bool& usedBySelected) const
{
    usedBySelected = false;
    auto* sel = selectedStrip();
    juce::String users;
    auto note = [&] (Strip* s)
    {
        if (s == sel)
            usedBySelected = true;
        if (users.isEmpty())
            users = hasCustomName (*s) ? stripName (*s).toUpperCase() : s->getLabel();
    };

    if (mode == Mode::Outputs)
    {
        for (auto* s : allTargets())
            if (s->params.outputLeft.load() == port || s->params.outputRight.load() == port)
                note (s);
        if (users.isEmpty() && (ctx.engine.monitorOutputLeft.load() == port || ctx.engine.monitorOutputRight.load() == port))
            users = "MONITOR";
        return users;
    }

    for (auto* s : allTargets())
    {
        const int l = s->params.inputLeft.load(), r = s->params.inputRight.load();
        const bool stereo = s->params.stereo.load();
        if (l == port || (stereo && (r == port || (r < 0 && l >= 0 && l + 1 == port))))
            note (s);
    }
    return users;
}

void RoutingPage::paintTarget (juce::Graphics& g, const TargetTile& t, bool selected) const
{
    auto r = t.area.toFloat();
    auto& p = t.strip->params;
    const int port = mode == Mode::Outputs ? p.outputLeft.load() : p.inputLeft.load();
    const bool patched = port >= 0;

    auto bar = r.removeFromTop (juce::jmax (12.0f, r.getHeight() * 0.22f));
    g.setColour (patched || hasCustomName (*t.strip) ? t.strip->getColour() : navy);
    g.fillRect (bar);
    if (hasCustomName (*t.strip))
    {
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (juce::jmin (15.0f, bar.getHeight() * 0.95f), juce::Font::bold));
        g.drawFittedText (stripName (*t.strip).toUpperCase(), bar.toNearestInt().reduced (2, 0), juce::Justification::centred, 1);
    }
    g.setColour (patched ? tileOn : tileOff);
    g.fillRect (r);

    auto label = r.removeFromBottom (juce::jmin (20.0f, r.getHeight() * 0.36f));
    if (patched)
    {
        drawIcon (g, mode == Mode::Outputs ? Icon::Speaker : stripIcon (t.strip->getKind()), r.reduced (r.getWidth() * 0.25f, 2.0f), juce::Colours::white);
        g.setColour (lime);
        g.setFont (juce::FontOptions (juce::jmin (15.0f, label.getHeight() * 0.9f), juce::Font::bold));
        juce::String text = portName (port);
        if (mode == Mode::Channels && p.stereo.load())
            text = juce::String (port + 1) + "/" + juce::String ((p.inputRight.load() >= 0 ? p.inputRight.load() : port + 1) + 1);
        g.drawFittedText (text, label.toNearestInt().reduced (2, 0), juce::Justification::centred, 1);
    }
    else
    {
        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (juce::jmin (16.0f, label.getHeight() * 0.95f), juce::Font::bold));
        g.drawFittedText (shortLabel (*t.strip), label.toNearestInt(), juce::Justification::centred, 1);
    }

    if (selected)
    {
        g.setColour (Colours::yellow);
        g.drawRect (t.area.toFloat().expanded (2.0f), 3.0f);
    }
}

void RoutingPage::paintPort (juce::Graphics& g, juce::Rectangle<int> area, int port) const
{
    bool mine = false;
    const auto users = portUsers (port, mine);
    auto r = area.toFloat();
    auto bar = r.removeFromTop (juce::jmax (12.0f, r.getHeight() * 0.22f));
    g.setColour (users.isNotEmpty() ? Colours::amber : navy);
    g.fillRect (bar);
    if (users.isNotEmpty())
    {
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (juce::jmin (15.0f, bar.getHeight() * 0.95f), juce::Font::bold));
        g.drawFittedText (users, bar.toNearestInt().reduced (2, 0), juce::Justification::centred, 1);
    }
    g.setColour (users.isNotEmpty() ? tileOn : tileOff);
    g.fillRect (r);
    auto label = r.removeFromBottom (juce::jmin (20.0f, r.getHeight() * 0.38f));
    if (users.isNotEmpty())
        drawIcon (g, mode == Mode::Outputs ? Icon::Speaker : Icon::Xlr, r.reduced (r.getWidth() * 0.3f, 2.0f), juce::Colours::white);
    g.setColour (users.isNotEmpty() ? lime : Colours::cyan);
    g.setFont (juce::FontOptions (juce::jmin (15.0f, label.getHeight() * 0.9f), juce::Font::bold));
    g.drawFittedText (portName (port), label.toNearestInt().reduced (2, 0), juce::Justification::centred, 1);

    if (mine)
    {
        g.setColour (Colours::yellow);
        g.drawRect (area.toFloat().expanded (2.0f), 3.0f);
    }
}

void RoutingPage::paint (juce::Graphics& g)
{
    g.fillAll (Colours::screen);

    // Mode keys.
    const std::tuple<const char*, Icon> keys[] = { { "CHANNELS", Icon::Xlr }, { "OUTPUTS", Icon::Speaker }, { "MONITOR", Icon::Headphones } };
    for (int i = 0; i < 3; ++i)
    {
        const bool on = (int) mode == i;
        auto k = modeKeys[i].toFloat();
        g.setColour (on ? juce::Colour (0xffbdbdbd) : Colours::screenHeader);
        g.fillRoundedRectangle (k, 2.0f);
        auto icon = k.removeFromLeft (k.getHeight()).reduced (k.getHeight() * 0.2f);
        drawIcon (g, std::get<1> (keys[i]), icon, on ? juce::Colours::black : Colours::text);
        g.setColour (on ? juce::Colours::black : Colours::text);
        g.setFont (juce::FontOptions (juce::jmin (17.0f, k.getHeight() * 0.48f), juce::Font::bold));
        g.drawFittedText (std::get<0> (keys[i]), k.toNearestInt().reduced (2, 0), juce::Justification::centredLeft, 1);
    }
    g.setColour (Colours::text);
    g.setFont (juce::FontOptions (juce::jmin (24.0f, modeKeys[0].getHeight() * 0.7f), juce::Font::bold));
    g.drawText ("ROUTING", getLocalBounds().withHeight (modeKeys[0].getBottom() + 2).withTrimmedLeft (8), juce::Justification::centredLeft);

    if (mode == Mode::Monitor)
        return;

    // Panels.
    for (auto panel : { leftPanel, rightPanel })
    {
        g.setColour (Colours::screenPanel);
        g.fillRect (panel);
    }
    for (auto header : { leftHeader, rightHeader })
    {
        g.setColour (Colours::screenHeader);
        g.fillRect (header);
    }
    g.setColour (juce::Colour (0xff1c1c1c));
    for (auto panel : { leftPanel, rightPanel })
        g.fillRect (panel.withTrimmedTop (leftHeader.getHeight() + 6).reduced (2));

    auto* sel = selectedStrip();
    auto key = [&g] (juce::Rectangle<int> r, const juce::String& text, bool on, juce::Colour onColour)
    {
        if (r.isEmpty())
            return;
        g.setColour (on ? onColour : juce::Colour (0xff5a5a5a));
        g.fillRect (r);
        g.setColour (on ? juce::Colours::black : Colours::text);
        g.setFont (juce::FontOptions (juce::jmin (20.0f, r.getHeight() * 0.55f), juce::Font::bold));
        g.drawFittedText (text, r.reduced (2, 0), juce::Justification::centred, 1);
    };

    if (mode == Mode::Channels)
    {
        g.setColour (juce::Colours::black);
        g.fillRect (monoKey.getUnion (stereoKey).withHeight (16).translated (0, -16));
        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText ("CHANNEL MODE", monoKey.getUnion (stereoKey).withHeight (16).translated (0, -16), juce::Justification::centred);
        const bool stereo = sel != nullptr && sel->params.stereo.load();
        key (monoKey, "MONO", ! stereo, Colours::cyan);
        key (stereoKey, "STEREO", stereo, Colours::cyan);
    }
    g.setColour (autoAdvance ? Colours::yellow : Colours::text);
    g.setFont (juce::FontOptions (juce::jmin (18.0f, autoKey.getHeight() * 0.42f), juce::Font::bold));
    g.drawFittedText ("+1\nAUTO", autoKey, juce::Justification::centred, 2);
    // Locked: the closed lock lit red; unlocked: the open lock lit green (as on the console).
    g.setColour (locked ? juce::Colour (0xffe8344a) : juce::Colour (0xff3a3a3a));
    g.fillRect (lockKey);
    drawIcon (g, Icon::LockClosed, lockKey.toFloat().reduced (lockKey.getHeight() * 0.2f), locked ? juce::Colours::black : Colours::text);
    g.setColour (! locked ? Colours::green : juce::Colour (0xff3a3a3a));
    g.fillRect (unlockKey);
    drawIcon (g, Icon::LockOpen, unlockKey.toFloat().reduced (unlockKey.getHeight() * 0.2f), ! locked ? juce::Colours::black : Colours::text);
    key (prevPage, "<", false, Colours::text);
    key (nextPage, ">", false, Colours::text);

    // Group selector: icon, title, name, arrow.
    {
        auto r = groupSelector;
        g.setColour (juce::Colour (0xff5a5a5a));
        g.fillRect (r);
        auto arrow = r.removeFromRight (r.getHeight() * 2 / 3);
        g.setColour (juce::Colour (0xff6c6c6c));
        g.fillRect (arrow);
        juce::Path tri;
        const auto c = arrow.getCentre().toFloat();
        tri.addTriangle (c.x - 8.0f, c.y - 4.0f, c.x + 8.0f, c.y - 4.0f, c.x, c.y + 5.0f);
        g.setColour (Colours::text);
        g.fillPath (tri);
        auto icon = r.removeFromLeft (r.getHeight()).reduced (6);
        g.setColour (juce::Colours::black);
        g.fillRect (icon);
        drawIcon (g, mode == Mode::Outputs ? Icon::Speaker : Icon::Xlr, icon.toFloat().reduced (5.0f), Colours::text);
        r.removeFromLeft (8);
        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (juce::jmin (16.0f, r.getHeight() * 0.32f), juce::Font::bold));
        g.drawText (mode == Mode::Outputs ? "OUTPUT GROUP" : "SOURCE GROUP", r.removeFromTop (r.getHeight() * 45 / 100), juce::Justification::bottomLeft);
        g.setFont (juce::FontOptions (juce::jmin (26.0f, r.getHeight() * 0.8f), juce::Font::bold));
        const int a = portGroup * kPortsPerGroup + 1;
        g.drawFittedText ((mode == Mode::Outputs ? "AUDIO OUT " : "AUDIO IN ") + juce::String (a) + "-" + juce::String (a + kPortsPerGroup - 1), r,
                          juce::Justification::centredLeft, 1);
    }
    key (offKey, mode == Mode::Outputs ? "UNPATCH" : "OFF", false, Colours::text);

    // Tiles.
    if (! separator.isEmpty())
    {
        g.setColour (juce::Colour (0xff6a6a6a));
        g.fillRect (separator);
    }
    for (auto& t : targetTiles)
        paintTarget (g, t, t.strip == sel);
    for (size_t i = 0; i < portTiles.size(); ++i)
        paintPort (g, portTiles[i], portGroup * kPortsPerGroup + (int) i);

    if (locked)
    {
        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.fillRect (rightPanel.withTrimmedTop (rightHeader.getHeight() + 6));
    }
}

void RoutingPage::assign (int port, bool right)
{
    auto* s = selectedStrip();
    if (s == nullptr || locked)
        return;
    auto& p = s->params;

    if (mode == Mode::Outputs)
    {
        const bool stereo = p.stereo.load() || s->isOutput();
        if (p.outputLeft.load() == port)
        {
            // Clicking the strip's own output again removes it.
            p.outputLeft = -1;
            p.outputRight = -1;
        }
        else if (right)
            p.outputRight = port;
        else
        {
            p.outputLeft = port;
            p.outputRight = stereo ? port + 1 : -1;
        }
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
        const auto list = allTargets();
        for (size_t i = 0; i + 1 < list.size(); ++i)
            if (list[i] == s)
            {
                select (list[i + 1]);
                break;
            }
    }
    repaint();
}

void RoutingPage::showGroupMenu()
{
    juce::PopupMenu menu;
    for (int i = 0; i < numPortGroups(); ++i)
    {
        const int a = i * kPortsPerGroup + 1;
        menu.addItem (i + 1, (mode == Mode::Outputs ? "AUDIO OUT " : "AUDIO IN ") + juce::String (a) + "-" + juce::String (a + kPortsPerGroup - 1), true,
                      i == portGroup);
    }
    juce::Component::SafePointer<RoutingPage> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withTargetScreenArea (localAreaToGlobal (groupSelector)),
                        [safe] (int r)
                        {
                            if (safe != nullptr && r > 0)
                            {
                                safe->portGroup = r - 1;
                                safe->layoutTiles();
                                safe->repaint();
                            }
                        });
}

void RoutingPage::mouseUp (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();
    for (int i = 0; i < 3; ++i)
        if (modeKeys[i].contains (pos))
        {
            setMode (static_cast<Mode> (i));
            return;
        }
    if (mode == Mode::Monitor)
        return;

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
    else if (groupSelector.contains (pos))
        showGroupMenu();
    else if (monoKey.contains (pos) || stereoKey.contains (pos))
    {
        if (auto* s = selectedStrip(); s != nullptr && ! locked)
            s->params.stereo = stereoKey.contains (pos);
    }
    else if (offKey.contains (pos))
    {
        if (auto* s = selectedStrip(); s != nullptr && ! locked)
        {
            auto& p = s->params;
            (mode == Mode::Outputs ? p.outputLeft : p.inputLeft) = -1;
            (mode == Mode::Outputs ? p.outputRight : p.inputRight) = -1;
        }
    }
    else
    {
        for (auto& t : targetTiles)
            if (t.area.contains (pos))
            {
                select (t.strip);
                if (ctx.selectStrip)
                    ctx.selectStrip (t.strip);
                return;
            }
        for (size_t i = 0; i < portTiles.size(); ++i)
            if (portTiles[i].contains (pos))
            {
                // Right click or shift: the right input/output of a stereo strip.
                assign (portGroup * kPortsPerGroup + (int) i, e.mods.isPopupMenu() || e.mods.isShiftDown());
                return;
            }
    }
    repaint();
}

} // namespace livedaw::ui
