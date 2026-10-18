#include "ui/ScopeWindow.h"

namespace livedaw::ui
{

namespace
{
    const juce::Colour tileBlue = Colours::valueBlue, tileYellow = Colours::gold, configLit = Colours::valueBlue, configUnlit (0xff1f4973),
                       tileOff = Colours::screenTile, groupHeader = Colours::screenLine;

    /** The CONTENTS keys and the console sections they map to (our ten
        sections; the console has seventeen categories). */
    struct ContentKey
    {
        const char* name;
        juce::uint32 bits;
        bool dropdown;
    };

    constexpr juce::uint32 kNoneKey = 0xffff0000u, kAllKey = 0xfffe0000u, kBlank = 0;

    const ContentKey contentKeys[] = {
        { "CUST", state::Name, false },       { "TAGS", state::Groups, false },  { "NONE", kNoneKey, false },          { "ALL", kAllKey, false },
        { "CONN", state::Routing, false },    { "IN", state::Preamp, false },    { "FILTER", state::Preamp, false },   { "DELAY", state::Preamp, false },
        { "GATE", state::Gate, false },       { "DYN", state::Dynamics, false }, { "PRE", state::Inserts, false },     { "POST", state::Inserts, false },
        { "EQ", state::Eq, false },           { "PAN", state::Fader, false },    { "MAIN", state::Sends, true },       { "SEND", state::Sends, true },
        { "FDR", state::Fader, false },       { "MUTE", state::Fader, false },   { "", kBlank, false },                { "CONFIG", state::Inserts, false },
    };

