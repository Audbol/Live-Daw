#include "ui/ViewPages.h"
#include "dsp/DspUtils.h"
#include "engine/Models.h"

#include <array>

namespace livedaw::ui
{

namespace
{
    constexpr Screen viewScreens[] = { Screen::Overview, Screen::Faders, Screen::Sends, Screen::Config, Screen::MuteGroups };

    const juce::Colour blockFill = Colours::screenTile, blockOutline = Colours::screenLine, chipOff = Colours::chipOff, track = Colours::screenDark,
                       eqCurve = Colours::amber,
        panGreen = Colours::width, sendRed = Colours::tapRed;

    Icon viewIcon (Screen s)
    {
        switch (s)
        {
            case Screen::Overview:   return Icon::Overview;
            case Screen::Faders:     return Icon::Faders;
            case Screen::Sends:      return Icon::Sends;
            case Screen::Config:     return Icon::LayersGear;
            case Screen::MuteGroups: return Icon::MuteGroup;
            case Screen::Home:
            case Screen::Effects:
            case Screen::Meters:
            case Screen::Routing:
            case Screen::Setup:
            case Screen::Library:
            case Screen::Utility:
            case Screen::Recorder:
                break;
        }
        return Icon::Overview;
    }

    /** The console's meter scale: CLIP, -6, -12, -24, -36, -48, -60 evenly spaced. */
    float meterPos (float db)
    {
        static constexpr float marks[] = { -60.0f, -48.0f, -36.0f, -24.0f, -12.0f, -6.0f, 0.0f };
        if (db <= marks[0])
            return 0.0f;
        for (size_t i = 1; i < std::size (marks); ++i)
            if (db <= marks[i])
                return ((float) (i - 1) + (db - marks[i - 1]) / (marks[i] - marks[i - 1])) / 6.0f;
        return 1.0f;
    }

    /** What a fader of a layer is: a strip or a DCA, with its name, colour and label. */
    struct Target
    {
        Strip* strip = nullptr;
        int dca = -1;

        bool isValid() const noexcept { return strip != nullptr || dca >= 0; }
    };

    Target resolve (UiContext& ctx, const state::FaderRef& f)
    {
        if (f.dca)
            return { nullptr, juce::jlimit (0, kNumDcas - 1, f.dcaIndex) };
        if (f.fxParam)
            return {}; // FX parameters have no strip view here
        return { state::findStrip (ctx.engine, f.strip), -1 }; // a send: its source
    }

    juce::Colour colourOf (UiContext& ctx, const Target& t)
    {
        return t.strip != nullptr ? stripDisplayColour (*t.strip) : wingColour (ctx.engine.getDca (t.dca).colour.load());
    }

    /** The name shown in the name bar (empty for default names, as on the console). */
    juce::String barName (UiContext& ctx, const Target& t)
    {
        if (t.strip != nullptr)
            return stripName (*t.strip) != t.strip->getLabel() ? stripName (*t.strip).toUpperCase() : juce::String();
        const auto name = ctx.engine.getDcaName (t.dca);
        return name.removeCharacters (" .").equalsIgnoreCase ("DCA" + juce::String (t.dca + 1)) ? juce::String() : name.toUpperCase();
    }

    juce::String numberOf (const Target& t)
    {
        return t.strip != nullptr ? shortStripLabel (*t.strip) : "D" + juce::String (t.dca + 1);
    }

    std::atomic<float>& faderOf (UiContext& ctx, const Target& t)
    {
        return t.strip != nullptr ? t.strip->params.faderDb : ctx.engine.getDca (t.dca).faderDb;
    }

    std::atomic<bool>& muteOf (UiContext& ctx, const Target& t)
    {
        return t.strip != nullptr ? t.strip->params.mute : ctx.engine.getDca (t.dca).mute;
    }

    void paintNameBar (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour colour, const juce::String& name)
    {
        g.setColour (colour);
        g.fillRect (r);
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (juce::jlimit (8.0f, 15.0f, r.getHeight() * 0.86f), juce::Font::bold));
        g.drawFittedText (name, r.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.6f);
    }

    void paintNumberTile (juce::Graphics& g, juce::Rectangle<float> r, const Target& t)
    {
        g.setColour (blockFill);
        g.fillRect (r);
        auto inner = r.reduced (3.0f, 2.0f);
        g.setColour (Dark::textDim);
        g.setFont (juce::FontOptions (juce::jlimit (9.0f, 22.0f, inner.getHeight() * 0.9f), juce::Font::bold));
        const auto number = numberOf (t);
        g.drawFittedText (number, inner.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
        if (t.strip != nullptr && inner.getWidth() > inner.getHeight() * 2.2f)
            drawStripIcon (g, *t.strip, inner.removeFromRight (inner.getHeight()).reduced (1.0f), Dark::textDim);
    }

    void block (juce::Graphics& g, juce::Rectangle<float> r)
    {
        g.setColour (blockFill);
        g.fillRect (r);
        g.setColour (blockOutline);
        g.drawRect (r, 1.0f);
    }

    void miniChip (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, bool on, juce::Colour onColour)
    {
        g.setColour (on ? onColour : chipOff);
        g.fillRect (r);
        g.setColour (on ? juce::Colours::black : Dark::textDim);
        g.setFont (juce::FontOptions (juce::jlimit (6.0f, 13.0f, juce::jmin (r.getHeight() * 0.8f, r.getWidth() * 0.36f)), juce::Font::bold));
        g.drawFittedText (text, r.toNearestInt(), juce::Justification::centred, 1, 0.6f);
    }

    void verticalMeter (juce::Graphics& g, juce::Rectangle<float> r, float level, juce::Colour c)
    {
        const int segments = juce::jlimit (4, 20, (int) (r.getHeight() / 3.0f));
        const float h = r.getHeight() / (float) segments;
        for (int i = 0; i < segments; ++i)
        {
            const bool lit = level > ((float) i + 0.5f) / (float) segments;
            g.setColour (lit ? c : Colours::screenDark);
            g.fillRect (r.getX(), r.getBottom() - (float) (i + 1) * h + 0.5f, r.getWidth(), juce::jmax (1.0f, h - 1.0f));
        }
    }

    void levelBar (juce::Graphics& g, juce::Rectangle<float> r, float db, bool muted)
    {
        g.setColour (muted ? Dark::muted : track);
        g.fillRect (r);
        if (! muted)
        {
            g.setColour (Dark::faderFill);
            g.fillRect (r.withWidth (r.getWidth() * fader::dbToPos (db)));
        }
    }

    /** The pan display: a green arc centred on the pan position, as wide as the stereo width. */
    void panArc (juce::Graphics& g, juce::Rectangle<float> r, float pan, float width)
    {
        const float s = juce::jmin (r.getWidth(), r.getHeight());
        auto k = r.withSizeKeepingCentre (s, s).reduced (s * 0.08f);
        const float rad = k.getWidth() * 0.4f, thick = juce::jmax (2.0f, k.getWidth() * 0.18f);
        const float limit = juce::MathConstants<float>::pi * 0.75f;
        const float centre = juce::jlimit (-1.0f, 1.0f, pan / 100.0f) * limit;
        const float half = juce::jlimit (0.12f, limit, std::abs (width) / 100.0f * juce::MathConstants<float>::halfPi);
        juce::Path bg, arc;
        bg.addCentredArc (k.getCentreX(), k.getCentreY(), rad, rad, 0.0f, -limit, limit, true);
        arc.addCentredArc (k.getCentreX(), k.getCentreY(), rad, rad, 0.0f, juce::jmax (-limit, centre - half), juce::jmin (limit, centre + half), true);
        g.setColour (juce::Colours::black);
        g.strokePath (bg, juce::PathStrokeType (thick));
        g.setColour (panGreen);
        g.strokePath (arc, juce::PathStrokeType (thick));
        g.setColour (Dark::text);
        const auto c = k.getCentre();
        g.drawLine (c.x, c.y, c.x + std::sin (centre) * rad, c.y - std::cos (centre) * rad, 1.5f);
    }

    /** Mini send levels: one cell per destination, filled to the level when the send is on. */
    void sendCells (juce::Graphics& g, juce::Rectangle<float> r, UiContext& ctx, Strip& s)
    {
        std::vector<std::pair<bool, float>> sends;
        const auto kind = s.getKind();
        if (s.isInput() || (s.isBus() && s.getId() >= kBusToBusTargets))
        {
            const int n = s.isInput() ? juce::jmin (16, (int) ctx.engine.getBuses().size()) : kBusToBusTargets;
            for (int i = 0; i < n; ++i)
            {
                auto* send = s.sends.find (i);
                sends.push_back ({ send != nullptr && send->enabled.load(), send != nullptr ? send->levelDb.load() : -100.0f });
            }
        }
        else if (kind == StripKind::Bus || kind == StripKind::Main)
            for (auto& m : s.params.matrix)
                sends.push_back ({ m.on.load(), m.levelDb.load() });
        if (sends.empty())
            return;
        const int cols = 4, rows = ((int) sends.size() + cols - 1) / cols;
        const float w = r.getWidth() / (float) cols, h = r.getHeight() / (float) rows;
        for (size_t i = 0; i < sends.size(); ++i)
        {
            auto c = juce::Rectangle<float> (r.getX() + w * (float) ((int) i % cols), r.getY() + h * (float) ((int) i / cols), w, h).reduced (1.0f);
            g.setColour (Colours::screenTile);
            g.fillRect (c);
            if (sends[i].first)
            {
                g.setColour (Colours::meterGreen.withAlpha (0.85f));
                g.fillRect (c.withTrimmedTop (c.getHeight() * (1.0f - fader::dbToPos (sends[i].second))));
            }
        }
    }

    /** A screen fader: dark track, blue (or red) fill to the cap, a white value box at the cap. */
    void paintFader (juce::Graphics& g, juce::Rectangle<float> r, float db, juce::Colour fill, bool active)
    {
        const float boxH = juce::jlimit (12.0f, 18.0f, r.getWidth() * 0.26f);
        auto lane = r.reduced (0.0f, boxH * 0.5f);
        const float trackW = juce::jmax (4.0f, r.getWidth() * 0.2f);
        auto trk = lane.withSizeKeepingCentre (trackW, lane.getHeight());
        g.setColour (juce::Colours::black);
        g.fillRect (trk);
        const float pos = fader::dbToPos (db);
        const float y = lane.getBottom() - lane.getHeight() * pos;
        if (active)
        {
            g.setColour (fill);
            g.fillRect (trk.withTop (y));
        }
        // The unity mark.
        g.setColour (Dark::faderFill);
        const float unity = lane.getBottom() - lane.getHeight() * fader::dbToPos (0.0f);
        g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre ({ trk.getRight() + 6.0f, unity }));
        auto box = juce::Rectangle<float> (juce::jmin (r.getWidth(), 64.0f), boxH).withCentre ({ r.getCentreX(), y });
        g.setColour (active ? juce::Colours::white : Colours::textDim);
        g.fillRoundedRectangle (box, 2.0f);
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (juce::jmin (12.0f, boxH * 0.8f), juce::Font::bold));
        g.drawFittedText (fader::text (db) + " dB", box.toNearestInt(), juce::Justification::centred, 1, 0.6f);
    }

    /** Vertical fader drag with the TOUCH FADER RES of SETUP > GENERAL: NORM,
        FINE (a quarter of the speed), or AUTO (FINE once the pointer leaves the strip sideways). */
    float dragScale (UiContext& ctx, juce::Rectangle<int> column, juce::Point<int> pos)
    {
        switch (prefs::faderResolution (ctx))
        {
            case 0: return 1.0f;
            case 1: return 0.25f;
            default: return pos.x < column.getX() || pos.x > column.getRight() ? 0.25f : 1.0f;
        }
    }

    /** A DCA's name and colour: a list (NAME, then the 18 colours) under `anchor`. */
    void editDca (UiContext& ctx, int dca, juce::Component& page, juce::Component& anchor)
    {
        juce::StringArray items { "NAME" };
        for (int k = 1; k <= 18; ++k)
            items.add (wingColourName (k).toUpperCase());
        popups::ListOptions options;
        options.caption = "DCA " + juce::String (dca + 1);
        juce::Component::SafePointer<juce::Component> safePage (&page);
        popups::chooseFrom (anchor, items, ctx.engine.getDca (dca).colour.load(), [&ctx, dca, safePage] (int r)
        {
            if (r >= 1 && r <= 18)
                ctx.engine.getDca (dca).colour = r;
            else if (r == 0 && safePage != nullptr)
            {
                // DCAs named MON A, MON B or MON LVL control the monitor level (firmware 1.09).
                popups::TextOptions text;
                text.suggestions = { "MON A", "MON B", "MON LVL" };
                popups::editText (*safePage, "DCA " + juce::String (dca + 1) + " NAME", ctx.engine.getDcaName (dca),
                                  [&ctx, dca] (const juce::String& name) { ctx.engine.setDcaName (dca, name.trim()); }, text);
            }
        }, options);
    }
} // namespace

