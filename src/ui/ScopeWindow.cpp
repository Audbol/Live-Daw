#include "ui/ScopeWindow.h"

namespace livedaw::ui
{

namespace
{
    const juce::Colour tileBlue = Colours::valueBlue, tileYellow = Colours::gold, configLit = Colours::valueBlue, tileOff = Colours::screenPanel,
                       groupHeader = Colours::screenLine;

    /** The CONTENTS keys and the console sections they map to (our ten
        sections; the console has seventeen categories). */
    struct ContentKey
    {
        const char* name;
        juce::uint32 bits;
        bool dropdown;
        Icon icon;
    };

    constexpr juce::uint32 kNoneKey = 0xffff0000u, kAllKey = 0xfffe0000u, kBlank = 0;

    const ContentKey contentKeys[] = {
        { "CUST", state::Name, false, Icon::KeyboardEdit }, { "TAGS", state::Groups, false, Icon::Tag },   { "NONE", kNoneKey, false, Icon::ListNone },
        { "ALL", kAllKey, false, Icon::ListAll },          { "CONN", state::Routing, false, Icon::Link },  { "IN", state::Preamp, false, Icon::Gear },
        { "FILTER", state::Preamp, false, Icon::LowCut },  { "DELAY", state::Preamp, false, Icon::DeltaT }, { "GATE", state::Gate, false, Icon::Gate },
        { "DYN", state::Dynamics, false, Icon::Compressor }, { "PRE", state::Inserts, false, Icon::Insert }, { "POST", state::Inserts, false, Icon::Insert },
        { "EQ", state::Eq, false, Icon::Eq },              { "PAN", state::Fader, false, Icon::Circle },   { "MAIN", state::Sends, true, Icon::Faders },
        { "SEND", state::Sends, true, Icon::Sends },       { "FDR", state::Fader, false, Icon::GainArrows }, { "MUTE", state::Fader, false, Icon::MuteGroup },
        { "", kBlank, false, Icon::Circle },               { "CONFIG", state::Inserts, false, Icon::Wrench },
    };

    /** The CONFIGURATION keys: CONFIG is the console-wide settings, the rest
        (the surface, preferences, safes and layer sets) aren't kept here. */
    struct ConfigKey
    {
        const char* name;
        Icon icon;
        bool dropdown;
    };
    const ConfigKey configKeys[] = { { "CONFIG", Icon::Overview, false }, { "SFC", Icon::Faders, false },  { "PREFS", Icon::Wrench, false },
                                     { "SAFES", Icon::LockClosed, false }, { "L", Icon::Layers, true },    { "C", Icon::Layers, true },
                                     { "R", Icon::Layers, true },         { "CC", Icon::CcEdit, true },    { "CMPCT", Icon::Layers, true },
                                     { "RCK", Icon::Layers, true },       { "EXT", Icon::Layers, true },   { "VRT", Icon::Layers, true } };

    /** INITIALIZE CONSOLE's SOURCE and OUTPUT groups (the console's names). */
    const char* const sourceNames[] = { "LCL", "AUX", "A", "B", "C", "SC", "USB", "CRD", "MOD", "PLAY", "AES", "USR", "OSC" };
    const char* const outputNames[] = { "LCL", "AUX", "A", "B", "C", "SC", "USB", "CRD", "MOD", "REC", "AES" };
    constexpr juce::uint32 kSidecarSources =
        ScopeWindow::SourceLocal | ScopeWindow::SourceCard | ScopeWindow::SourcePlayer | ScopeWindow::SourceUser | ScopeWindow::SourceOsc;

    constexpr juce::uint32 kConsoleTile = 1;