    constexpr juce::uint32 kConsoleTile = 1;
} // namespace

ScopeWindow::ScopeWindow (UiContext& c, Mode m, juce::String t, state::Scope initial)
    : ctx (c), mode (m), title (std::move (t)), selection (std::move (initial))
{
    // NONE / ALL / CANCEL: icons over their captions, as on the console.
    for (auto* k : { &noneKey, &allKey, &cancelKey, &saveKey, &noGlobKey })
    {
        styleIconKey (*k, k == &noneKey || k == &allKey || k == &cancelKey);
        addAndMakeVisible (k);
    }
    noGlobKey.setVisible (mode == Mode::PartialLoad);
    noGlobKey.isOn = [this] { return noGlobal; };
    noGlobKey.onClick = [this] { noGlobal = ! noGlobal; noGlobKey.repaint(); };
    if (mode == Mode::PartialLoad)
        saveKey.setButtonText ("LOAD");
    else if (mode == Mode::PartialUpdate)
        saveKey.setButtonText ("UPDATE");

    noneKey.onClick = [this]
    {
        for (auto& tile : tiles)
            if (isActive (tile) && tile.kind != Kind::Content)
                toggle (tile, false);
        selection.sections = 0;
        repaint();
    };
    allKey.onClick = [this]
    {
        for (auto& tile : tiles)
            if (isActive (tile) && tile.kind != Kind::Content)
                toggle (tile, true);
        if (mode != Mode::Global)
            selection.sections = state::AllSections;
        repaint();
    };
    cancelKey.onClick = [this] { if (onCancel) onCancel(); };
    saveKey.onClick = [this]
    {
        // Every strip ticked means "all strips" (strips added later included).
        if (! selection.allStrips && mode != Mode::Global && mode != Mode::SnippetFocus && mode != Mode::SnippetAdd && mode != Mode::SnippetRemove)
        {
            size_t total = 0;
            for (int k = 0; k < kNumStripKinds; ++k)
                total += ctx.engine.getStrips (static_cast<StripKind> (k)).size();
            if (selection.strips.size() == total)
            {
                selection.allStrips = true;
                selection.strips.clear();
            }
        }
        if (onSave)
            onSave (selection, noGlobal);
    };
    if (selection.allStrips)
    {
        // Show "every strip" as every strip ticked.
        selection.allStrips = false;
        selection.strips.clear();
        for (int k = 0; k < kNumStripKinds; ++k)
            for (auto& s : ctx.engine.getStrips (static_cast<StripKind> (k)))
                selection.strips.push_back (state::StripRef::of (*s));
    }
}

bool ScopeWindow::hasStrip (const state::StripRef& r) const
{
    return std::find (selection.strips.begin(), selection.strips.end(), r) != selection.strips.end();
}

void ScopeWindow::toggleStrip (const state::StripRef& r, bool on)
{
    auto& s = selection.strips;
    s.erase (std::remove (s.begin(), s.end(), r), s.end());
    if (on)
        s.push_back (r);
}

juce::uint32* ScopeWindow::maskOf (Kind k)
{
    switch (k)
    {
        case Kind::Dca:       return &selection.dcas;
        case Kind::MuteGroup: return &selection.muteGroups;
        case Kind::Fx:        return &selection.fx;
        case Kind::Strip:
        case Kind::Config:
        case Kind::Content:   break;
    }
    return nullptr;
}

bool ScopeWindow::isActive (const Tile& t) const
{
    switch (t.kind)
    {
        case Kind::Strip:
        case Kind::Dca:
        case Kind::MuteGroup:
        case Kind::Fx:      return true;
        case Kind::Config:  return t.bits == kConsoleTile;
        case Kind::Content: return t.bits != kBlank;
    }
    return false;
}

bool ScopeWindow::isSelected (const Tile& t) const
{
    switch (t.kind)
    {
        case Kind::Strip:     return hasStrip (t.strip);
        case Kind::Dca:       return (selection.dcas & t.bits) != 0;
        case Kind::MuteGroup: return (selection.muteGroups & t.bits) != 0;
        case Kind::Fx:        return (selection.fx & t.bits) != 0;
        case Kind::Config:    return t.bits == kConsoleTile && selection.console;
        case Kind::Content:
            if (t.bits == kNoneKey || t.bits == kAllKey || t.bits == kBlank)
                return false;
            return (selection.sections & t.bits) != 0;
    }
    return false;
}

void ScopeWindow::toggle (const Tile& t, bool on)
{
    if (t.kind == Kind::Strip)
        toggleStrip (t.strip, on);
    else if (t.kind == Kind::Config && t.bits == kConsoleTile)
        selection.console = on;
    else if (auto* mask = maskOf (t.kind))
        *mask = on ? (*mask | t.bits) : (*mask & ~t.bits);
}

void ScopeWindow::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
    // What the window edits (EDIT SCOPE: <scene>, GLOBAL SAFE, ...) on the
    // #202020 window title bar of the other WING windows.
    g.setColour (Colours::screenHeader);
    g.fillRect (titleArea);
    g.setColour (Colours::caption);
    g.setFont (panelTitleFont (titleArea.getHeight()));
    g.drawFittedText (title, titleArea.reduced (12, 0), juce::Justification::centredLeft, 1, 0.8f);

    for (auto& gr : groups)
    {
        g.setColour (groupHeader);
        g.fillRect (gr.header);
        g.setColour (Dark::text);
        g.setFont (juce::FontOptions (juce::jmin (14.0f, (float) gr.header.getHeight() * 0.8f), juce::Font::bold));
        g.drawText (gr.title, gr.header, juce::Justification::centred);
    }
    for (auto [area, text] : { std::pair { configHeader, "CONFIGURATION" }, std::pair { contentsHeader, "CONTENTS" } })
    {
        g.setColour (groupHeader);
        g.fillRect (area);
        g.setColour (Dark::text);
        g.setFont (juce::FontOptions (juce::jmin (14.0f, (float) area.getHeight() * 0.8f), juce::Font::bold));
        g.drawText (text, area, juce::Justification::centred);
    }