const char* viewScreenName (Screen s)
{
    switch (s)
    {
        case Screen::Overview:   return "OVERVIEW";
        case Screen::Faders:     return "FADERS";
        case Screen::Sends:      return "SENDS";
        case Screen::Config:     return "CONFIG";
        case Screen::MuteGroups: return "MUTEGRP";
        case Screen::Home:
        case Screen::Effects:
        case Screen::Meters:
        case Screen::Routing:
        case Screen::Setup:
        case Screen::Library:
        case Screen::Utility:
        case Screen::Recorder:
            break;
    }
    return "";
}

//==============================================================================
/** The layer column of the VIEW screens: the section's seven layer keys (and
    any further user layers) on the right with LEFT / CENTER / RIGHT under them;
    on the left, the pages of the layer shown ("1-8", "9-16", ...), with the
    page keys at the bottom. */
class ViewPage::LayerColumn final : public juce::Component
{
public:
    explicit LayerColumn (ViewPage& p) : page (p) {}

    void rebuild()
    {
        keys.clear();
        pageKeys.clear();
        auto b = getLocalBounds().reduced (3);
        auto pager = b.removeFromBottom (juce::jlimit (20, 34, getHeight() / 14));
        prevKey = pager.removeFromLeft (pager.getWidth() / 2).reduced (2, 0);
        nextKey = pager.reduced (2, 0);
        b.removeFromBottom (6);

        auto& layers = page.ctx.show.layers;
        std::vector<Layer> right = layers.getDefaultLayers (page.ctx.engine, page.section());
        const auto& user = layers.getUserLayers();
        for (size_t i = 2; i < user.size(); ++i)
            right.push_back (user[i]); // user layers without a USER key

        const int numPages = page.numPages();
        const int rowsNeeded = juce::jmax (numPages, (int) right.size() + 4);
        const int keyH = juce::jlimit (16, 40, b.getHeight() / juce::jmax (1, rowsNeeded));
        const int colW = (b.getWidth() - 4) / 2;
        auto lc = b.removeFromLeft (colW);
        b.removeFromLeft (4);
        auto rc = b;
        for (int i = 2; i >= 0; --i)
            sectionKeys[i] = rc.removeFromBottom (keyH).reduced (0, 2);
        rc.removeFromBottom (keyH / 2);
        for (auto& l : right)
            if (rc.getHeight() >= keyH)
                keys.push_back ({ rc.removeFromTop (keyH).reduced (0, 2), l.id, l.user ? l.name.toUpperCase() : LayerSet::keyLabel (l) });

        // The pages of the layer, labelled by their first and last fader.
        const auto layer = page.currentLayer();
        const int size = page.pageSize();
        for (int i = 0; i < numPages && lc.getHeight() >= keyH; ++i)
        {
            const int first = i * size, last = juce::jmin ((int) layer.faders.size(), first + size) - 1;
            juce::String label = juce::String (i + 1);
            if (last >= first)
            {
                const auto a = resolve (page.ctx, layer.faders[(size_t) first]), z = resolve (page.ctx, layer.faders[(size_t) last]);
                if (a.isValid() && z.isValid())
                    label = first == last ? numberOf (a) : numberOf (a) + "-" + numberOf (z);
            }
            pageKeys.push_back ({ lc.removeFromTop (keyH).reduced (0, 2), juce::String (i), label });
        }
        shownLayer = layer.id;
        repaint();
    }

    /** The layer shown changed elsewhere (the section's layer keys). */
    void followLayer()
    {
        if (page.currentLayerId() != shownLayer)
            rebuild();
    }

    void resized() override { rebuild(); }

    void paint (juce::Graphics& g) override
    {
        g.setColour (Colours::screenDark);
        g.fillRect (getLocalBounds());
        const auto current = page.currentLayerId();
        auto key = [&g] (juce::Rectangle<int> r, const juce::String& text, juce::Colour fill, juce::Colour ink)
        {
            g.setColour (fill);
            g.fillRect (r);
            g.setColour (ink);
            const int lines = text.containsChar ('\n') ? 2 : 1;
            g.setFont (juce::FontOptions (juce::jlimit (9.0f, 15.0f, (float) r.getHeight() * (lines == 2 ? 0.4f : 0.55f)), juce::Font::bold));
            g.drawFittedText (text, r.reduced (2, 0), juce::Justification::centred, lines, 0.6f);
        };
        for (auto& k : keys)
        {
            const bool on = k.id == current;
            key (k.area, k.name, on ? Dark::layer : Colours::screenTile, on ? juce::Colours::white : Dark::text);
        }
        for (auto& k : pageKeys)
        {
            const bool on = k.id.getIntValue() == page.page && pageKeys.size() > 1;
            key (k.area, k.name, on ? Dark::layer : Colours::screenPanel, on ? juce::Colours::white : Dark::textDim);
        }
        const char* names[] = { "LEFT", "CENTER", "RIGHT" };
        for (int i = 0; i < 3; ++i)
        {
            const bool on = (int) page.section() == i;
            key (sectionKeys[i], names[i], on ? Dark::section : Colours::screenTile, on ? juce::Colours::black : Dark::text);
        }
        const bool paged = page.numPages() > 1;
        for (auto [r, icon, enabled] : { std::tuple { prevKey, Icon::ChevronLeft, paged && page.page > 0 },
                                         std::tuple { nextKey, Icon::ChevronRight, paged && page.page < page.numPages() - 1 } })
        {
            g.setColour (Colours::screenPanel);
            g.fillRect (r);
            drawIcon (g, icon, r.toFloat().reduced ((float) r.getHeight() * 0.2f), enabled ? Dark::text : Dark::textDim.withAlpha (0.4f));
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const auto pos = e.getPosition();
        for (auto& k : keys)
            if (k.area.contains (pos))
            {
                page.selectLayer (k.id);
                return;
            }
        for (auto& k : pageKeys)
            if (k.area.contains (pos))
            {
                page.stepPage (k.id.getIntValue() - page.page);
                return;
            }
        for (int i = 0; i < 3; ++i)
            if (sectionKeys[i].contains (pos))
            {
                page.selectSection (static_cast<LayerSet::Section> (i));
                return;
            }
        if (prevKey.contains (pos))
            page.stepPage (-1);
        else if (nextKey.contains (pos))
            page.stepPage (1);
    }

private:
    struct Key
    {
        juce::Rectangle<int> area;
        juce::String id, name;
    };

    ViewPage& page;
    std::vector<Key> keys, pageKeys;
    juce::Rectangle<int> sectionKeys[3], prevKey, nextKey;
    juce::String shownLayer;
};

//==============================================================================
ViewPage::ViewPage (UiContext& c, Screen s) : ctx (c), screen (s)
{
    column = std::make_unique<LayerColumn> (*this);
    addChildComponent (*column);
    anchor.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (anchor);
    startTimerHz (15);
}

ViewPage::~ViewPage() = default;

juce::Component& ViewPage::anchorAt (juce::Rectangle<int> area)
{
    anchor.setBounds (area);
    return anchor;
}

std::vector<StatusKey> ViewPage::getStatusKeys()
{
    // The VIEW screens switch between each other from the status bar.
    std::vector<StatusKey> keys;
    for (auto s : viewScreens)
        keys.push_back ({ viewScreenName (s), viewIcon (s), s == screen, [this, s] { if (ctx.showScreen) ctx.showScreen (s); } });
    return keys;
}

StatusTitle ViewPage::getStatusTitle()
{
    // A dark tile with the section letter, the layers icon and the layer [I10].
    StatusTitle t;
    t.kind = StatusTitle::Kind::Layer;
    const char letters[] = { 'L', 'C', 'R' };
    t.prefix = juce::String::charToString ((juce::juce_wchar) letters[juce::jlimit (0, 2, (int) section())]);
    t.text = currentLayer().name.toUpperCase();
    t.icon = Icon::Layers;
    return t;
}

void ViewPage::refresh()
{
    page = juce::jlimit (0, numPages() - 1, page);
    resized();
    repaint();
    notifyStatusKeysChanged();
}

void ViewPage::timerCallback()
{
    if (! isShowing())
        return;
    if (hasLayerColumn())
        column->followLayer();
    // The title tile follows the layer (it may change from the surface).
    if (const auto title = getStatusTitle(); title.prefix + title.text != shownTitle)
    {
        shownTitle = title.prefix + title.text;
        notifyStatusKeysChanged();
    }
    repaint();
}

juce::String ViewPage::currentLayerId() const
{
    const auto s = section();
    if (prefs::screenFollowsLayer (ctx) || browsed[s].isEmpty())
        return ctx.show.layers.getSelectedLayerId (s);
    return browsed[s];
}

Layer ViewPage::currentLayer() const
{
    return ctx.show.layers.findLayer (ctx.engine, section(), currentLayerId());
}

int ViewPage::pageSize() const
{
    return juce::jmax (1, ctx.show.layers.getEffectiveWidth (section()));
}

int ViewPage::numPages() const
{
    const int n = (int) currentLayer().faders.size();
    return juce::jmax (1, (n + pageSize() - 1) / pageSize());
}

std::vector<state::FaderRef> ViewPage::pageFaders() const
{
    const auto layer = currentLayer();
    const int size = pageSize(), first = juce::jlimit (0, numPages() - 1, page) * size;
    std::vector<state::FaderRef> out;
    for (int i = first; i < juce::jmin ((int) layer.faders.size(), first + size); ++i)
        out.push_back (layer.faders[(size_t) i]);
    return out;
}

std::vector<juce::Rectangle<int>> ViewPage::stripColumns (juce::Rectangle<int> area, int count) const
{
    // Strips are a twelfth of the screen wide (a section of 8 leaves room on
    // the left, as on the console); wider sections shrink them to fit.
    count = juce::jmax (1, count);
    const int w = area.getWidth() / juce::jmax (count, hasLayerColumn() ? 10 : count);
    std::vector<juce::Rectangle<int>> out;
    for (int i = 0; i < count; ++i)
        out.push_back (juce::Rectangle<int> (area.getRight() - (count - i) * w, area.getY(), w, area.getHeight()).reduced (2, 0));
    return out;
}

void ViewPage::selectLayer (const juce::String& id)
{
    const auto s = section();
    browsed[s] = id;
    if (prefs::screenFollowsLayer (ctx) && id != ctx.show.layers.getSelectedLayerId (s))
    {
        ctx.show.layers.setSelectedLayerId (s, id);
        notifyLayersChangedAsync (ctx);
    }
    page = 0;
    layerChanged();
    column->rebuild();
    repaint();
}

void ViewPage::selectSection (LayerSet::Section s)
{
    ctx.viewSection = s;
    page = 0;
    resized();
    layerChanged();
    repaint();
}

void ViewPage::stepPage (int delta)
{
    page = juce::jlimit (0, numPages() - 1, page + delta);
    layerChanged();
    column->rebuild();
    repaint();
}

void ViewPage::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
    paintContent (g);
}

void ViewPage::resized()
{
    auto b = getLocalBounds();
    column->setVisible (hasLayerColumn());
    if (hasLayerColumn())
    {
        column->setBounds (b.removeFromRight (juce::jlimit (110, 200, getWidth() * 16 / 100)));
        b.removeFromRight (4);
        column->rebuild();
    }
    content = b;
    layoutContent();
}

//==============================================================================
namespace
{
    /** The rows of an OVERVIEW strip, top to bottom: five processing blocks
        with the level bar among them where the fader sits. */
    struct OverviewRows
    {
        juce::Rectangle<float> name, number, input, blocks[5], bar, pan, sends;
    };

