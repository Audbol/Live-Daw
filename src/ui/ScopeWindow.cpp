#include "ui/ScopeWindow.h"

namespace livedaw::ui
{

namespace
{
    const juce::Colour tileBlue = Colours::valueBlue, tileYellow = Colours::gold, configLit = Colours::valueBlue, configUnlit (0xff1f4973),
                       tileOff = Colours::screenTile,
        groupHeader = Colours::screenLine;

    /** The CONTENTS keys and the console sections they map to (until E15
        splits our ten sections into the console's seventeen categories). */
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
} // namespace

ScopeWindow::ScopeWindow (UiContext& c, Mode m, juce::String t, Selection initial)
    : ctx (c), mode (m), title (std::move (t)), selection (std::move (initial))
{
    for (auto* k : { &noneKey, &allKey, &cancelKey, &saveKey })
    {
        styleIconKey (*k);
        addAndMakeVisible (k);
    }
    noneKey.onClick = [this]
    {
        if (stripsActive())
            selection.strips.clear();
        if (contentsActive())
            selection.sections = 0;
        repaint();
    };
    allKey.onClick = [this]
    {
        if (stripsActive())
            for (auto& tile : tiles)
                if (tile.kind == Kind::Strip)
                    toggleStrip (tile.strip, true);
        if (contentsActive())
            selection.sections = state::AllSections;
        repaint();
    };
    cancelKey.onClick = [this] { if (onCancel) onCancel(); };
    saveKey.onClick = [this] { if (onSave) onSave (selection); };
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

bool ScopeWindow::isSelected (const Tile& t) const
{
    switch (t.kind)
    {
        case Kind::Strip:     return hasStrip (t.strip);
        case Kind::Dca:
        case Kind::MuteGroup: return mode != Mode::Global && (selection.sections & state::Groups) != 0;
        case Kind::Fx:        return mode != Mode::Global && (selection.sections & state::Inserts) != 0;
        case Kind::Config:    return false;
        case Kind::Content:
            if (t.bits == kNoneKey || t.bits == kAllKey || t.bits == kBlank)
                return false;
            return (selection.sections & t.bits) != 0;
    }
    return false;
}

void ScopeWindow::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
    g.setColour (Dark::text);
    g.setFont (juce::FontOptions (juce::jmin (20.0f, (float) titleArea.getHeight() * 0.7f), juce::Font::bold));
    g.drawFittedText (title, titleArea, juce::Justification::centredLeft, 1);

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
        const bool active = (t.kind == Kind::Strip && stripsActive()) || (t.kind == Kind::Content && contentsActive());
        const bool on = isSelected (t);
        auto r = t.area.toFloat().reduced (1.5f);
        juce::Colour fill = tileOff;
        if (t.kind == Kind::Config)
            fill = configUnlit;
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
    titleArea = b.removeFromTop (juce::jlimit (20, 30, getHeight() / 16)).reduced (6, 0);
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

    // Middle column: DCA, MUTEGROUP and FX (whole-console items: E17 for safes, E15 for scopes).
    auto m = middle;
    const int mw = m.getWidth() / 8;
    const int mh = juce::jmin (tileH, mw * 3 / 4);
    auto addRowGroup = [&] (const char* groupTitle, const char* caption, Kind kind, int count)
    {
        Group gr { groupTitle, m.removeFromTop (headerH), tiles.size(), (size_t) count };
        for (int i = 0; i < count; ++i)
            tiles.push_back ({ kind, juce::Rectangle<int> (m.getX() + (i % 8) * mw, m.getY() + (i / 8) * mh, mw, mh), caption, juce::String (i + 1) });
        m.removeFromTop ((count + 7) / 8 * mh + 6);
        groups.push_back (gr);
    };
    addRowGroup ("DCA", "DCA", Kind::Dca, kNumDcas);
    addRowGroup ("MUTEGROUP", "MUTE", Kind::MuteGroup, kNumMuteGroups);
    addRowGroup ("FX", "FX", Kind::Fx, kNumFx);

    // Right column: NONE / ALL / CANCEL, CONFIGURATION, CONTENTS and SAVE.
    auto r = right;
    const int keyH = juce::jlimit (26, 40, getHeight() / 12);
    auto top = r.removeFromTop (keyH);
    const int kw = top.getWidth() / 3;
    noneKey.setBounds (top.removeFromLeft (kw).reduced (2, 0));
    allKey.setBounds (top.removeFromLeft (kw).reduced (2, 0));
    cancelKey.setBounds (top.reduced (2, 0));
    saveKey.setBounds (r.removeFromBottom (keyH).withTrimmedLeft (r.getWidth() / 2).reduced (2, 0));
    r.removeFromBottom (4);
    r.removeFromTop (6);

    const int cw = r.getWidth() / 4;
    const int ch = juce::jlimit (16, 40, (r.getHeight() - 2 * (headerH + 6)) / 7);
    configHeader = r.removeFromTop (headerH);
    const char* configKeys[] = { "CONFIG", "SFC", "PREFS", "L", "C", "R" };
    for (int i = 0; i < 6; ++i)
        tiles.push_back ({ Kind::Config, juce::Rectangle<int> (r.getX() + (i % 3) * (r.getWidth() / 3), r.getY() + (i / 3) * ch, r.getWidth() / 3, ch), {},
                           configKeys[i] });
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
    if (stripsActive())
        for (auto& gr : groups)
            if (gr.header.contains (pos) && gr.count > 0 && tiles[gr.first].kind == Kind::Strip)
            {
                bool all = true;
                for (size_t i = gr.first; i < gr.first + gr.count; ++i)
                    all = all && hasStrip (tiles[i].strip);
                for (size_t i = gr.first; i < gr.first + gr.count; ++i)
                    toggleStrip (tiles[i].strip, ! all);
                repaint();
                return;
            }

    for (auto& t : tiles)
    {
        if (! t.area.contains (pos))
            continue;
        if (t.kind == Kind::Strip && stripsActive())
            toggleStrip (t.strip, ! hasStrip (t.strip));
        else if (t.kind == Kind::Content && contentsActive())
        {
            if (t.bits == kNoneKey)
                selection.sections = 0;
            else if (t.bits == kAllKey)
                selection.sections = state::AllSections;
            else if (t.bits != kBlank)
                selection.sections ^= t.bits;
        }
        repaint();
        return;
    }
}

} // namespace livedaw::ui