    for (auto& t : tiles)
    {
        if (t.kind == Kind::Content && t.bits == kBlank)
            continue;
        const bool active = isActive (t);
        const bool on = isSelected (t);
        auto r = t.area.toFloat().reduced (1.5f);
        juce::Colour fill = tileOff;
        if (t.kind == Kind::Config)
            fill = on ? configLit : configUnlit;
        else if (on)
            fill = t.kind == Kind::Content ? tileYellow : tileBlue;
        if (! active)
            fill = fill.darker (0.55f).withMultipliedSaturation (0.6f);
        g.setColour (fill);
        g.fillRect (r);
        auto ink = on ? juce::Colours::black : Dark::text;
        if (! active)
            ink = ink.withAlpha (0.45f);
        g.setColour (ink);
        if (t.caption.isNotEmpty())
        {
            auto cap = r.removeFromTop (r.getHeight() * 0.36f);
            g.setFont (juce::FontOptions (juce::jlimit (7.0f, 11.0f, cap.getHeight() * 0.9f), juce::Font::bold));
            g.drawText (t.caption, cap.toNearestInt(), juce::Justification::centred);
        }
        g.setFont (juce::FontOptions (juce::jlimit (8.0f, 18.0f, juce::jmin (r.getHeight() * 0.8f, r.getWidth() * 0.4f)), juce::Font::bold));
        g.drawFittedText (t.number, r.toNearestInt(), juce::Justification::centred, 1, 0.7f);
        if (t.dropdown)
        {
            juce::Path tri;
            const auto br = t.area.toFloat().reduced (3.0f).getBottomRight();
            tri.addTriangle (br.x - 7.0f, br.y - 4.0f, br.x - 1.0f, br.y - 4.0f, br.x - 4.0f, br.y);
            g.fillPath (tri);
        }
    }
}