    /** The processing blocks of a strip in its WING order (HomeTab::Home = an
        empty position), and how many come before the fader. */
    struct Blocks
    {
        std::array<HomeTab, 5> tabs { HomeTab::Home, HomeTab::Home, HomeTab::Home, HomeTab::Home, HomeTab::Home };
        int beforeFader = 4;
    };

    Blocks blocksOf (const Strip& s)
    {
        Blocks b;
        if (s.isOutput())
        {
            // Buses, mains, matrices: DYN, INS 1, fader, EQ, INS 2.
            b.tabs = { HomeTab::Home, HomeTab::Dynamics, HomeTab::PreInsert, HomeTab::Eq, HomeTab::PostInsert };
            b.beforeFader = 3;
            return b;
        }
        if (! s.hasSelectableTap())
        {
            // Aux: DYN, EQ, INS 1 before the fader; no INS 2.
            b.tabs = { HomeTab::Home, HomeTab::Dynamics, HomeTab::Eq, HomeTab::PreInsert, HomeTab::Home };
            return b;
        }
        // Input channels: G, E, D, I in their process order, then INS 2.
        const auto order = models::list (models::ProcessOrders)[juce::jlimit (0, 23, (int) s.params.processOrder.load())];
        size_t i = 0;
        for (auto letter : order)
        {
            if (i >= 4)
                break;
            if (letter == 'G')
                b.tabs[i++] = HomeTab::Gate;
            else if (letter == 'E')
                b.tabs[i++] = HomeTab::Eq;
            else if (letter == 'D')
                b.tabs[i++] = HomeTab::Dynamics;
            else if (letter == 'I')
                b.tabs[i++] = HomeTab::PreInsert;
        }
        b.tabs[4] = HomeTab::PostInsert;
        return b;
    }

    OverviewRows overviewRows (juce::Rectangle<int> column, int beforeFader)
    {
        OverviewRows r;
        auto c = column.toFloat();
        const float h = c.getHeight();
        r.name = c.removeFromTop (h * 0.045f);
        c.removeFromTop (2.0f);
        r.number = c.removeFromTop (h * 0.07f);
        c.removeFromTop (4.0f);
        const float barH = juce::jmax (5.0f, h * 0.028f);
        const float gap = 3.0f;
        const float blockH = (c.getHeight() - barH - gap * 9.0f) / 8.0f;
        r.input = c.removeFromTop (blockH);
        for (int i = 0; i < 5; ++i)
        {
            if (i == beforeFader)
            {
                c.removeFromTop (gap);
                r.bar = c.removeFromTop (barH);
            }
            c.removeFromTop (gap);
            r.blocks[i] = c.removeFromTop (blockH);
        }
        if (beforeFader >= 5)
        {
            c.removeFromTop (gap);
            r.bar = c.removeFromTop (barH);
        }
        c.removeFromTop (gap);
        r.pan = c.removeFromTop (blockH);
        c.removeFromTop (gap);
        r.sends = c.removeFromTop (blockH);
        return r;
    }

    void paintInsertBlock (juce::Graphics& g, juce::Rectangle<float> r, UiContext& ctx, Strip& s, bool post)
    {
        block (g, r);
        auto& ins = post ? s.params.postInsert : s.params.preInsert;
        const int slot = ins.fx.load();
        const int mode = post ? ins.mode.load() : 0;
        const bool on = ins.on.load() && (slot >= 0 || mode > 0);
        auto inner = r.reduced (3.0f);
        const juce::String text = mode > 0 ? juce::String ("AUTO") : slot >= 0 ? "FX " + juce::String (slot + 1) : juce::String ("INS");
        miniChip (g, inner.removeFromTop (juce::jmin (inner.getHeight() * 0.5f, 16.0f)).withWidth (inner.getWidth() * 0.62f), text, on, Dark::on);
        juce::String device;
        auto& fx = ctx.engine.getFx();
        if (mode > 0)
            device = mode == 1 ? "X" : "Y";
        else if (slot >= 0)
        {
            if (auto* plugin = fx.getPlugin (slot))
                device = plugin->getName().toUpperCase();
            else if (fx.getModel (slot) > ld::kFxExt)
                device = models::model (models::Fx, fx.getModel (slot)).name.toUpperCase();
        }
        g.setColour (Dark::text);
        g.setFont (juce::FontOptions (juce::jlimit (7.0f, 13.0f, inner.getHeight() * 0.7f), juce::Font::bold));
        g.drawFittedText (device, inner.toNearestInt(), juce::Justification::bottomRight, 1, 0.6f);
    }

    void paintDynBlock (juce::Graphics& g, juce::Rectangle<float> r, Strip& s, bool gate)
    {
        block (g, r);
        auto& slot = gate ? s.params.gate : s.params.dynamics;
        auto inner = r.reduced (3.0f);
        const float gr = (gate ? s.meters.gateReductionDb.load() : s.meters.dynReductionDb.load()) / 24.0f;
        verticalMeter (g, inner.removeFromRight (juce::jmax (3.0f, inner.getWidth() * 0.1f)), juce::jlimit (0.0f, 1.0f, gr), Colours::meterYellow);
        inner.removeFromRight (2.0f);
        auto top = inner.removeFromTop (juce::jmin (inner.getHeight() * 0.5f, 16.0f));
        if (gate)
        {
            // Green while the gate is open.
            auto led = top.removeFromRight (top.getHeight()).reduced (top.getHeight() * 0.2f);
            g.setColour (slot.on.load() && ! s.meters.gateActive.load() ? Colours::meterGreen : Colours::screenDark);
            g.fillRect (led);
            top.removeFromRight (2.0f);
        }
        miniChip (g, top.withWidth (juce::jmin (top.getWidth(), inner.getWidth() * 0.75f)), models::model (models::Dynamics, slot.model.load()).token,
                  slot.on.load(), Dark::on);
    }

    void paintEqBlock (juce::Graphics& g, juce::Rectangle<float> r, Strip& s)
    {
        g.setColour (juce::Colours::black);
        g.fillRect (r);
        g.setColour (blockOutline);
        g.drawRect (r, 1.0f);
        std::array<double, 24> freqs {}, db {};
        for (size_t i = 0; i < freqs.size(); ++i)
            freqs[i] = 20.0 * std::pow (1000.0, (double) i / 23.0);
        ld::ld_strip_response (s.getHandle(), 1, freqs.data(), db.data(), (int32_t) freqs.size(), 48000.0);
        juce::Path curve;
        auto inner = r.reduced (2.0f);
        for (size_t i = 0; i < db.size(); ++i)
        {
            const float x = inner.getX() + inner.getWidth() * (float) i / (float) (db.size() - 1);
            const float y = inner.getCentreY() - juce::jlimit (-1.0f, 1.0f, (float) db[i] / 15.0f) * inner.getHeight() * 0.45f;
            if (i == 0)
                curve.startNewSubPath (x, y);
            else
                curve.lineTo (x, y);
        }
        g.setColour (s.params.eq.on.load() ? eqCurve : Dark::textDim.withAlpha (0.5f));
        g.strokePath (curve, juce::PathStrokeType (1.5f));
    }

    void paintInputBlock (juce::Graphics& g, juce::Rectangle<float> r, UiContext& ctx, Strip& s)
    {
        block (g, r);
        auto inner = r.reduced (3.0f);
        const float level = meterPos (dsp::gainToDb (ctx.meters.get (&s, MeterPoint::Trim, 0)));
        verticalMeter (g, inner.removeFromRight (juce::jmax (3.0f, inner.getWidth() * 0.1f)), level, Colours::meterGreen);
        inner.removeFromRight (2.0f);
        auto& p = s.params;
        const bool ch = s.getKind() == StripKind::Channel;
        const std::pair<const char*, bool> chips[] = { { "INV", p.input.invert.load() }, { "LC", ch && p.filter.lowCut.load() },
                                                       { "HC", ch && p.filter.highCut.load() }, { "FLT", ch && p.filter.tool.load() },
                                                       { "DLY", p.input.delayOn.load() }, { p.stereo.load() || s.isOutput() ? "ST" : "M", false } };
        const float w = inner.getWidth() / 3.0f, h = juce::jmin (inner.getHeight() / 2.0f, 16.0f);
        for (int i = 0; i < 6; ++i)
            miniChip (g, juce::Rectangle<float> (inner.getX() + w * (float) (i % 3), inner.getCentreY() - h + h * (float) (i / 3), w, h).reduced (1.0f),
                      chips[i].first, chips[i].second, Dark::choice);
    }

    void paintPanBlock (juce::Graphics& g, juce::Rectangle<float> r, UiContext& ctx, Strip& s)
    {
        block (g, r);
        auto inner = r.reduced (3.0f);
        auto meter = inner.removeFromRight (juce::jmax (6.0f, inner.getWidth() * 0.16f));
        for (int ch = 0; ch < 2; ++ch)
            verticalMeter (g, meter.withWidth (meter.getWidth() / 2.0f - 1.0f).translated ((float) ch * meter.getWidth() / 2.0f, 0.0f),
                           meterPos (dsp::gainToDb (ctx.meters.get (&s, prefs::meterPoint (ctx, s.getKind()), ch))), Colours::meterGreen);
        inner.removeFromRight (2.0f);
        if (s.getKind() != StripKind::Main && s.getKind() != StripKind::Matrix)
        {
            auto mains = inner.removeFromRight (juce::jmax (5.0f, inner.getWidth() * 0.14f));
            const float h = mains.getHeight() / 4.0f;
            for (int i = 0; i < kNumMains; ++i)
            {
                g.setColour (s.params.mains[i].on.load() ? Dark::faderFill : Colours::screenDark);
                g.fillRect (mains.withHeight (h).translated (0.0f, h * (float) i).reduced (0.5f));
            }
            inner.removeFromRight (2.0f);
        }
        panArc (g, inner, s.params.pan.load(), s.params.width.load());
    }
} // namespace

void OverviewViewPage::paintContent (juce::Graphics& g)
{
    const auto faders = pageFaders();
    const auto cols = stripColumns (content, juce::jmax ((int) faders.size(), pageSize()));
    auto* selected = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr;
    for (size_t i = 0; i < faders.size() && i < cols.size(); ++i)
    {
        const auto t = resolve (ctx, faders[i]);
        if (! t.isValid())
            continue;
        const auto blocks = t.strip != nullptr ? blocksOf (*t.strip) : Blocks {};
        const auto rows = overviewRows (cols[i].withTrimmedBottom (6), blocks.beforeFader);
        paintNameBar (g, rows.name, colourOf (ctx, t), barName (ctx, t));
        paintNumberTile (g, rows.number, t);
        levelBar (g, rows.bar, faderOf (ctx, t).load(), muteOf (ctx, t).load());
        if (t.strip == nullptr)
            continue; // a DCA: name, number and level only

        auto& s = *t.strip;
        paintInputBlock (g, rows.input, ctx, s);
        for (size_t k = 0; k < blocks.tabs.size(); ++k)
        {
            switch (blocks.tabs[k])
            {
                case HomeTab::Gate:       paintDynBlock (g, rows.blocks[k], s, true); break;
                case HomeTab::Dynamics:   paintDynBlock (g, rows.blocks[k], s, false); break;
                case HomeTab::Eq:         paintEqBlock (g, rows.blocks[k], s); break;
                case HomeTab::PreInsert:  paintInsertBlock (g, rows.blocks[k], ctx, s, false); break;
                case HomeTab::PostInsert: paintInsertBlock (g, rows.blocks[k], ctx, s, true); break;
                case HomeTab::Home:
                case HomeTab::Icon:
                case HomeTab::Tags:
                case HomeTab::Input:
                case HomeTab::Filter:
                case HomeTab::Main:
                case HomeTab::Sends:
                case HomeTab::Name:
                    break;
            }
        }
        paintPanBlock (g, rows.pan, ctx, s);
        block (g, rows.sends);
        sendCells (g, rows.sends.reduced (3.0f), ctx, s);
        if (&s == selected)
        {
            g.setColour (Dark::on);
            g.fillRect (cols[i].toFloat().withTop ((float) cols[i].getBottom() - 3.0f));
        }
    }
}