    /** GLOBAL SAFE's SOURCE and OUTPUT groups (G5.6.3): the engine's safes
        behind group `group` (its index in sourceNames / outputNames): source
        table indices or output ports. Empty for groups Sidecar doesn't have.
        LCL and A are the same ports here, as on the remote's patch. */
    std::vector<int> ioItems (bool outputs, int group)
    {
        std::vector<int> items;
        auto ports = [&items] (int first, int count, bool sourceIndex)
        {
            for (int n = 0; n < count; ++n)
                items.push_back (sourceIndex ? MixerEngine::sourceIndex (first + n) : first + n);
        };
        const juce::String name (outputs ? outputNames[group] : sourceNames[group]);
        const int block = name == "LCL" || name == "A" ? 0 : name == "B" ? 1 : name == "C" ? 2 : -1;
        if (block >= 0)
            ports (block * 48, 48, ! outputs);
        else if (! outputs && name == "CRD")
            ports (ld::kPlaybackSourceBase, ld::kNumPlaybackChannels, true);
        else if (! outputs && name == "PLAY")
            ports (ld::kUsbPlayerSourceBase, ld::kUsbChannels, true);
        else if (! outputs && name == "OSC")
            ports (ld::kOscSourceBase, ld::kNumOscillators, true);
        items.erase (std::remove (items.begin(), items.end(), -1), items.end());
        return items;
    }
} // namespace

ScopeWindow::ScopeWindow (UiContext& c, Mode m, juce::String t, state::Scope initial)
    : ctx (c), mode (m), title (std::move (t)), selection (std::move (initial))
{
    // NONE and ALL: icons over their captions; CANCEL: the caption beside its icon.
    for (auto* k : { &noneKey, &allKey, &cancelKey, &saveKey, &noGlobKey })
    {
        styleIconKey (*k, k == &noneKey || k == &allKey || k == &cancelKey);
        addAndMakeVisible (k);
    }
    cancelKey.iconAbove = false;
    noGlobKey.setVisible (mode == Mode::PartialLoad);
    if (mode == Mode::Global)
    {
        // The source and output safes are the engine's; SAVE writes them back.
        for (const bool outputs : { false, true })
            for (int g = 0; g < (int) (outputs ? std::size (outputNames) : std::size (sourceNames)); ++g)
                for (const int i : ioItems (outputs, g))
                    if (ld::ld_engine_io_safe (ctx.engine.getHandle(), outputs ? 1 : 0, i))
                        (outputs ? safeOutputs : safeSources).insert (i);
    }
    if (mode == Mode::Initialize)
    {
        saveKey.setButtonText ("INIT");
        saveKey.setIcon (Icon::Reset);
    }
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
        sourceGroups = 0;
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
        if (mode == Mode::Global)
            for (const bool outputs : { false, true })
                for (int g = 0; g < (int) (outputs ? std::size (outputNames) : std::size (sourceNames)); ++g)
                    for (const int i : ioItems (outputs, g))
                        ld::ld_engine_set_io_safe (ctx.engine.getHandle(), outputs ? 1 : 0, i, (outputs ? safeOutputs : safeSources).count (i) > 0);
        if (onSave)
            onSave (selection, noGlobal);
    };
    if (selection.allStrips && mode != Mode::Initialize)
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
        case Kind::Source:    return &sourceGroups;
        case Kind::Strip:
        case Kind::Config:
        case Kind::Content:
        case Kind::Output:    break;
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
        // INITIALIZE CONSOLE resets strips, groups, FX and sources, not the console-wide settings.
        case Kind::Config:  return t.bits == kConsoleTile && mode != Mode::Initialize;
        case Kind::Content: return t.bits != kBlank;
        case Kind::Source:  return mode == Mode::Global ? ! ioItems (false, groupOf (t)).empty() : (t.bits & kSidecarSources) != 0;
        case Kind::Output:  return mode == Mode::Global && ! ioItems (true, groupOf (t)).empty();
    }
    return false;
}

int ScopeWindow::groupOf (const Tile& t)
{
    for (int i = 0; i < 32; ++i)
        if (t.bits == (1u << i))
            return i;
    return 0;
}

int ScopeWindow::ioSafeCount (const Tile& t) const
{
    const bool outputs = t.kind == Kind::Output;
    const auto& set = outputs ? safeOutputs : safeSources;
    int n = 0;
    for (const int i : ioItems (outputs, groupOf (t)))
        n += set.count (i) > 0 ? 1 : 0;
    return n;
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
        case Kind::Source:
        case Kind::Output:
            if (mode == Mode::Global) // every source / output of the group is safe
                return ioSafeCount (t) > 0 && ioSafeCount (t) == (int) ioItems (t.kind == Kind::Output, groupOf (t)).size();
            return t.kind == Kind::Source && (sourceGroups & t.bits) != 0;
        case Kind::Content:
            if (t.bits == kNoneKey || t.bits == kAllKey || t.bits == kBlank)
                return false;
            return (selection.sections & t.bits) != 0;
    }
    return false;
}