void ScopeWindow::resized()
{
    tiles.clear();
    groups.clear();

    auto b = getLocalBounds();
    titleArea = b.removeFromTop (juce::jlimit (20, 30, getHeight() / 16));
    b.removeFromTop (4);
    const int gap = 10;
    auto left = b.removeFromLeft (b.getWidth() * 38 / 100);
    b.removeFromLeft (gap);
    auto middle = b.removeFromLeft (b.getWidth() * 60 / 100);
    b.removeFromLeft (gap);
    auto right = b;

    // Left column: the strips, 8 to a row.
    struct StripGroup
    {
        const char* title;
        const char* caption;
        StripKind kind;
    };
    const StripGroup stripGroups[] = { { "CHANNEL", "CH", StripKind::Channel }, { "AUX", "AUX", StripKind::Aux }, { "BUS", "BUS", StripKind::Bus },
                                       { "MAIN", "MAIN", StripKind::Main }, { "MATRIX", "MTX", StripKind::Matrix } };
    const int headerH = juce::jlimit (12, 18, getHeight() / 28);
    int rows = 0, nonEmpty = 0;
    for (auto& sg : stripGroups)
    {
        const int n = (int) ctx.engine.getStrips (sg.kind).size();
        rows += (n + 7) / 8;
        nonEmpty += n > 0 ? 1 : 0;
    }
    const int tileW = left.getWidth() / 8;
    const int tileH = juce::jlimit (10, juce::jmax (10, tileW * 3 / 4), (left.getHeight() - nonEmpty * (headerH + 4)) / juce::jmax (1, rows));
    auto l = left;
    for (auto& sg : stripGroups)
    {
        auto& strips = ctx.engine.getStrips (sg.kind);
        if (strips.empty())
            continue;
        Group gr { sg.title, l.removeFromTop (headerH), tiles.size(), strips.size() };
        for (size_t i = 0; i < strips.size(); ++i)
        {
            const auto& s = *strips[i];
            const int col = (int) i % 8, row = (int) i / 8;
            tiles.push_back ({ Kind::Strip, juce::Rectangle<int> (l.getX() + col * tileW, l.getY() + row * tileH, tileW, tileH), sg.caption,
                               juce::String (s.getNumber()), state::StripRef::of (s) });
        }
        l.removeFromTop (((int) strips.size() + 7) / 8 * tileH + 4);
        groups.push_back (gr);
    }

    // Middle column: DCA, MUTEGROUP and FX.
    auto m = middle;
    const int mw = m.getWidth() / 8;
    const int mh = juce::jmin (tileH, mw * 3 / 4);
    auto addRowGroup = [&] (const char* groupTitle, const char* caption, Kind kind, int count)
    {
        Group gr { groupTitle, m.removeFromTop (headerH), tiles.size(), (size_t) count };
        for (int i = 0; i < count; ++i)
        {
            Tile t { kind, juce::Rectangle<int> (m.getX() + (i % 8) * mw, m.getY() + (i / 8) * mh, mw, mh), caption, juce::String (i + 1) };
            t.bits = 1u << i;
            tiles.push_back (t);
        }
        m.removeFromTop ((count + 7) / 8 * mh + 6);
        groups.push_back (gr);
    };
    addRowGroup ("DCA", "DCA", Kind::Dca, kNumDcas);
    addRowGroup ("MUTEGROUP", "MUTE", Kind::MuteGroup, kNumMuteGroups);
    addRowGroup ("FX", "FX", Kind::Fx, kNumFx);

    // Right column: NONE / ALL / CANCEL (NO GLOB), CONFIGURATION, CONTENTS and SAVE.
    auto r = right;
    const int keyH = juce::jlimit (26, 40, getHeight() / 12);
    auto top = r.removeFromTop (keyH);
    const int kw = top.getWidth() / 3;
    noneKey.setBounds (top.removeFromLeft (kw).reduced (2, 0));
    allKey.setBounds (top.removeFromLeft (kw).reduced (2, 0));
    cancelKey.setBounds (top.reduced (2, 0));
    auto bottom = r.removeFromBottom (keyH);
    saveKey.setBounds (bottom.removeFromRight (bottom.getWidth() / 2).reduced (2, 0));
    noGlobKey.setBounds (bottom.reduced (2, 0));
    r.removeFromBottom (4);
    r.removeFromTop (6);

    const int cw = r.getWidth() / 4;
    const int ch = juce::jlimit (16, 40, (r.getHeight() - 2 * (headerH + 6)) / 7);
    configHeader = r.removeFromTop (headerH);
    const char* configKeys[] = { "CONFIG", "SFC", "PREFS", "L", "C", "R" };
    for (int i = 0; i < 6; ++i)
    {
        Tile t { Kind::Config, juce::Rectangle<int> (r.getX() + (i % 3) * (r.getWidth() / 3), r.getY() + (i / 3) * ch, r.getWidth() / 3, ch), {},
                 configKeys[i] };
        t.bits = i == 0 ? kConsoleTile : 0;
        tiles.push_back (t);
    }
    r.removeFromTop (2 * ch + 6);
    contentsHeader = r.removeFromTop (headerH);
    for (int i = 0; i < (int) std::size (contentKeys); ++i)
    {
        Tile t { Kind::Content, juce::Rectangle<int> (r.getX() + (i % 4) * cw, r.getY() + (i / 4) * ch, cw, ch), {}, contentKeys[i].name };
        t.bits = contentKeys[i].bits;
        t.dropdown = contentKeys[i].dropdown;
        tiles.push_back (t);
    }
}

void ScopeWindow::mouseUp (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();
    // A group header ticks (or unticks) the whole group.
    for (auto& gr : groups)
        if (gr.header.contains (pos) && gr.count > 0)
        {
            bool all = true;
            for (size_t i = gr.first; i < gr.first + gr.count; ++i)
                all = all && isSelected (tiles[i]);
            for (size_t i = gr.first; i < gr.first + gr.count; ++i)
                toggle (tiles[i], ! all);
            repaint();
            return;
        }

    for (auto& t : tiles)
    {
        if (! t.area.contains (pos) || ! isActive (t))
            continue;
        if (t.kind == Kind::Content)
        {
            if (t.bits == kNoneKey)
                selection.sections = 0;
            else if (t.bits == kAllKey)
                selection.sections = state::AllSections;
            else
                selection.sections ^= t.bits;
        }
        else
            toggle (t, ! isSelected (t));
        repaint();
        return;
    }
}

} // namespace livedaw::ui