void OverviewViewPage::mouseUp (const juce::MouseEvent& e)
{
    const auto faders = pageFaders();
    const auto cols = stripColumns (content, juce::jmax ((int) faders.size(), pageSize()));
    const auto pos = e.position;
    for (size_t i = 0; i < faders.size() && i < cols.size(); ++i)
    {
        if (! cols[i].toFloat().contains (pos))
            continue;
        const auto t = resolve (ctx, faders[i]);
        const auto blocks = t.strip != nullptr ? blocksOf (*t.strip) : Blocks {};
        const auto rows = overviewRows (cols[i].withTrimmedBottom (6), blocks.beforeFader);
        if (t.dca >= 0)
        {
            if (rows.name.contains (pos) || rows.number.contains (pos))
                editDca (ctx, t.dca, *this, anchorAt (cols[i]));
            return;
        }
        if (t.strip == nullptr)
            return;
        auto tab = HomeTab::Home;
        if (rows.input.contains (pos))
            tab = HomeTab::Input;
        for (size_t k = 0; k < blocks.tabs.size(); ++k)
            if (rows.blocks[k].contains (pos))
                tab = blocks.tabs[k];
        if (rows.pan.contains (pos))
            tab = HomeTab::Main;
        else if (rows.sends.contains (pos))
            tab = HomeTab::Sends;
        if (ctx.selectStrip)
            ctx.selectStrip (t.strip);
        if (ctx.showHomeTab)
            ctx.showHomeTab (tab);
        return;
    }
}

//==============================================================================
namespace
{
    struct FaderRows
    {
        juce::Rectangle<float> name, number, pan, gr, mute, fader, assign, label;
    };

    FaderRows faderRows (juce::Rectangle<int> column)
    {
        FaderRows r;
        auto c = column.toFloat();
        const float h = c.getHeight();
        r.name = c.removeFromTop (h * 0.045f);
        c.removeFromTop (2.0f);
        r.number = c.removeFromTop (h * 0.065f);
        c.removeFromTop (4.0f);
        r.pan = c.removeFromTop (h * 0.1f);
        c.removeFromTop (3.0f);
        r.gr = c.removeFromTop (h * 0.055f);
        c.removeFromTop (3.0f);
        r.mute = c.removeFromTop (h * 0.065f);
        c.removeFromTop (4.0f);
        r.label = c.removeFromBottom (h * 0.07f);
        r.assign = c.removeFromBottom (juce::jmax (10.0f, h * 0.035f));
        c.removeFromBottom (3.0f);
        r.fader = c;
        return r;
    }

    void paintMuteKey (juce::Graphics& g, juce::Rectangle<float> r, bool muted)
    {
        auto k = r.reduced (r.getWidth() * 0.12f, 1.0f);
        g.setColour (muted ? Dark::muted : chipOff);
        g.fillRect (k);
        g.setColour (muted ? juce::Colours::black : Dark::textDim);
        g.setFont (juce::FontOptions (juce::jlimit (8.0f, 14.0f, k.getHeight() * 0.55f), juce::Font::bold));
        g.drawFittedText ("MUTE", k.toNearestInt(), juce::Justification::centred, 1, 0.6f);
    }

    /** DCA 1-16, MAIN 1-4 and MUTE GROUP 1-8 membership as rows of tiny cells. */
    void paintAssignments (juce::Graphics& g, juce::Rectangle<float> r, Strip& s)
    {
        const uint32_t dca = s.params.dcaMask.load(), mute = s.params.muteGroupMask.load();
        const float rowH = r.getHeight() / 3.0f;
        auto row = [&g, &r, rowH] (int index, int count, auto isOn, juce::Colour c)
        {
            const float w = r.getWidth() / 16.0f;
            for (int i = 0; i < count; ++i)
            {
                g.setColour (isOn (i) ? c : Colours::screenPanel);
                g.fillRect (juce::Rectangle<float> (r.getX() + w * (float) i, r.getY() + rowH * (float) index, w, rowH).reduced (0.5f));
            }
        };
        row (0, kNumDcas, [dca] (int i) { return (dca >> i) & 1u; }, Colours::cyan);
        row (1, kNumMains, [&s] (int i) { return s.params.mains[i].on.load(); }, Dark::faderFill);
        row (2, kNumMuteGroups, [mute] (int i) { return (mute >> i) & 1u; }, Dark::muteGroup);
    }

    juce::String bottomLabel (const Target& t)
    {
        return t.strip != nullptr ? t.strip->getLabel() : "DCA " + juce::String (t.dca + 1);
    }
} // namespace

void FadersViewPage::paintContent (juce::Graphics& g)
{
    const auto faders = pageFaders();
    const auto cols = stripColumns (content, juce::jmax ((int) faders.size(), pageSize()));
    auto* selected = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr;
    for (size_t i = 0; i < faders.size() && i < cols.size(); ++i)
    {
        const auto t = resolve (ctx, faders[i]);
        if (! t.isValid())
            continue;
        const auto rows = faderRows (cols[i].withTrimmedBottom (6));
        g.setColour (Colours::screenDark);
        g.fillRect (rows.mute.getUnion (rows.label));
        paintNameBar (g, rows.name, colourOf (ctx, t), barName (ctx, t));
        paintNumberTile (g, rows.number, t);
        paintMuteKey (g, rows.mute, muteOf (ctx, t).load());

        auto faderArea = rows.fader.reduced (2.0f, 0.0f);
        if (t.strip != nullptr)
        {
            auto& s = *t.strip;
            block (g, rows.pan);
            auto pan = rows.pan.reduced (3.0f);
            panArc (g, pan.removeFromLeft (pan.getHeight()), s.params.pan.load(), s.params.width.load());
            sendCells (g, pan.reduced (2.0f, 1.0f), ctx, s);
            if (s.getKind() == StripKind::Channel)
            {
                // GATE and COMP gain reduction: the gate's square lights green while it's open.
                auto gr = rows.gr;
                for (int k = 0; k < 2; ++k)
                {
                    auto bar = gr.removeFromTop (gr.getHeight() / (k == 0 ? 2.0f : 1.0f)).reduced (0.0f, 1.0f);
                    auto& slot = k == 0 ? s.params.gate : s.params.dynamics;
                    const bool isGate = models::model (models::Dynamics, slot.model.load()).token == "GATE";
                    auto square = bar.removeFromLeft (bar.getHeight());
                    if (k == 0 && isGate)
                    {
                        g.setColour (slot.on.load() && ! s.meters.gateActive.load() ? Colours::meterGreen : Colours::screenPanel);
                        g.fillRect (square.reduced (1.0f));
                    }
                    bar.removeFromLeft (2.0f);
                    g.setColour (track);
                    g.fillRect (bar);
                    const float reduction = (k == 0 ? s.meters.gateReductionDb.load() : s.meters.dynReductionDb.load()) / 24.0f;
                    g.setColour (k == 0 && ! isGate ? Colours::meterYellow : Colours::meterGreen);
                    g.fillRect (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, reduction)));
                }
            }
            // The level meter right of the fader.
            auto meter = faderArea.removeFromRight (juce::jmax (5.0f, faderArea.getWidth() * 0.12f));
            for (int ch = 0; ch < 2; ++ch)
                verticalMeter (g, meter.withWidth (meter.getWidth() / 2.0f - 0.5f).translated ((float) ch * meter.getWidth() / 2.0f, 0.0f),
                               meterPos (dsp::gainToDb (ctx.meters.get (&s, prefs::meterPoint (ctx, s.getKind()), ch))), Colours::meterGreen);
            paintAssignments (g, rows.assign.reduced (3.0f, 0.0f), s);
        }
        paintFader (g, faderArea, faderOf (ctx, t).load(), Dark::faderFill, true);
        g.setColour (Dark::textDim);
        g.setFont (juce::FontOptions (juce::jlimit (9.0f, 18.0f, rows.label.getHeight() * 0.7f), juce::Font::bold));
        g.drawFittedText (bottomLabel (t), rows.label.toNearestInt(), juce::Justification::centred, 1, 0.6f);
        if (t.strip != nullptr && t.strip == selected)
        {
            g.setColour (Dark::on);
            g.fillRect (cols[i].toFloat().withTop ((float) cols[i].getBottom() - 3.0f));
        }
    }
}

std::atomic<float>* FadersViewPage::faderAt (juce::Point<int> pos, juce::Rectangle<int>* columnOut) const
{
    const auto faders = pageFaders();
    const auto cols = stripColumns (content, juce::jmax ((int) faders.size(), pageSize()));
    for (size_t i = 0; i < faders.size() && i < cols.size(); ++i)
        if (cols[i].contains (pos))
        {
            const auto t = resolve (ctx, faders[i]);
            if (! t.isValid() || ! faderRows (cols[i].withTrimmedBottom (6)).fader.contains (pos.toFloat()))
                return nullptr;
            if (columnOut != nullptr)
                *columnOut = cols[i];
            return &faderOf (ctx, t);
        }
    return nullptr;
}

void FadersViewPage::mouseDown (const juce::MouseEvent& e)
{
    dragging = faderAt (e.getPosition(), &dragColumn);
    if (dragging != nullptr)
    {
        dragPos = fader::dbToPos (dragging->load());
        lastY = e.getPosition().y;
        // TOUCH FADER SELECT: the knob right of the display fine-tunes the touched fader.
        if (ctx.setFineTarget && ctx.settings.getBoolValue (uiprefs::touchFaderSelect, false))
            ctx.setFineTarget (bindFader ("LEVEL", *dragging), this);
    }
}

void FadersViewPage::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging == nullptr)
        return;
    const float h = (float) juce::jmax (40, dragColumn.getHeight() / 2);
    dragPos = juce::jlimit (0.0f, 1.0f, dragPos + (float) (lastY - e.getPosition().y) / h * dragScale (ctx, dragColumn, e.getPosition()));
    lastY = e.getPosition().y;
    dragging->store (fader::posToDb (dragPos));
    repaint();
}

void FadersViewPage::mouseUp (const juce::MouseEvent& e)
{
    const bool dragged = dragging != nullptr && e.mouseWasDraggedSinceMouseDown();
    dragging = nullptr;
    if (dragged)
        return;
    const auto faders = pageFaders();
    const auto cols = stripColumns (content, juce::jmax ((int) faders.size(), pageSize()));
    const auto pos = e.position;
    for (size_t i = 0; i < faders.size() && i < cols.size(); ++i)
    {
        if (! cols[i].toFloat().contains (pos))
            continue;
        const auto t = resolve (ctx, faders[i]);
        const auto rows = faderRows (cols[i].withTrimmedBottom (6));
        if (rows.mute.contains (pos))
        {
            auto& m = muteOf (ctx, t);
            m = ! m.load();
        }
        else if (rows.name.contains (pos) || rows.number.contains (pos))
        {
            if (t.dca >= 0)
                editDca (ctx, t.dca, *this, anchorAt (cols[i]));
            else if (t.strip != nullptr)
            {
                if (ctx.selectStrip)
                    ctx.selectStrip (t.strip);
                if (ctx.showHomeTab)
                    ctx.showHomeTab (HomeTab::Home);
            }
        }
        else if (t.strip != nullptr && rows.pan.contains (pos))
        {
            if (ctx.selectStrip)
                ctx.selectStrip (t.strip);
            if (ctx.showHomeTab)
                ctx.showHomeTab (HomeTab::Main);
        }
        else if (t.strip != nullptr && ctx.selectStrip)
            ctx.selectStrip (t.strip);
        repaint();
        return;
    }
}

void FadersViewPage::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (auto* p = faderAt (e.getPosition()))
        p->store (0.0f);
}

void FadersViewPage::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (auto* p = faderAt (e.getPosition()))
        p->store (fader::posToDb (juce::jlimit (0.0f, 1.0f, fader::dbToPos (p->load()) + (w.deltaY > 0.0f ? 0.01f : -0.01f))));
}

//==============================================================================
std::vector<SendsViewPage::Destination> SendsViewPage::destinations() const
{
    // BUS 1-8, BUS 9-16 (and more banks of 8 for extra buses), MAIN, MATRIX.
    std::vector<Destination> d;
    const int buses = (int) ctx.engine.getBuses().size();
    const int busGroups = (buses + 7) / 8;
    if (group < busGroups)
    {
        for (int i = group * 8; i < juce::jmin (buses, group * 8 + 8); ++i)
            d.push_back ({ StripKind::Bus, i });
    }
    else if (group == busGroups)
        for (int i = 0; i < kNumMains; ++i)
            d.push_back ({ StripKind::Main, i });
    else
        for (int i = 0; i < kNumMatrices; ++i)
            d.push_back ({ StripKind::Matrix, i });
    return d;
}