void ScopeWindow::toggle (const Tile& t, bool on)
{
    if (mode == Mode::Global && (t.kind == Kind::Source || t.kind == Kind::Output))
    {
        auto& set = t.kind == Kind::Output ? safeOutputs : safeSources;
        for (const int i : ioItems (t.kind == Kind::Output, groupOf (t)))
            if (on)
                set.insert (i);
            else
                set.erase (i);
        return;
    }
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
    // What the window edits (EDIT SCOPE: <scene>, GLOBAL SAFE, ...) in the free
    // space above the DCAs (INITIALIZE CONSOLE has SOURCE and OUTPUT there).
    if (! titleArea.isEmpty())
    {
        g.setColour (Colours::caption);
        g.setFont (panelTitleFont (juce::roundToInt (wingPx (64.0f))));
        g.drawFittedText (title, titleArea, juce::Justification::centred, 3, 0.8f);
    }

    // [Fig. 47] a group: a #484848 bar with its title over a black well in a
    // #484848 frame.
    const int frame = juce::jmax (1, juce::roundToInt (wingPx (3.0f)));
    auto paintGroup = [&] (juce::Rectangle<int> header, juce::Rectangle<int> box, const juce::String& text)
    {
        g.setColour (groupHeader);
        g.fillRect (header.getUnion (box));
        g.setColour (juce::Colours::black);
        g.fillRect (box.reduced (frame));
        g.setColour (Dark::caption);
        g.setFont (wingFontFit (13.0f / 800.0f, (float) header.getHeight() * 0.9f, WingWeight::SemiBold)); // 13 px capitals (3.1)
        g.drawText (text, header, juce::Justification::centred);
    };
    for (auto& gr : groups)
    {
        juce::Rectangle<int> box;
        for (size_t i = gr.first; i < gr.first + gr.count; ++i)
            box = box.isEmpty() ? tiles[i].area : box.getUnion (tiles[i].area);
        paintGroup (gr.header, box.expanded (frame).withTop (gr.header.getBottom()), gr.title);
    }
    paintGroup (configHeader, configBox, "CONFIGURATION");
    paintGroup (contentsHeader, contentsBox, "CONTENTS");

    for (auto& t : tiles)
    {
        if (t.kind == Kind::Content && t.bits == kBlank)
            continue;
        const bool active = isActive (t);
        const bool on = isSelected (t);
        auto r = t.area.toFloat().reduced (juce::jmax (1.0f, wingPx (2.0f)));
        juce::Colour fill = tileOff;
        if (on)
            fill = t.kind == Kind::Content ? tileYellow : (t.kind == Kind::Config ? configLit : tileBlue);
        else if (mode == Mode::Global && (t.kind == Kind::Source || t.kind == Kind::Output) && ioSafeCount (t) > 0)
            fill = tileBlue.darker (0.9f); // [manual 5.6 p. 54] part of the group: dimly lit
        g.setColour (fill);
        g.fillRoundedRectangle (r, wingPx (3.0f));
        auto ink = on ? juce::Colours::black : (active ? Dark::textDim.brighter (0.25f) : Dark::textDim.withAlpha (0.35f));
        if (t.icon.has_value())
        {
            // An icon over its caption.
            auto label = r.removeFromBottom (r.getHeight() * 0.34f);
            drawIcon (g, *t.icon, r.reduced (r.getHeight() * 0.16f), ink);
            g.setColour (ink);
            g.setFont (fontFittingWidth (wingFontFit (TypeScale::statusCaption, label.getHeight() * 0.85f), t.number, label.getWidth() - 2.0f, 0.7f));
            g.drawText (t.number, label.toNearestInt(), juce::Justification::centredTop);
        }
        else
        {
            // A small caption over a rule, the number under it.
            auto cap = r.removeFromTop (r.getHeight() * 0.38f);
            g.setColour (on ? juce::Colours::black : Dark::textDim.withAlpha (active ? 0.8f : 0.35f));
            g.setFont (wingFontFit (TypeScale::statusCaption, cap.getHeight() * 0.75f));
            g.drawText (t.caption, cap.toNearestInt(), juce::Justification::centred);
            g.setColour (on ? juce::Colours::black.withAlpha (0.5f) : juce::Colours::black);
            g.fillRect (cap.getX() + cap.getWidth() * 0.1f, cap.getBottom() - 1.0f, cap.getWidth() * 0.8f, juce::jmax (1.0f, wingPx (2.0f)));
            g.setColour (ink);
            g.setFont (wingFontFit (TypeScale::value, juce::jmin (r.getHeight() * 0.75f, r.getWidth() * 0.4f)));
            g.drawFittedText (t.number, r.toNearestInt(), juce::Justification::centred, 1, 0.7f);
        }
        if (t.dropdown)
        {
            juce::Path tri;
            const auto br = t.area.toFloat().reduced (wingPx (4.0f)).getBottomRight();
            const float a = juce::jmax (3.0f, wingPx (4.0f));
            tri.addTriangle (br.x - a * 1.6f, br.y - a, br.x, br.y - a, br.x - a * 0.8f, br.y);
            g.setColour (ink);
            g.fillPath (tri);
        }
    }
}