SendsViewPage::Destination SendsViewPage::destination() const
{
    const auto d = destinations();
    return d.empty() ? Destination {} : d[(size_t) juce::jlimit (0, (int) d.size() - 1, chosen)];
}

Strip* SendsViewPage::destinationStrip() const
{
    const auto d = destination();
    auto& list = ctx.engine.getStrips (d.kind);
    return juce::isPositiveAndBelow (d.index, (int) list.size()) ? list[(size_t) d.index].get() : nullptr;
}

bool SendsViewPage::canSend (const Strip& s) const
{
    auto* dest = destinationStrip();
    if (dest == nullptr)
        return false;
    switch (dest->getKind())
    {
        // Inputs feed every bus, buses 9 and up feed buses 1-8.
        case StripKind::Bus:    return s.isInput() || (s.isBus() && s.getId() >= kBusToBusTargets && dest->getId() < kBusToBusTargets);
        case StripKind::Main:   return s.isInput() || s.isBus();
        case StripKind::Matrix: return s.getKind() != StripKind::Matrix;
        case StripKind::Channel:
        case StripKind::Aux:
            break;
    }
    return false;
}

std::atomic<float>* SendsViewPage::sendLevel (Strip& s, bool create) const
{
    if (! canSend (s))
        return nullptr;
    auto* dest = destinationStrip();
    const auto d = destination();
    if (d.kind == StripKind::Bus)
    {
        auto* send = create ? &s.sends[(size_t) dest->getId()] : s.sends.find (dest->getId());
        return send != nullptr ? &send->levelDb : nullptr;
    }
    return d.kind == StripKind::Main ? &s.params.mains[d.index].levelDb : &s.params.matrix[d.index].levelDb;
}

std::atomic<bool>* SendsViewPage::sendOn (Strip& s, bool create) const
{
    if (! canSend (s))
        return nullptr;
    auto* dest = destinationStrip();
    const auto d = destination();
    if (d.kind == StripKind::Bus)
    {
        auto* send = create ? &s.sends[(size_t) dest->getId()] : s.sends.find (dest->getId());
        return send != nullptr ? &send->enabled : nullptr;
    }
    return d.kind == StripKind::Main ? &s.params.mains[d.index].on : &s.params.matrix[d.index].on;
}

void SendsViewPage::layoutContent()
{
    auto b = content;
    auto top = b.removeFromTop (juce::jlimit (40, 80, getHeight() * 15 / 100));
    b.removeFromTop (6);
    strips = b;
    groupKeys.clear();
    destTiles.clear();
    const int buses = (int) ctx.engine.getBuses().size();
    const int groups = (buses + 7) / 8 + 2;
    auto keys = top.removeFromLeft (top.getWidth() * 34 / 100);
    keys = keys.withSizeKeepingCentre (keys.getWidth(), juce::jmin (keys.getHeight(), 40));
    const int kw = keys.getWidth() / juce::jmax (4, juce::jmin (groups, 6));
    for (int i = 0; i < groups && i < 6; ++i)
        groupKeys.push_back (keys.removeFromLeft (kw).reduced (2, 0));
    top.removeFromLeft (8);
    const int tw = top.getWidth() / 8;
    for (int i = 0; i < 8; ++i)
        destTiles.push_back (top.removeFromLeft (tw).reduced (2, 0));
}

void SendsViewPage::paintContent (juce::Graphics& g)
{
    // Destination layer keys.
    const int buses = (int) ctx.engine.getBuses().size();
    const int busGroups = (buses + 7) / 8;
    for (size_t i = 0; i < groupKeys.size(); ++i)
    {
        const int gi = (int) i;
        const juce::String name = gi < busGroups ? "BUS " + juce::String (gi * 8 + 1) + "-" + juce::String (juce::jmin (buses, gi * 8 + 8))
                                                 : gi == busGroups ? juce::String ("MAIN") : juce::String ("MATRIX");
        const bool on = gi == group;
        g.setColour (on ? Dark::layer : Colours::screenTile);
        g.fillRect (groupKeys[i]);
        g.setColour (on ? juce::Colours::white : Dark::text);
        g.setFont (juce::FontOptions (juce::jlimit (9.0f, 15.0f, (float) groupKeys[i].getHeight() * 0.4f), juce::Font::bold));
        g.drawFittedText (name, groupKeys[i].reduced (2, 0), juce::Justification::centred, 2, 0.6f);
    }

    // Destination tiles: number header in the destination's colour, icon, name.
    const auto dests = destinations();
    for (size_t i = 0; i < destTiles.size(); ++i)
    {
        auto r = destTiles[i].toFloat();
        if (i >= dests.size())
            continue;
        auto& list = ctx.engine.getStrips (dests[i].kind);
        if (! juce::isPositiveAndBelow (dests[i].index, (int) list.size()))
            continue;
        auto& s = *list[(size_t) dests[i].index];
        auto header = r.removeFromTop (r.getHeight() * 0.28f);
        g.setColour (stripDisplayColour (s));
        g.fillRect (header);
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (juce::jlimit (8.0f, 13.0f, header.getHeight() * 0.85f), juce::Font::bold));
        g.drawFittedText (shortStripLabel (s), header.toNearestInt(), juce::Justification::centred, 1);
        g.setColour (blockFill);
        g.fillRect (r);
        auto name = r.removeFromBottom (r.getHeight() * 0.36f);
        drawStripIcon (g, s, r.reduced (r.getHeight() * 0.15f), Dark::textDim);
        g.setColour (Dark::text);
        g.setFont (juce::FontOptions (juce::jlimit (8.0f, 12.0f, name.getHeight() * 0.9f), juce::Font::bold));
        g.drawFittedText (stripName (s).toUpperCase(), name.toNearestInt(), juce::Justification::centred, 1, 0.6f);
        if ((int) i == chosen)
        {
            g.setColour (Dark::selectedOutline);
            g.drawRect (destTiles[i].toFloat().expanded (1.0f), 2.5f);
        }
    }

    // The layer's strips as send faders.
    const auto faders = pageFaders();
    const auto cols = stripColumns (strips, juce::jmax ((int) faders.size(), pageSize()));
    for (size_t i = 0; i < faders.size() && i < cols.size(); ++i)
    {
        const auto t = resolve (ctx, faders[i]);
        if (! t.isValid())
            continue;
        auto c = cols[i].withTrimmedBottom (6).toFloat();
        const float h = c.getHeight();
        auto name = c.removeFromTop (h * 0.05f);
        c.removeFromTop (2.0f);
        auto number = c.removeFromTop (h * 0.075f);
        c.removeFromTop (4.0f);
        auto onKey = c.removeFromTop (h * 0.075f);
        c.removeFromTop (4.0f);
        auto label = c.removeFromBottom (h * 0.08f);
        paintNameBar (g, name, colourOf (ctx, t), barName (ctx, t));
        paintNumberTile (g, number, t);
        auto* level = t.strip != nullptr ? sendLevel (*t.strip, false) : nullptr;
        auto* on = t.strip != nullptr ? sendOn (*t.strip, false) : nullptr;
        const bool sends = t.strip != nullptr && canSend (*t.strip);
        const bool isOn = on != nullptr && on->load();
        miniChip (g, onKey.reduced (onKey.getWidth() * 0.12f, 1.0f), "ON", isOn, Dark::on);
        g.setColour (Colours::screenDark);
        g.fillRect (c);
        auto meter = c.removeFromRight (juce::jmax (5.0f, c.getWidth() * 0.12f));
        if (t.strip != nullptr)
            for (int ch = 0; ch < 2; ++ch)
                verticalMeter (g, meter.withWidth (meter.getWidth() / 2.0f - 0.5f).translated ((float) ch * meter.getWidth() / 2.0f, 0.0f),
                               meterPos (dsp::gainToDb (ctx.meters.get (t.strip, prefs::meterPoint (ctx, t.strip->getKind()), ch))), Colours::meterGreen);
        if (sends)
            paintFader (g, c.reduced (2.0f, 2.0f), level != nullptr ? level->load() : -100.0f, sendRed, isOn);
        g.setColour (Dark::textDim);
        g.setFont (juce::FontOptions (juce::jlimit (9.0f, 18.0f, label.getHeight() * 0.7f), juce::Font::bold));
        g.drawFittedText (sends ? "SEND" : "-", label.toNearestInt(), juce::Justification::centred, 1);
    }
}

std::atomic<float>* SendsViewPage::faderAt (juce::Point<int> pos, juce::Rectangle<int>* columnOut) const
{
    const auto faders = pageFaders();
    const auto cols = stripColumns (strips, juce::jmax ((int) faders.size(), pageSize()));
    for (size_t i = 0; i < faders.size() && i < cols.size(); ++i)
        if (cols[i].contains (pos))
        {
            const auto t = resolve (ctx, faders[i]);
            if (t.strip == nullptr)
                return nullptr;
            const auto c = cols[i].withTrimmedBottom (6);
            const int h = c.getHeight();
            if (pos.y < c.getY() + h * 25 / 100 || pos.y > c.getBottom() - h * 8 / 100)
                return nullptr;
            if (columnOut != nullptr)
                *columnOut = cols[i];
            return sendLevel (*t.strip, true);
        }
    return nullptr;
}

void SendsViewPage::mouseDown (const juce::MouseEvent& e)
{
    dragging = faderAt (e.getPosition(), &dragColumn);
    if (dragging != nullptr)
    {
        dragPos = fader::dbToPos (dragging->load());
        lastY = e.getPosition().y;
    }
}

void SendsViewPage::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging == nullptr)
        return;
    const float h = (float) juce::jmax (40, dragColumn.getHeight() / 2);
    dragPos = juce::jlimit (0.0f, 1.0f, dragPos + (float) (lastY - e.getPosition().y) / h * dragScale (ctx, dragColumn, e.getPosition()));
    lastY = e.getPosition().y;
    dragging->store (fader::posToDb (dragPos));
    repaint();
}

void SendsViewPage::mouseUp (const juce::MouseEvent& e)
{
    const bool dragged = dragging != nullptr && e.mouseWasDraggedSinceMouseDown();
    dragging = nullptr;
    if (dragged)
        return;
    const auto pos = e.getPosition();
    for (size_t i = 0; i < groupKeys.size(); ++i)
        if (groupKeys[i].contains (pos))
        {
            group = (int) i;
            chosen = 0;
            repaint();
            return;
        }
    for (size_t i = 0; i < destTiles.size(); ++i)
        if (destTiles[i].contains (pos) && i < destinations().size())
        {
            chosen = (int) i;
            repaint();
            return;
        }
    const auto faders = pageFaders();
    const auto cols = stripColumns (strips, juce::jmax ((int) faders.size(), pageSize()));
    for (size_t i = 0; i < faders.size() && i < cols.size(); ++i)
    {
        if (! cols[i].contains (pos))
            continue;
        const auto t = resolve (ctx, faders[i]);
        if (t.strip == nullptr)
            return;
        const auto c = cols[i].withTrimmedBottom (6);
        const int h = c.getHeight();
        const int onTop = c.getY() + juce::roundToInt ((float) h * 0.125f) + 6;
        if (pos.y >= onTop && pos.y < onTop + juce::roundToInt ((float) h * 0.075f))
        {
            if (auto* on = sendOn (*t.strip, true))
                *on = ! on->load();
        }
        else if (pos.y < onTop && ctx.selectStrip)
            ctx.selectStrip (t.strip);
        repaint();
        return;
    }
}

void SendsViewPage::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (auto* p = faderAt (e.getPosition()))
        p->store (0.0f);
}

//==============================================================================
ConfigViewPage::ConfigViewPage (UiContext& c) : ViewPage (c, Screen::Config)
{
    // SENDS (the input channels' sends to a bus, main or matrix), BUS -> BUS
    // (the buses' sends to one) and FX (a slot's parameters) as faders; each
    // field picks its destination or slot and shows those items. MIDI CC needs
    // MIDI remote control.
    for (int i = 0; i < 4; ++i)
    {
        static const char* captions[] = { "SENDS", "BUS -> BUS", "FX", "MIDI CC" };
        auto* f = fields.add (new DropdownField (captions[i], [this, i] { return fieldText (i); }, [this, i] (DropdownField& field) { chooseField (i, field); }));
        f->setEnabled (i < 3);
        addAndMakeVisible (f);
    }
    for (auto* k : { &clearKey, &renameKey, &newKey, &deleteKey })
    {
        styleIconKey (*k, k != &deleteKey);
        addAndMakeVisible (k);
    }
    newKey.setTooltip ("A new user layer, copied from the layer shown");
    deleteKey.setTooltip ("Delete the user layer, or give an edited layer key its own faders back");
    clearKey.onClick = [this]
    {
        const auto layer = currentLayer();
        popups::confirm (*this, "CLEAR", "Remove every fader from \"" + layer.name + "\"?", "CLEAR", [this, layer] { setLayerFaders (layer, {}); }, true);
    };
    renameKey.onClick = [this]
    {
        const auto layer = currentLayer();
        const auto id = userLayerId (layer, true);
        if (id.isEmpty())
            return;
        popups::TextOptions options;
        options.icon = Icon::Layers;
        juce::Component::SafePointer<ConfigViewPage> safe (this);
        popups::editText (*this, "LAYER NAME", layer.name, [safe, id] (const juce::String& text)
        {
            if (safe == nullptr || text.trim().isEmpty())
                return;
            safe->ctx.show.layers.renameUserLayer (id, text.trim());
            notifyLayersChangedAsync (safe->ctx);
            safe->resized();
            safe->notifyStatusKeysChanged();
        }, options);
    };
    newKey.onClick = [this]
    {
        const auto layer = currentLayer();
        const auto name = layer.user || layer.id.startsWith ("user-") ? "USER " + juce::String ((int) ctx.show.layers.getUserLayers().size() + 1) : layer.name;
        const auto id = ctx.show.layers.addUserLayer (name, layer.faders);
        notifyLayersChangedAsync (ctx);
        selectLayer (id);
        resized();
    };
    deleteKey.onClick = [this]
    {
        const auto layer = currentLayer();
        if (const auto id = userLayerId (layer, false); id.isNotEmpty())
        {
            popups::confirm (*this, "DELETE LAYER", "Delete the user layer \"" + layer.name + "\"?", "DELETE", [this, id]
            {
                ctx.show.layers.removeUserLayer (id);
                const auto defaults = ctx.show.layers.getDefaultLayers (ctx.engine, section());
                if (! defaults.empty())
                    selectLayer (defaults.front().id);
                notifyLayersChangedAsync (ctx);
                resized();
            }, true);
        }
        else if (ctx.show.layers.hasDefaultOverride (section(), layer.id))
        {
            popups::confirm (*this, "RESTORE LAYER", "Give \"" + layer.name + "\" its own faders back?", "RESTORE", [this, id = layer.id]
            {
                ctx.show.layers.clearDefaultOverride (section(), id);
                notifyLayersChangedAsync (ctx);
                resized();
                repaint();
            }, true);
        }
    };
}

void ConfigViewPage::layerChanged()
{
    updateKeys();
}

juce::String ConfigViewPage::userLayerId (const Layer& layer, bool create)
{
    if (layer.user)
        return layer.id;
    if (layer.id != "user-1" && layer.id != "user-2")
        return {};
    // USER 1 / USER 2 show the first two user layers: make them on first use.
    auto& layers = ctx.show.layers;
    const size_t index = layer.id == "user-1" ? 0 : 1;
    while (create && layers.getUserLayers().size() <= index)
        layers.addUserLayer ("USER " + juce::String ((int) layers.getUserLayers().size() + 1));
    const auto& user = layers.getUserLayers();
    return index < user.size() ? user[index].id : juce::String();
}

void ConfigViewPage::setLayerFaders (const Layer& layer, const std::vector<state::FaderRef>& faders)
{
    auto& layers = ctx.show.layers;
    if (const auto id = userLayerId (layer, true); id.isNotEmpty())
    {
        // A user layer: empty it, then add the faders in order.
        if (const auto* current = layers.getUserLayer (id))
            for (const auto old = current->faders; const auto& f : old)
                layers.removeFromUserLayer (id, f);
        for (auto& f : faders)
            layers.addToUserLayer (id, f);
    }
    else
    {
        // A default key keeps an edited copy in the show.
        layers.setDefaultOverride (section(), layer.id, faders);
    }
    notifyLayersChangedAsync (ctx);
    resized(); // the layer column and the page map show the new pages
    repaint();
}

void ConfigViewPage::updateKeys()
{
    const auto layer = currentLayer();
    enableKey (clearKey, ! layer.faders.empty());
    enableKey (renameKey, layer.user || layer.id == "user-1" || layer.id == "user-2");
    enableKey (deleteKey, userLayerId (layer, false).isNotEmpty() || ctx.show.layers.hasDefaultOverride (section(), layer.id));
    deleteKey.setButtonText (! layer.user && ctx.show.layers.hasDefaultOverride (section(), layer.id) ? "RESTORE" : "DELETE");
}

std::vector<state::StripRef> ConfigViewPage::destinations (bool busToBus) const
{
    // Inputs send to every bus, main and matrix; buses 9+ to buses 1-8, every bus to the mains and matrices.
    std::vector<state::StripRef> out;
    for (auto& b : ctx.engine.getStrips (StripKind::Bus))
        if (! busToBus || b->getId() < kBusToBusTargets)
            out.push_back (state::StripRef::of (*b));
    for (auto k : { StripKind::Main, StripKind::Matrix })
        for (auto& s : ctx.engine.getStrips (k))
            out.push_back (state::StripRef::of (*s));
    return out;
}

juce::String ConfigViewPage::fieldText (int field) const
{
    if (field == 2)
        return juce::String (fxSlot + 1);
    if (field == 3)
        return "CH 1";
    const auto dests = destinations (field == 1);
    const int i = field == 0 ? sendDest : busDest;
    if (! juce::isPositiveAndBelow (i, (int) dests.size()))
        return "-";
    auto* s = state::findStrip (ctx.engine, dests[(size_t) i]);
    return s != nullptr ? s->getLabel().toUpperCase() : juce::String ("-");
}

void ConfigViewPage::chooseField (int field, DropdownField& fieldKey)
{
    juce::StringArray items;
    int selected = 0;
    if (field == 2)
    {
        auto& fx = ctx.engine.getFx();
        for (int i = 0; i < kNumFx; ++i)
            items.add ("FX " + juce::String (i + 1) + "  " + (fx.getModel (i) > ld::kFxExt ? models::model (models::Fx, fx.getModel (i)).name.toUpperCase() : juce::String ("-")));
        selected = fxSlot;
    }
    else
    {
        for (auto& r : destinations (field == 1))
            if (auto* s = state::findStrip (ctx.engine, r))
                items.add (describeStrip (*s).toUpperCase());
        selected = field == 0 ? sendDest : busDest;
    }
    popups::ListOptions options;
    options.columns = field == 2 ? 2 : 4;
    options.caption = field == 0 ? "SENDS TO" : field == 1 ? "BUS -> BUS TO" : "FX PARAMETERS OF";
    juce::Component::SafePointer<ConfigViewPage> safe (this);
    popups::chooseFrom (fieldKey, items, selected, [safe, field] (int i)
    {
        if (safe == nullptr)
            return;
        if (field == 0)
            safe->sendDest = i;
        else if (field == 1)
            safe->busDest = i;
        else
            safe->fxSlot = i;
        safe->category = 3 + field;
        safe->selectedItems.clear();
        safe->layoutContent();
        safe->repaint();
    }, options);
}

std::vector<state::FaderRef> ConfigViewPage::sourceItems() const
{
    std::vector<state::FaderRef> items;
    auto add = [this, &items] (StripKind k)
    {
        for (auto& s : ctx.engine.getStrips (k))
            items.push_back (state::FaderRef::of (state::StripRef::of (*s)));
    };
    if (category == 3 || category == 4)
    {
        const auto dests = destinations (category == 4);
        const int i = category == 3 ? sendDest : busDest;
        if (! juce::isPositiveAndBelow (i, (int) dests.size()))
            return items;
        const auto to = dests[(size_t) i];
        if (category == 3)
        {
            for (auto k : { StripKind::Channel, StripKind::Aux })
                for (auto& s : ctx.engine.getStrips (k))
                    items.push_back (state::FaderRef::ofSend (state::StripRef::of (*s), to));
        }
        else
            for (auto& b : ctx.engine.getStrips (StripKind::Bus))
                if (! (to.kind == StripKind::Bus && (b->getId() < kBusToBusTargets || b->getId() == to.id)))
                    items.push_back (state::FaderRef::ofSend (state::StripRef::of (*b), to));
        return items;
    }
    if (category == 5)
    {
        auto& fx = ctx.engine.getFx();
        if (const int model = fx.getModel (fxSlot); model > ld::kFxExt)
        {
            const auto& m = models::model (models::Fx, model);
            for (size_t i = 0; i < m.params.size(); ++i)
                if (m.params[i].isContinuous())
                    items.push_back (state::FaderRef::ofFxParam (fxSlot, (int) i));
        }
        return items;
    }
    if (category == 0)
    {
        add (StripKind::Channel);
        add (StripKind::Aux);
    }
    else if (category == 1)
    {
        add (StripKind::Bus);
        add (StripKind::Main);
        add (StripKind::Matrix);
    }
    else
        for (int i = 0; i < kNumDcas; ++i)
            items.push_back (state::FaderRef::ofDca (i));
    return items;
}

void ConfigViewPage::layoutContent()
{
    auto b = content;
    auto top = b.removeFromTop (juce::jlimit (36, 56, getHeight() / 10));
    b.removeFromTop (6);
    categoryKeys.clear();
    const int kw = juce::jmin (100, top.getWidth() / 11);
    for (int i = 0; i < 3; ++i)
        categoryKeys.push_back (top.removeFromLeft (kw).reduced (2, 2));
    top.removeFromLeft (6);
    deleteKey.setBounds (top.removeFromRight (kw).reduced (2, 2));
    newKey.setBounds (top.removeFromRight (kw * 2 / 3).reduced (2, 2));
    renameKey.setBounds (top.removeFromRight (kw * 3 / 4).reduced (2, 2));
    clearKey.setBounds (top.removeFromRight (kw * 3 / 4).reduced (2, 2));
    top.removeFromRight (6);
    const int fw = top.getWidth() / juce::jmax (1, fields.size());
    for (auto* f : fields)
        f->setBounds (top.removeFromLeft (fw).reduced (2, 2));

    // The page map at the very bottom, the layer's slots above it, the source grid on top.
    mapArea = b.removeFromBottom (juce::jlimit (14, 22, getHeight() / 26));
    b.removeFromBottom (4);
    slotsArea = b.removeFromBottom (juce::jlimit (50, 96, getHeight() * 17 / 100));
    b.removeFromBottom (8);
    sourceArea = b;
    hintArea = slotsArea;

    sourceTiles.clear();
    const auto items = sourceItems();
    int rowsNeeded = 0, firstAux = -1;
    for (size_t i = 0; i < items.size(); ++i)
        if (! items[i].dca && items[i].strip.kind == StripKind::Aux && firstAux < 0)
            firstAux = (int) i;
    // Aux tiles start a new row (fig. 58).
    const int beforeAux = firstAux < 0 ? (int) items.size() : firstAux;
    rowsNeeded = (beforeAux + 7) / 8 + ((int) items.size() - beforeAux + 7) / 8;
    const int tw = sourceArea.getWidth() / 8;
    const int th = juce::jlimit (12, 44, sourceArea.getHeight() / juce::jmax (1, rowsNeeded));
    for (int i = 0; i < (int) items.size(); ++i)
    {
        const int index = i < beforeAux ? i : (beforeAux + 7) / 8 * 8 + (i - beforeAux);
        sourceTiles.push_back (juce::Rectangle<int> (sourceArea.getX() + (index % 8) * tw, sourceArea.getY() + (index / 8) * th, tw, th).reduced (2));
    }

    slotAreas.clear();
    const int n = pageSize();
    auto s = slotsArea.withTrimmedBottom (14);
    const int sw = s.getWidth() / juce::jmax (n, 8);
    for (int i = 0; i < n; ++i)
        slotAreas.push_back (juce::Rectangle<int> (s.getX() + i * sw, s.getY(), sw, s.getHeight()).reduced (3, 4));

    // Every slot of the layer (whole pages, one more page to append to), in groups of four.
    mapCells.clear();
    const int slots = (numPages() + 1) * n;
    const int groups = (slots + 3) / 4;
    const float cell = juce::jmin ((float) mapArea.getHeight(), (float) mapArea.getWidth() / ((float) slots + (float) groups * 0.5f));
    float x = (float) mapArea.getX();
    for (int i = 0; i < slots; ++i)
    {
        if (i > 0 && i % 4 == 0)
            x += cell * 0.5f;
        mapCells.push_back (juce::Rectangle<float> (x, (float) mapArea.getY(), cell, (float) mapArea.getHeight()).reduced (1.0f, 0.0f).toNearestInt());
        x += cell;
    }
    updateKeys();
}