void ScopeWindow::resized()
{
    tiles.clear();
    groups.clear();

    // [Fig. 47] in px of the 1280 x 720 page under the status bar: the strip
    // groups at the left (52 px tiles on a 56 px pitch), SOURCE, OUTPUT (only
    // on INITIALIZE CONSOLE), DCA, MUTEGROUP and FX in the middle, NONE / ALL /
    // CANCEL, CONFIGURATION, CONTENTS and SAVE (INIT, LOAD ...) at the right.
    const float sx = (float) getWidth() / 1280.0f, sy = (float) getHeight() / 720.0f;
    auto X = [&] (float px) { return juce::roundToInt (px * sx); };
    auto Y = [&] (float px) { return juce::roundToInt ((px - 80.0f) * sy); };
    auto rect = [&] (float x0, float y0, float x1, float y1) { return juce::Rectangle<int>::leftTopRightBottom (X (x0), Y (y0), X (x1), Y (y1)); };
    const int headerH = juce::jmax (10, Y (108.0f) - Y (84.0f));
    const int pitchX = X (92.0f) - X (36.0f);

    // A group of `count` tiles, eight to a row, from `top`; returns the bottom.
    auto addGroup = [&] (const juce::String& groupTitle, float left, int top, int count, int pitch, std::function<Tile (int index, juce::Rectangle<int>)> make)
    {
        Group gr { groupTitle, juce::Rectangle<int> (X (left - 6.0f), top, X (left + 450.0f) - X (left - 6.0f), headerH), tiles.size(), (size_t) count };
        const int y0 = top + headerH + juce::roundToInt (4.0f * sy);
        for (int i = 0; i < count; ++i)
            tiles.push_back (make (i, juce::Rectangle<int> (X (left) + (i % 8) * pitchX, y0 + (i / 8) * pitch, pitchX, pitch)));
        groups.push_back (gr);
        return y0 + (count + 7) / 8 * pitch + juce::roundToInt (4.0f * sy);
    };
    auto simple = [] (Kind kind, const char* caption)
    {
        return [kind, caption] (int i, juce::Rectangle<int> area)
        {
            Tile t { kind, area, caption, juce::String (i + 1) };
            t.bits = 1u << i;
            return t;
        };
    };

    // Left column: the strips; more than the console's 40 channels shrink the rows.
    struct StripGroup
    {
        const char* title;
        const char* caption;
        StripKind kind;
    };
    const StripGroup stripGroups[] = { { "CHANNEL", "CH", StripKind::Channel }, { "AUX", "AUX", StripKind::Aux }, { "BUS", "BUS", StripKind::Bus },
                                       { "MAIN", "MAIN", StripKind::Main }, { "MATRIX", "MTX", StripKind::Matrix } };
    int rows = 0, nonEmpty = 0;
    for (auto& sg : stripGroups)
    {
        const int n = (int) ctx.engine.getStrips (sg.kind).size();
        rows += (n + 7) / 8;
        nonEmpty += n > 0 ? 1 : 0;
    }
    const int stripPitch = juce::jmin (Y (168.0f) - Y (112.0f),
                                       (Y (792.0f) - Y (84.0f) - nonEmpty * (headerH + juce::roundToInt (8.0f * sy))) / juce::jmax (1, rows));
    int y = Y (84.0f);
    for (auto& sg : stripGroups)
    {
        auto& strips = ctx.engine.getStrips (sg.kind);
        if (strips.empty())
            continue;
        y = addGroup (sg.title, 36.0f, y, (int) strips.size(), stripPitch, [&strips, &sg] (int i, juce::Rectangle<int> area)
        {
            const auto& s = *strips[(size_t) i];
            return Tile { Kind::Strip, area, sg.caption, juce::String (s.getNumber()), state::StripRef::of (s) };
        });
    }

    // Middle column.
    const int pitchY = Y (168.0f) - Y (112.0f);
    titleArea = {};
    if (mode == Mode::Initialize || mode == Mode::Global)
    {
        addGroup ("SOURCE", 530.0f, Y (84.0f), (int) std::size (sourceNames), pitchY, [] (int i, juce::Rectangle<int> area)
        {
            Tile t { Kind::Source, area, "SRC", sourceNames[i] };
            t.bits = 1u << i;
            t.dropdown = true;
            return t;
        });
        addGroup ("OUTPUT", 530.0f, Y (245.0f), (int) std::size (outputNames), pitchY, [] (int i, juce::Rectangle<int> area)
        {
            Tile t { Kind::Output, area, "OUT", outputNames[i] };
            t.bits = 1u << i;
            t.dropdown = true;
            return t;
        });
    }
    else
        titleArea = rect (530.0f, 100.0f, 980.0f, 380.0f);
    const int mgTop = addGroup ("DCA", 530.0f, Y (406.0f), kNumDcas, pitchY, simple (Kind::Dca, "DCA"));
    addGroup ("MUTEGROUP", 530.0f, mgTop, kNumMuteGroups, pitchY, simple (Kind::MuteGroup, "MUTE"));
    addGroup ("FX", 530.0f, Y (652.0f), kNumFx, pitchY, simple (Kind::Fx, "FX"));

    // Right column.
    noneKey.setBounds (rect (999.0f, 84.0f, 1061.0f, 148.0f));
    allKey.setBounds (rect (1063.0f, 84.0f, 1119.0f, 148.0f));
    cancelKey.setBounds (rect (1123.0f, 84.0f, 1269.0f, 148.0f));
    saveKey.setBounds (rect (1057.0f, 716.0f, 1211.0f, 772.0f));
    noGlobKey.setBounds (rect (999.0f, 716.0f, 1053.0f, 772.0f));
    if (noGlobKey.isVisible())
        saveKey.setBounds (rect (1123.0f, 716.0f, 1269.0f, 772.0f));

    const int keyPitch = X (1080.0f) - X (1024.0f);
    configHeader = rect (1018.0f, 167.0f, 1250.0f, 191.0f);
    for (int i = 0; i < (int) std::size (configKeys); ++i)
    {
        Tile t { Kind::Config, juce::Rectangle<int> (X (1024.0f) + (i % 4) * keyPitch, Y (195.0f) + (i / 4) * pitchY, keyPitch, pitchY), {}, configKeys[i].name };
        t.bits = i == 0 ? kConsoleTile : 0;
        t.icon = configKeys[i].icon;
        t.dropdown = configKeys[i].dropdown;
        tiles.push_back (t);
    }
    configBox = juce::Rectangle<int>::leftTopRightBottom (configHeader.getX(), configHeader.getBottom(), configHeader.getRight(), Y (195.0f) + 3 * pitchY + juce::roundToInt (4.0f * sy));
    contentsHeader = rect (1018.0f, 384.0f, 1250.0f, 408.0f);
    for (int i = 0; i < (int) std::size (contentKeys); ++i)
    {
        Tile t { Kind::Content, juce::Rectangle<int> (X (1024.0f) + (i % 4) * keyPitch, Y (412.0f) + (i / 4) * pitchY, keyPitch, pitchY), {}, contentKeys[i].name };
        t.bits = contentKeys[i].bits;
        t.dropdown = contentKeys[i].dropdown;
        t.icon = contentKeys[i].icon;
        tiles.push_back (t);
    }
    contentsBox = juce::Rectangle<int>::leftTopRightBottom (contentsHeader.getX(), contentsHeader.getBottom(), contentsHeader.getRight(), Y (412.0f) + 5 * pitchY + juce::roundToInt (4.0f * sy));
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