void ConfigViewPage::paintContent (juce::Graphics& g)
{
    // Category keys.
    const char* names[] = { "CHANNELS", "BUS/MAIN", "DCA" };
    for (int i = 0; i < 3; ++i)
    {
        const bool on = i == category;
        g.setColour (on ? Colours::caption : Dark::key);
        g.fillRect (categoryKeys[(size_t) i]);
        g.setColour (on ? juce::Colours::black : Dark::text);
        g.setFont (juce::FontOptions (juce::jlimit (9.0f, 15.0f, (float) categoryKeys[(size_t) i].getHeight() * 0.4f), juce::Font::bold));
        g.drawFittedText (names[i], categoryKeys[(size_t) i].reduced (2, 0), juce::Justification::centred, 1, 0.6f);
    }

    // The active SENDS / BUS -> BUS / FX field is framed.
    if (category >= 3 && category - 3 < fields.size())
    {
        g.setColour (Colours::yellow);
        g.drawRect (fields[category - 3]->getBounds().expanded (2), 2);
    }

    auto tile = [this, &g] (juce::Rectangle<float> r, const state::FaderRef& f, bool highlighted)
    {
        if (f.fxParam)
        {
            // An FX parameter: the slot on an FX-coloured bar, the parameter's name.
            const int model = ctx.engine.getFx().getModel (f.fxSlot);
            const auto& m = models::model (models::Fx, juce::jmax (0, model));
            const auto label = juce::isPositiveAndBelow (f.param, (int) m.params.size()) && model > ld::kFxExt ? m.params[(size_t) f.param].label : juce::String ("?");
            auto bar = r.removeFromTop (r.getHeight() * 0.36f);
            paintNameBar (g, bar, FileColours::fx, "FX " + juce::String (f.fxSlot + 1));
            g.setColour (blockFill);
            g.fillRect (r);
            g.setColour (Dark::text);
            g.setFont (juce::FontOptions (juce::jlimit (8.0f, 14.0f, r.getHeight() * 0.6f), juce::Font::bold));
            g.drawFittedText (label.toUpperCase(), r.reduced (3.0f, 1.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.6f);
            if (highlighted)
            {
                g.setColour (Colours::yellow);
                g.drawRect (bar.getUnion (r).expanded (1.0f), 2.0f);
            }
            return;
        }
        const auto t = resolve (ctx, f);
        if (! t.isValid())
            return;
        auto bar = r.removeFromTop (r.getHeight() * 0.36f);
        paintNameBar (g, bar, colourOf (ctx, t), barName (ctx, t));
        g.setColour (blockFill);
        g.fillRect (r);
        auto inner = r.reduced (3.0f, 1.0f);
        if (f.send)
        {
            // A send level: the source with an "S > B4" mark (fig. 59).
            auto* to = state::findStrip (ctx.engine, f.to);
            const auto mark = "S>" + (to != nullptr ? shortStripLabel (*to) : juce::String ("?"));
            g.setColour (Colours::solo);
            g.setFont (juce::FontOptions (juce::jlimit (7.0f, 13.0f, inner.getHeight() * 0.6f), juce::Font::bold));
            g.drawFittedText (mark, inner.removeFromRight (inner.getWidth() * 0.5f).toNearestInt(), juce::Justification::centredRight, 1, 0.6f);
        }
        g.setColour (Dark::text);
        g.setFont (juce::FontOptions (juce::jlimit (8.0f, 18.0f, inner.getHeight() * 0.85f), juce::Font::bold));
        g.drawFittedText (numberOf (t), inner.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
        if (! f.send && t.strip != nullptr && inner.getWidth() > inner.getHeight() * 2.5f)
            drawStripIcon (g, *t.strip, inner.removeFromRight (inner.getHeight()), Dark::textDim);
        if (highlighted)
        {
            g.setColour (Colours::yellow);
            g.drawRect (bar.getUnion (r).expanded (1.0f), 2.0f);
        }
    };

    // The source grid.
    const auto items = sourceItems();
    for (size_t i = 0; i < items.size() && i < sourceTiles.size(); ++i)
    {
        const bool sel = std::find (selectedItems.begin(), selectedItems.end(), items[i]) != selectedItems.end();
        tile (sourceTiles[i].toFloat(), items[i], sel);
    }

    // The layer's slots, in groups of four framed in blue; an empty slot shows
    // its number large and dim over EMPTY.
    const auto layer = currentLayer();
    const auto faders = pageFaders();
    for (size_t i = 0; i < slotAreas.size(); ++i)
    {
        auto r = slotAreas[i].toFloat();
        g.setColour (Dark::well);
        g.fillRect (r);
        const int number = page * pageSize() + (int) i + 1;
        if (i < faders.size() && ! (dragActive && dragSlot == (int) i))
            tile (r, faders[i], false);
        else
        {
            auto text = r.reduced (2.0f);
            g.setColour (Dark::textDim.withAlpha (0.55f));
            g.setFont (juce::FontOptions (juce::jlimit (10.0f, 30.0f, text.getHeight() * 0.5f), juce::Font::bold));
            g.drawFittedText (juce::String (number), text.removeFromTop (text.getHeight() * 0.62f).toNearestInt(), juce::Justification::centredBottom, 1);
            g.setFont (juce::FontOptions (juce::jlimit (8.0f, 13.0f, text.getHeight() * 0.8f), juce::Font::bold));
            g.drawFittedText ("EMPTY", text.toNearestInt(), juce::Justification::centredTop, 1);
        }
        g.setColour (Dark::textDim);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (juce::String (number), juce::Rectangle<int> (slotAreas[i].getX(), slotAreas[i].getBottom() + 3, slotAreas[i].getWidth(), 12),
                    juce::Justification::centred);
        if (i % 4 == 0)
        {
            const auto last = slotAreas[juce::jmin (slotAreas.size() - 1, i + 3)];
            g.setColour (Dark::layer);
            g.drawRect (slotAreas[i].getUnion (last).expanded (2, 3).toFloat(), 1.5f);
        }
    }

    // While dragging: a blue line where the items would be inserted, or a
    // frame on the slot they'd replace.
    if (dragActive)
        if (const auto drop = dropTargetAt (dragPoint); drop.index >= 0)
        {
            const int local = drop.index - page * pageSize();
            g.setColour (Dark::layer.brighter (0.3f));
            if (drop.insert && local >= 0 && local <= (int) slotAreas.size() && ! slotAreas.empty())
            {
                const int x = local < (int) slotAreas.size() ? slotAreas[(size_t) local].getX() - 3 : slotAreas.back().getRight() + 3;
                g.fillRect (juce::Rectangle<int> (x - 2, slotAreas.front().getY() - 4, 4, slotAreas.front().getHeight() + 8));
            }
            else if (juce::isPositiveAndBelow (local, (int) slotAreas.size()))
                g.drawRect (slotAreas[(size_t) local].expanded (2).toFloat(), 3.0f);
        }

    // The page map: every slot (filled where assigned), the shown window framed blue.
    const int total = (int) layer.faders.size();
    for (size_t i = 0; i < mapCells.size(); ++i)
    {
        g.setColour ((int) i < total ? Dark::textDim : Dark::key);
        g.fillRect (mapCells[i]);
        if (mapCells[i].getWidth() >= 12)
        {
            g.setColour ((int) i < total ? juce::Colours::black : Dark::textDim);
            g.setFont (juce::FontOptions (juce::jmin (10.0f, (float) mapCells[i].getHeight() * 0.7f), juce::Font::bold));
            g.drawFittedText (juce::String ((int) i + 1), mapCells[i], juce::Justification::centred, 1, 0.5f);
        }
    }
    const int first = page * pageSize(), last = juce::jmin ((int) mapCells.size(), first + pageSize()) - 1;
    if (juce::isPositiveAndBelow (first, (int) mapCells.size()) && last >= first)
    {
        g.setColour (Dark::layer.brighter (0.4f));
        g.drawRect (mapCells[(size_t) first].getUnion (mapCells[(size_t) last]).expanded (2, 2).toFloat(), 2.0f);
    }

    if (! layer.user && ctx.show.layers.hasDefaultOverride (section(), layer.id) && faders.empty() && page == 0)
    {
        g.setColour (Dark::text);
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawFittedText ("EDITED LAYER KEY: RESTORE GIVES IT ITS OWN FADERS BACK", hintArea.withTrimmedBottom (14).reduced (8, 0),
                          juce::Justification::centred, 1);
    }

    // The item being dragged.
    if (dragActive)
    {
        const int count = dragSlot >= 0 ? 1 : juce::jmax (1, (int) selectedItems.size());
        auto ghost = juce::Rectangle<float> (70.0f, 34.0f).withCentre (dragPoint.toFloat());
        g.setColour (Dark::layer.withAlpha (0.8f));
        g.fillRect (ghost);
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawText (count > 1 ? juce::String (count) + " ITEMS" : juce::String ("1 ITEM"), ghost, juce::Justification::centred);
    }
}

ConfigViewPage::DropTarget ConfigViewPage::dropTargetAt (juce::Point<int> pos) const
{
    // Near the edge between two slots: insert there; over a slot: replace it.
    for (size_t i = 0; i < slotAreas.size(); ++i)
    {
        const auto r = slotAreas[i].expanded (3, 6);
        if (! r.contains (pos))
            continue;
        const int edge = juce::jmax (6, r.getWidth() / 5);
        const int index = page * pageSize() + (int) i;
        if (pos.x < r.getX() + edge)
            return { index, true };
        if (pos.x > r.getRight() - edge)
            return { index + 1, true };
        return { index, false };
    }
    return {};
}

void ConfigViewPage::placeAt (const std::vector<state::FaderRef>& items, int index, bool insert)
{
    if (items.empty())
        return;
    const auto layer = currentLayer();
    auto faders = layer.faders;
    // Items already in the layer move: take them out first (shifting the target).
    for (auto& f : items)
        if (const auto it = std::find (faders.begin(), faders.end(), f); it != faders.end())
        {
            const int at = (int) (it - faders.begin());
            if (at < index)
                --index;
            else if (at == index && ! insert)
                insert = true; // dropped onto its own slot: put it back there
            faders.erase (it);
        }
    index = juce::jlimit (0, (int) faders.size(), index);
    if (! insert && index < (int) faders.size())
        faders.erase (faders.begin() + index);
    faders.insert (faders.begin() + index, items.begin(), items.end());
    setLayerFaders (layer, faders);
}

void ConfigViewPage::mouseDown (const juce::MouseEvent& e)
{
    dragActive = false;
    dragSlot = -1;
    dragPoint = e.getPosition();
    for (size_t i = 0; i < slotAreas.size() && i < pageFaders().size(); ++i)
        if (slotAreas[i].contains (e.getPosition()))
            dragSlot = (int) i;
}

void ConfigViewPage::mouseDrag (const juce::MouseEvent& e)
{
    if (e.getDistanceFromDragStart() < 6)
        return;
    if (! dragActive)
    {
        // Start dragging the tile under the pointer (with the other selected tiles).
        if (dragSlot < 0)
        {
            const auto items = sourceItems();
            bool onTile = false;
            for (size_t i = 0; i < items.size() && i < sourceTiles.size(); ++i)
                if (sourceTiles[i].contains (e.getMouseDownPosition()))
                {
                    onTile = true;
                    if (std::find (selectedItems.begin(), selectedItems.end(), items[i]) == selectedItems.end())
                        selectedItems.push_back (items[i]);
                }
            if (! onTile)
                return;
        }
        dragActive = true;
    }
    dragPoint = e.getPosition();
    repaint();
}

void ConfigViewPage::mouseUp (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();
    if (dragActive)
    {
        dragActive = false;
        const auto drop = dropTargetAt (pos);
        const auto faders = pageFaders();
        if (dragSlot >= 0 && juce::isPositiveAndBelow (dragSlot, (int) faders.size()))
        {
            const auto layer = currentLayer();
            if (drop.index < 0 && ! slotsArea.contains (pos))
            {
                // Dragged off the layer: remove the fader.
                auto all = layer.faders;
                all.erase (std::remove (all.begin(), all.end(), faders[(size_t) dragSlot]), all.end());
                setLayerFaders (layer, all);
            }
            else if (drop.index >= 0)
                placeAt ({ faders[(size_t) dragSlot] }, drop.index, drop.insert);
        }
        else if (drop.index >= 0)
        {
            placeAt (selectedItems, drop.index, drop.insert);
            selectedItems.clear();
        }
        dragSlot = -1;
        repaint();
        return;
    }

    for (size_t i = 0; i < categoryKeys.size(); ++i)
        if (categoryKeys[i].contains (pos))
        {
            category = (int) i;
            selectedItems.clear();
            layoutContent();
            repaint();
            return;
        }

    // The page map pages the slots.
    for (size_t i = 0; i < mapCells.size(); ++i)
        if (mapCells[i].expanded (1, 4).contains (pos))
        {
            stepPage ((int) i / pageSize() - page);
            resized();
            return;
        }

    // A click on a tile toggles it; a click on a slot places the selected tiles there.
    const auto items = sourceItems();
    for (size_t i = 0; i < items.size() && i < sourceTiles.size(); ++i)
        if (sourceTiles[i].contains (pos))
        {
            auto it = std::find (selectedItems.begin(), selectedItems.end(), items[i]);
            if (it != selectedItems.end())
                selectedItems.erase (it);
            else
                selectedItems.push_back (items[i]);
            repaint();
            return;
        }
    if (const auto drop = dropTargetAt (pos); drop.index >= 0 && ! selectedItems.empty())
    {
        placeAt (selectedItems, drop.index, drop.insert);
        selectedItems.clear();
        repaint();
        return;
    }
    dragSlot = -1;
}

//==============================================================================
MuteGroupsViewPage::MuteGroupsViewPage (UiContext& c) : ViewPage (c, Screen::MuteGroups)
{
    styleIconKey (unassignKey);
    unassignKey.setTooltip ("Clears every assignment of the selected mute group");
    unassignKey.onClick = [this]
    {
        const uint32_t bit = 1u << (uint32_t) selectedGroup;
        for (int k = 0; k < kNumStripKinds; ++k)
            for (auto& s : ctx.engine.getStrips (static_cast<StripKind> (k)))
                s->params.muteGroupMask = s->params.muteGroupMask.load() & ~bit;
        repaint();
    };
    addAndMakeVisible (unassignKey);
}

void MuteGroupsViewPage::layoutContent()
{
    cells.clear();
    captions.clear();
    auto b = content;
    auto left = b.removeFromLeft (b.getWidth() * 63 / 100);
    b.removeFromLeft (8);
    auto right = b;

    const int colW = left.getWidth() / kNumMuteGroups;
    struct GridSpec
    {
        const char* caption;
        StripKind kind;
    };
    const GridSpec grids[] = { { "CHANNEL", StripKind::Channel }, { "AUX", StripKind::Aux }, { "BUS", StripKind::Bus },
                               { "MAIN", StripKind::Main }, { "MATRIX", StripKind::Matrix } };
    int rows = 0;
    for (auto& gs : grids)
        rows += ((int) ctx.engine.getStrips (gs.kind).size() + 3) / 4;
    const int headerH = juce::jlimit (14, 22, left.getHeight() / 26);
    const int numberH = juce::jlimit (16, 34, left.getHeight() / 15);
    const int captionH = juce::jlimit (10, 14, left.getHeight() / 34);
    const int sourcesH = captionH + juce::jlimit (10, 16, left.getHeight() / 30);
    const int available = left.getHeight() - headerH - numberH - 8 - 5 * captionH - sourcesH;
    const int cell = juce::jlimit (4, (colW - 8) / 4, available / juce::jmax (1, rows));

    for (int gi = 0; gi < kNumMuteGroups; ++gi)
    {
        auto col = juce::Rectangle<int> (left.getX() + gi * colW, left.getY(), colW, left.getHeight()).reduced (3, 0);
        columns[gi] = col;
        headers[gi] = col.removeFromTop (headerH);
        col.removeFromTop (2);
        col.removeFromTop (numberH);
        col.removeFromTop (4);
        auto grid = col.reduced (2, 0);
        for (auto& gs : grids)
        {
            captions.push_back ({ grid.removeFromTop (captionH), gs.caption });
            auto& strips = ctx.engine.getStrips (gs.kind);
            const int x0 = grid.getCentreX() - cell * 2;
            for (size_t i = 0; i < strips.size(); ++i)
                cells.push_back ({ juce::Rectangle<int> (x0 + ((int) i % 4) * cell, grid.getY() + ((int) i / 4) * cell, cell, cell), strips[i].get(), gi });
            grid.removeFromTop (((int) strips.size() + 3) / 4 * cell + 1);
        }
        captions.push_back ({ grid.removeFromTop (captionH), "SOURCES" });
    }

    // The group keys: 1-4 in the first column, 5-8 in the second.
    auto keysArea = right;
    unassignKey.setBounds (keysArea.removeFromBottom (juce::jlimit (28, 44, getHeight() / 11)).withTrimmedLeft (keysArea.getWidth() / 2).reduced (4, 2));
    keysArea.removeFromBottom (6);
    const int kw = keysArea.getWidth() / 2, kh = keysArea.getHeight() / 4;
    for (int gi = 0; gi < kNumMuteGroups; ++gi)
        groupKeys[gi] = juce::Rectangle<int> (keysArea.getX() + (gi / 4) * kw, keysArea.getY() + (gi % 4) * kh, kw, kh).reduced (4);
}

void MuteGroupsViewPage::paintContent (juce::Graphics& g)
{
    const uint32_t engaged = ctx.engine.muteGroups.load();
    for (int gi = 0; gi < kNumMuteGroups; ++gi)
    {
        const bool sel = gi == selectedGroup;
        auto col = columns[gi];
        g.setColour (Colours::screenDark);
        g.fillRect (col.withTrimmedTop (headers[gi].getHeight() + 2));
        // Name on the salmon bar, then the number.
        g.setColour (Dark::muteGroup);
        g.fillRect (headers[gi]);
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (juce::jlimit (9.0f, 15.0f, (float) headers[gi].getHeight() * 0.75f), juce::Font::bold));
        g.drawFittedText (ctx.engine.getMuteGroupName (gi).toUpperCase(), headers[gi].reduced (2, 0), juce::Justification::centred, 1, 0.6f);
        auto number = col.withTrimmedTop (headers[gi].getHeight() + 2).withHeight (juce::jlimit (16, 34, columns[gi].getHeight() / 15));
        g.setColour (Colours::screenPanel);
        g.fillRect (number);
        g.setColour (Dark::textDim);
        g.setFont (juce::FontOptions ((float) number.getHeight() * 0.8f, juce::Font::bold));
        g.drawText (juce::String (gi + 1), number, juce::Justification::centred);
        if (sel)
        {
            g.setColour (Dark::muteGroup.withAlpha (0.18f));
            g.fillRect (col.withTop (number.getBottom() + 2));
        }
    }
    for (auto& [area, text] : captions)
    {
        const bool sel = area.getX() >= columns[selectedGroup].getX() && area.getRight() <= columns[selectedGroup].getRight();
        g.setColour (sel ? Dark::muteGroup.withAlpha (0.55f) : Colours::screenPanel);
        g.fillRect (area);
        g.setColour (sel ? juce::Colours::white : Dark::textDim);
        g.setFont (juce::FontOptions ((float) area.getHeight() * 0.85f, juce::Font::bold));
        g.drawText (text, area, juce::Justification::centred);
        if (text == "SOURCES")
        {
            auto box = area.translated (0, area.getHeight() + 1).withHeight (juce::jmax (10, area.getHeight() + 2)).reduced (4, 0);
            g.setColour (juce::Colours::black);
            g.fillRect (box);
            g.setColour (Dark::textDim);
            g.drawText ("-", box, juce::Justification::centred);
        }
    }
    for (auto& c : cells)
    {
        const bool on = (c.strip->params.muteGroupMask.load() >> (uint32_t) c.group) & 1u;
        auto r = c.area.toFloat().reduced (0.5f);
        g.setColour (on ? stripDisplayColour (*c.strip) : Dark::well);
        g.fillRect (r);
        if (r.getHeight() >= 9.0f)
        {
            g.setColour (on ? juce::Colours::black : Colours::chipOff);
            g.setFont (juce::FontOptions (juce::jmin (11.0f, r.getHeight() * 0.7f), juce::Font::bold));
            g.drawFittedText (juce::String (c.strip->getNumber()), r.toNearestInt(), juce::Justification::centred, 1, 0.5f);
        }
    }
    g.setColour (Dark::muteGroup);
    g.drawRect (columns[selectedGroup].toFloat().expanded (1.0f), 2.0f);

    // The group keys: big number, and the key lit salmon while the group is engaged.
    for (int gi = 0; gi < kNumMuteGroups; ++gi)
    {
        auto r = groupKeys[gi];
        g.setColour (Colours::screenPanel);
        g.fillRect (r);
        auto num = r.removeFromTop (r.getHeight() * 40 / 100);
        g.setColour (Dark::textDim);
        g.setFont (juce::FontOptions (juce::jmin (32.0f, (float) num.getHeight() * 0.9f), juce::Font::bold));
        g.drawText (juce::String (gi + 1), num, juce::Justification::centred);
        auto key = r.reduced (r.getWidth() / 5, r.getHeight() / 6);
        const bool on = (engaged >> (uint32_t) gi) & 1u;
        g.setColour (on ? Colours::mute : Colours::screenTile);
        g.fillRect (key);
        g.setColour (on ? juce::Colours::white : Dark::textDim);
        g.setFont (juce::FontOptions (juce::jlimit (10.0f, 16.0f, (float) key.getHeight() * 0.45f), juce::Font::bold));
        g.drawFittedText (ctx.engine.getMuteGroupName (gi).toUpperCase(), key.reduced (2, 0), juce::Justification::centred, 1, 0.6f);
    }
}

void MuteGroupsViewPage::mouseUp (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();
    for (int gi = 0; gi < kNumMuteGroups; ++gi)
    {
        if (groupKeys[gi].contains (pos))
        {
            ctx.engine.muteGroups = ctx.engine.muteGroups.load() ^ (1u << (uint32_t) gi);
            repaint();
            return;
        }
        if (headers[gi].contains (pos))
        {
            // Rename the group.
            selectedGroup = gi;
            popups::TextOptions options;
            options.icon = Icon::MuteGroup;
            juce::Component::SafePointer<MuteGroupsViewPage> safe (this);
            popups::editText (*this, "MUTE GROUP " + juce::String (gi + 1) + " NAME", ctx.engine.getMuteGroupName (gi),
                              [safe, gi] (const juce::String& text)
                              {
                                  if (safe == nullptr)
                                      return;
                                  safe->ctx.engine.setMuteGroupName (gi, text.trim());
                                  safe->repaint();
                              },
                              options);
            repaint();
            return;
        }
    }
    // A cell of the selected group toggles its strip; elsewhere in a column selects that group.
    for (auto& c : cells)
        if (c.area.contains (pos) && c.group == selectedGroup)
        {
            const uint32_t bit = 1u << (uint32_t) c.group;
            c.strip->params.muteGroupMask = c.strip->params.muteGroupMask.load() ^ bit;
            repaint();
            return;
        }
    for (int gi = 0; gi < kNumMuteGroups; ++gi)
        if (columns[gi].contains (pos))
        {
            selectedGroup = gi;
            repaint();
            return;
        }
}

} // namespace livedaw::ui
