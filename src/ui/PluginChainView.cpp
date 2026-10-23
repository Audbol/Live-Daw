#include "ui/PluginChainView.h"
#include "engine/SandboxedPlugin.h"
#include "ui/LookAndFeel.h"

#include <algorithm>
#include <iterator>

namespace livedaw::ui
{

namespace
{
    /** A flat key of the bar and the title rows: a glyph or a word, lit (filled)
        when toggled on, a tile only under the mouse. */
    class Key final : public juce::Button
    {
    public:
        enum class Glyph
        {
            word,
            collapse, // ▾ (expanded) or ▸ (collapsed: the toggle state)
            minus,
            plus,
            remove
        };

        Key (Glyph g, const juce::String& word, const juce::String& tip, juce::Colour litColour = Colours::gold)
            : juce::Button (word), glyph (g), lit (litColour)
        {
            setTooltip (tip);
        }

        Glyph glyph;
        juce::Colour lit;
        juce::Colour ink = Colours::caption;

        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            auto r = getLocalBounds().toFloat().reduced (1.0f);
            const bool on = glyph != Glyph::collapse && getToggleState();
            if (on || over || down)
            {
                auto fill = on ? lit : Colours::screenTile;
                g.setColour (down ? fill.brighter (0.2f) : fill);
                g.fillRoundedRectangle (r, 3.0f);
            }
            const auto colour = (on ? juce::Colours::black : (over ? Colours::text : ink)).withMultipliedAlpha (isEnabled() ? 1.0f : 0.3f);
            g.setColour (colour);

            const auto c = r.getCentre();
            const float s = juce::jmin (r.getWidth(), r.getHeight()) * 0.22f;
            juce::Path p;
            switch (glyph)
            {
                case Glyph::word:
                    g.setFont (wingFontPx (juce::jmin (8.0f, r.getHeight() * 0.42f), WingWeight::SemiBold));
                    g.drawText (getButtonText(), r, juce::Justification::centred);
                    return;
                case Glyph::collapse:
                    if (getToggleState())
                        p.addTriangle (c.x - s * 0.6f, c.y - s, c.x - s * 0.6f, c.y + s, c.x + s * 0.9f, c.y);
                    else
                        p.addTriangle (c.x - s, c.y - s * 0.6f, c.x + s, c.y - s * 0.6f, c.x, c.y + s * 0.9f);
                    g.fillPath (p);
                    return;
                case Glyph::minus:
                case Glyph::plus:
                    g.fillRect (juce::Rectangle<float> (2.0f * s, 1.6f).withCentre (c));
                    if (glyph == Glyph::plus)
                        g.fillRect (juce::Rectangle<float> (1.6f, 2.0f * s).withCentre (c));
                    return;
                case Glyph::remove:
                    p.startNewSubPath (c.x - s, c.y - s);
                    p.lineTo (c.x + s, c.y + s);
                    p.startNewSubPath (c.x + s, c.y - s);
                    p.lineTo (c.x - s, c.y + s);
                    g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                    return;
            }
        }
    };

    juce::Point<int> defaultMaxSize()
    {
        if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            return { juce::roundToInt (display->userBounds.getWidth() * 0.9f), juce::roundToInt (display->userBounds.getHeight() * 0.85f) };
        return { 1600, 1000 };
    }

    /** Editors in native views (HWND, NSView, X11) that a transform can't scale. */
    bool isNativeFormat (const juce::String& format)
    {
        return format == "VST3" || format == "VST" || format == "AudioUnit" || format == "LV2" || format == "AAX";
    }

    /** Formats whose hosted editors follow AudioProcessorEditor::setScaleFactor. */
    bool followsHostScale (const juce::String& format)
    {
        return format == "VST3" || format == "VST" || format == "LV2";
    }

    juce::String latencyText (const juce::AudioProcessor& p)
    {
        const int samples = p.getLatencySamples();
        if (samples <= 0)
            return {};
        if (p.getSampleRate() > 0.0)
            return juce::String (1000.0 * samples / p.getSampleRate(), 1) + " ms";
        return juce::String (samples) + " smp";
    }

    const juce::String fixedSizeTip ("This plugin's editor is a native view with a fixed size, which Sidecar can't scale; collapse it instead");
} // namespace

//==============================================================================
/** One plugin: a slim title row over its editor (only a thin column, or the
    title row, when collapsed). */
class PluginChainView::Card final : public juce::Component, public juce::SettableTooltipClient
{
public:
    Card (PluginChainView& o, juce::AudioPluginInstance& p) : owner (o), plugin (p)
    {
        editor.reset (plugin.hasEditor() ? plugin.createEditorAndMakeActive() : nullptr);
        if (editor == nullptr)
            editor = std::make_unique<juce::GenericAudioProcessorEditor> (plugin);
        addAndMakeVisible (*editor);
        base = { juce::jmax (1, editor->getWidth()), juce::jmax (1, editor->getHeight()) };
        chooseMethod();

        for (auto* key : { &collapse, &clear, &bypass, &zoomOut, &zoomIn, &remove })
            addAndMakeVisible (*key);
        clear.ink = Colours::text;
        collapse.onClick = [this] { owner.setCollapsed (index, ! collapsed); };
        clear.onClick = [this] { owner.clearFault (index); };
        bypass.onClick = [this] { owner.toggleBypass (index); };
        zoomOut.onClick = [this] { owner.stepZoom (index, -1); };
        zoomIn.onClick = [this] { owner.stepZoom (index, 1); };
        remove.onClick = [this] { owner.remove (index); };
    }

    ~Card() override
    {
        // Before the plugin may go (plugin formats' editors tell it themselves; others don't).
        plugin.editorBeingDeleted (editor.get());
        editor = nullptr;
    }

    void update (int newIndex, int newCount, bool isBypassed, bool isFaulted, PluginChain::View view, bool isSelected)
    {
        index = newIndex;
        count = newCount;
        bypassed = isBypassed;
        faulted = isFaulted;
        collapsed = view.collapsed;
        selected = isSelected;
        vertical = owner.layoutMode == Layout::row;

        const int zoom = method == ZoomMethod::fixed ? 100 : view.zoom;
        if (zoom != applied)
            applyZoom (zoom);
        editor->setVisible (! collapsed);
        editor->setAlpha (bypassed || faulted ? 0.45f : 1.0f); // both out of the signal path

        const auto desc = plugin.getPluginDescription();
        setTooltip (plugin.getName() + " (" + desc.manufacturerName + ", " + desc.pluginFormatName + "), plugin " + juce::String (index + 1)
                    + " of " + juce::String (count));
        collapse.setToggleState (collapsed, juce::dontSendNotification);
        collapse.setTooltip (collapsed ? "Show this plugin's editor" : "Collapse this plugin to its title");
        bypass.setToggleState (bypassed, juce::dontSendNotification);
        clear.setVisible (faulted && ! (collapsed && vertical));
        const bool fixed = method == ZoomMethod::fixed;
        zoomOut.setEnabled (! fixed && applied > PluginChain::kZoomSteps[0]);
        zoomIn.setEnabled (! fixed && applied < 100);
        zoomOut.setTooltip (fixed ? fixedSizeTip : "Make this plugin's editor smaller");
        zoomIn.setTooltip (fixed ? fixedSizeTip : "Make this plugin's editor larger (up to 100 %)");
        for (auto* key : { &bypass, &zoomOut, &zoomIn, &remove })
            key->setVisible (! (collapsed && vertical));
        resized();
        repaint();
    }

    /** The editor's size on screen (scaled). */
    juce::Point<int> shownSize() const
    {
        if (method == ZoomMethod::transform)
        {
            const float s = (float) applied / 100.0f;
            return { juce::roundToInt ((float) editor->getWidth() * s), juce::roundToInt ((float) editor->getHeight() * s) };
        }
        return { editor->getWidth(), editor->getHeight() };
    }

    int preferredWidth() const
    {
        if (collapsed && vertical)
            return kCollapsedWidth;
        return juce::jmax (kMinCardWidth, shownSize().x);
    }

    int preferredHeight() const
    {
        if (collapsed)
            return vertical ? 5 * kTitleHeight : kTitleHeight;
        return kTitleHeight + shownSize().y;
    }

    /** Called by the view's timer: a host scale factor the editor hasn't followed
        is dropped (the editor then has a fixed size). True if so. */
    bool checkHostScale()
    {
        if (hostCheckTicks <= 0 || method != ZoomMethod::hostScale)
            return false;
        if (smallerThanBase())
        {
            hostCheckTicks = 0;
            return false;
        }
        if (--hostCheckTicks > 0)
            return false;
        editor->setScaleFactor (1.0f);
        method = ZoomMethod::fixed;
        applied = 100;
        return true;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colours::screen);
        auto r = getLocalBounds();

        if (collapsed && vertical)
        {
            // A thin column: the expand key, a status dot, the name upwards.
            r.removeFromTop (kTitleHeight);
            auto dot = r.removeFromTop (16).toFloat().withSizeKeepingCentre (7.0f, 7.0f);
            paintDot (g, dot);
            r.removeFromTop (6);
            juce::Graphics::ScopedSaveState state (g);
            const auto area = r.reduced (0, 4).toFloat();
            g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi, area.getCentreX(), area.getCentreY()));
            const auto turned = juce::Rectangle<float> (area.getHeight(), area.getWidth()).withCentre (area.getCentre());
            g.setColour (bypassed ? Colours::textDim : Colours::caption);
            g.setFont (wingFontPx (8.5f, WingWeight::SemiBold));
            g.drawText (plugin.getName(), turned, juce::Justification::centredRight, true);
            return;
        }

        auto title = r.removeFromTop (kTitleHeight);
        g.setColour (Colours::screenPanel);
        g.fillRect (title);
        g.setColour (selected ? Colours::gold : Colours::screenLine);
        g.fillRect (title.removeFromBottom (selected ? 2 : 1));

        g.setColour (bypassed ? Colours::textDim : Colours::text);
        g.setFont (wingFontPx (9.5f, WingWeight::SemiBold));
        g.drawFittedText (plugin.getName(), nameArea, juce::Justification::centredLeft, 1, 0.7f);

        if (faulted)
        {
            const auto badge = faultArea.toFloat().reduced (0.0f, 2.0f);
            g.setColour (Colours::mute);
            g.fillRoundedRectangle (badge, 3.0f);
            g.setColour (Colours::text);
            g.setFont (wingFontPx (7.5f, WingWeight::SemiBold));
            g.drawText ("FAULT", badge, juce::Justification::centred);
        }

        if (! zoomArea.isEmpty())
        {
            g.setColour (method == ZoomMethod::fixed ? Colours::textDim.withAlpha (0.6f) : Colours::caption);
            g.setFont (wingFontPx (7.5f));
            g.drawText (method == ZoomMethod::fixed ? juce::String ("FIXED") : juce::String (applied) + " %", zoomArea, juce::Justification::centred);
        }

        if (collapsed)
        {
            g.setColour (Colours::textDim);
            g.setFont (wingFontPx (7.5f));
            g.drawText ("COLLAPSED", r.reduced (8, 0), juce::Justification::centredLeft);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        if (collapsed && vertical)
        {
            collapse.setBounds (r.removeFromTop (kTitleHeight).reduced (4));
            zoomArea = nameArea = faultArea = {};
            return;
        }

        auto title = r.removeFromTop (kTitleHeight).reduced (4, 3);
        collapse.setBounds (title.removeFromLeft (18));
        title.removeFromLeft (4);
        remove.setBounds (title.removeFromRight (20));
        title.removeFromRight (8);
        zoomIn.setBounds (title.removeFromRight (18));
        zoomArea = title.removeFromRight (40);
        zoomOut.setBounds (title.removeFromRight (18));
        title.removeFromRight (8);
        bypass.setBounds (title.removeFromRight (34));
        title.removeFromRight (6);
        faultArea = {};
        if (faulted)
        {
            clear.setBounds (title.removeFromRight (42));
            title.removeFromRight (4);
            faultArea = title.removeFromRight (42);
            title.removeFromRight (4);
        }
        nameArea = title;
        placeEditor();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            owner.showMemberMenu (index, *this);
        else if (collapsed && vertical)
            owner.setCollapsed (index, false);
        else if (e.y < kTitleHeight)
            owner.select (index);
    }

    PluginChainView& owner;
    juce::AudioPluginInstance& plugin;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    juce::Point<int> base; // the editor's size at 100 %
    ZoomMethod method = ZoomMethod::fixed;
    int applied = 100;     // the zoom shown
    bool collapsed = false;

private:
    void chooseMethod()
    {
        const auto format = plugin.getPluginDescription().pluginFormatName;
        const bool juceComponents = dynamic_cast<juce::GenericAudioProcessorEditor*> (editor.get()) != nullptr
                                 || dynamic_cast<SandboxedPlugin*> (&plugin) != nullptr; // the sandbox's own panel
        native = ! juceComponents && isNativeFormat (format);
        hostScaleCapable = native && followsHostScale (format);
        if (juceComponents)
            method = ZoomMethod::transform; // generic controls only scroll when resized
        else if (editor->isResizable())
            method = ZoomMethod::resize;
        else
            method = fallback();
    }

    ZoomMethod fallback() const
    {
        if (! native)
            return ZoomMethod::transform;
        return hostScaleCapable && method != ZoomMethod::hostScale ? ZoomMethod::hostScale : ZoomMethod::fixed;
    }

    bool smallerThanBase() const { return editor->getWidth() < base.x || editor->getHeight() < base.y; }

    void applyZoom (int zoom)
    {
        applied = zoom;
        const float s = (float) zoom / 100.0f;
        switch (method)
        {
            case ZoomMethod::resize:
            {
                int w = juce::roundToInt ((float) base.x * s), h = juce::roundToInt ((float) base.y * s);
                if (auto* c = editor->getConstrainer())
                {
                    if (const auto ratio = c->getFixedAspectRatio(); ratio > 0.0)
                        h = juce::roundToInt (w / ratio);
                    w = juce::jlimit (c->getMinimumWidth(), juce::jmax (c->getMinimumWidth(), c->getMaximumWidth()), w);
                    h = juce::jlimit (c->getMinimumHeight(), juce::jmax (c->getMinimumHeight(), c->getMaximumHeight()), h);
                }
                editor->setSize (juce::jmax (1, w), juce::jmax (1, h));
                if (zoom < 100 && ! smallerThanBase())
                {
                    // Its constrainer won't let it shrink: try the next way.
                    editor->setSize (base.x, base.y);
                    method = fallback();
                    applyZoom (method == ZoomMethod::fixed ? 100 : zoom);
                    return;
                }
                break;
            }
            case ZoomMethod::hostScale:
                editor->setScaleFactor (s);
                // Checked again by the view's timer: a plugin may resize itself later.
                hostCheckTicks = zoom < 100 && ! smallerThanBase() ? 3 : 0;
                break;
            case ZoomMethod::transform:
            case ZoomMethod::fixed:
                break;
        }
        placeEditor();
    }

    void placeEditor()
    {
        if (collapsed)
            return;
        const auto size = shownSize();
        const int x = juce::jmax (0, (getWidth() - size.x) / 2);
        if (method == ZoomMethod::transform)
        {
            editor->setTopLeftPosition (0, 0);
            const float s = (float) applied / 100.0f;
            editor->setTransform (juce::AffineTransform::scale (s).translated ((float) x, (float) kTitleHeight));
        }
        else
        {
            editor->setTopLeftPosition (x, kTitleHeight);
        }
    }

    void paintDot (juce::Graphics& g, juce::Rectangle<float> dot) const
    {
        if (faulted)
        {
            g.setColour (Colours::mute);
            g.fillEllipse (dot);
        }
        else if (bypassed)
        {
            g.setColour (Colours::textDim);
            g.drawEllipse (dot.reduced (0.5f), 1.2f);
        }
        else
        {
            g.setColour (Colours::green);
            g.fillEllipse (dot);
        }
    }

    int index = 0, count = 1;
    bool bypassed = false, faulted = false, selected = false, vertical = true;
    bool native = false, hostScaleCapable = false;
    int hostCheckTicks = 0;
    juce::Rectangle<int> nameArea, faultArea, zoomArea;
    Key collapse { Key::Glyph::collapse, {}, {} };
    Key clear { Key::Glyph::word, "CLEAR", "Clear the fault: the plugin runs again", Colours::mute };
    Key bypass { Key::Glyph::word, "BYP", "Bypass this plugin (it adds no latency while bypassed)", Colours::amber };
    Key zoomOut { Key::Glyph::minus, {}, {} }, zoomIn { Key::Glyph::plus, {}, {} };
    Key remove { Key::Glyph::remove, {}, "Remove this plugin from the chain" };
};

//==============================================================================
/** A plugin in the chain bar: click selects it, its dot bypasses it, a drag
    moves it, right-click opens its menu. */
class PluginChainView::Chip final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit Chip (PluginChainView& o) : owner (o) {}

    int index = 0;
    juce::String name, latency;
    bool bypassed = false, faulted = false, selected = false;

    int desiredWidth() const
    {
        const auto font = wingFontPx (8.5f, WingWeight::SemiBold);
        float w = 26.0f + juce::GlyphArrangement::getStringWidth (font, name);
        if (latency.isNotEmpty())
            w += 8.0f + juce::GlyphArrangement::getStringWidth (wingFontPx (7.0f), latency);
        return juce::jlimit (64, 190, juce::roundToInt (w));
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f, 5.0f);
        g.setColour (dragging ? Colours::screenTile.brighter (0.15f) : Colours::screenTile);
        g.fillRoundedRectangle (r, 4.0f);
        if (faulted)
        {
            g.setColour (Colours::mute);
            g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.2f);
        }
        if (selected)
        {
            g.setColour (Colours::gold);
            g.fillRoundedRectangle (r.withTop (r.getBottom() - 2.5f).reduced (4.0f, 0.0f), 1.0f);
        }

        auto inner = r.reduced (8.0f, 0.0f);
        const auto dot = dotArea().toFloat().withSizeKeepingCentre (8.0f, 8.0f);
        if (faulted)
        {
            g.setColour (Colours::mute);
            g.fillEllipse (dot);
            g.setColour (Colours::text);
            g.fillRect (juce::Rectangle<float> (1.4f, 3.6f).withCentre (dot.getCentre().translated (0.0f, -0.6f)));
        }
        else if (bypassed)
        {
            g.setColour (Colours::textDim);
            g.drawEllipse (dot.reduced (0.6f), 1.2f);
        }
        else
        {
            g.setColour (Colours::green);
            g.fillEllipse (dot);
        }
        inner.removeFromLeft (14.0f);

        if (latency.isNotEmpty())
        {
            g.setColour (Colours::textDim);
            g.setFont (wingFontPx (7.0f));
            const auto w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), latency) + 2.0f;
            g.drawText (latency, inner.removeFromRight (juce::jmin (w, inner.getWidth() * 0.45f)), juce::Justification::centredRight, true);
            inner.removeFromRight (4.0f);
        }
        g.setColour (bypassed ? Colours::textDim : Colours::text);
        g.setFont (wingFontPx (8.5f, WingWeight::SemiBold));
        g.drawFittedText (name, inner.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
    }

    juce::Rectangle<int> dotArea() const { return getLocalBounds().removeFromLeft (24); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = false;
        startX = getX();
        if (e.mods.isPopupMenu())
            owner.showMemberMenu (index, *this);
    }

    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    PluginChainView& owner;
    bool dragging = false;
    int startX = 0;
};

//==============================================================================
/** IN > chips > OUT, +, then FIT and ROW / FOCUS at the right end. */
class PluginChainView::Bar final : public juce::Component
{
public:
    explicit Bar (PluginChainView& o) : owner (o)
    {
        plus.setTooltip ("Add a plugin at the end of the chain (up to " + juce::String (PluginChain::kMaxMembers) + ")");
        plus.onClick = [this]
        {
            if (owner.callbacks.addPlugin)
                owner.callbacks.addPlugin (plus);
        };
        fit.onClick = [this] { owner.fitToHeight(); };
        rowKey.onClick = [this] { owner.setLayout (Layout::row); };
        focusKey.onClick = [this] { owner.setLayout (Layout::focus); };
        for (auto* key : { &plus, &fit, &rowKey, &focusKey })
            addAndMakeVisible (*key);
    }

    void update()
    {
        auto& fx = owner.fx;
        const int n = fx.getNumChainMembers (owner.slot);
        while ((int) chips.size() > n)
            chips.pop_back();
        while ((int) chips.size() < n)
            addAndMakeVisible (*chips.emplace_back (std::make_unique<Chip> (owner)));

        for (int i = 0; i < n; ++i)
        {
            auto& chip = *chips[(size_t) i];
            auto* plugin = fx.getChainMember (owner.slot, i);
            chip.index = i;
            chip.name = plugin->getName();
            chip.latency = latencyText (*plugin);
            chip.bypassed = fx.isChainMemberBypassed (owner.slot, i);
            chip.faulted = fx.isChainMemberFaulted (owner.slot, i);
            chip.selected = i == owner.selected && (owner.layoutMode == Layout::focus || n > 1);
            chip.setTooltip (chip.name + (chip.faulted ? " (FAULT: bypassed until cleared)" : chip.bypassed ? " (bypassed)" : juce::String())
                             + "\nClick: show it. Dot: bypass. Drag: move it in the chain. Right-click: more");
            chip.repaint();
        }
        plus.setEnabled (n > 0 && n < PluginChain::kMaxMembers);
        rowKey.setToggleState (owner.layoutMode == Layout::row, juce::dontSendNotification);
        focusKey.setToggleState (owner.layoutMode == Layout::focus, juce::dontSendNotification);
        resized();
        repaint();
    }

    /** The narrowest the bar lays out without squeezing the chips. */
    int minimumWidth() const
    {
        int w = kIn + kOut + 3 * kArrow + kKey + 16 + kFit + 2 * kLayoutKey + 24;
        for (auto& chip : chips)
            w += chip->desiredWidth() + kArrow;
        return w;
    }

    juce::Range<int> chipRange() const { return { kIn + kArrow, juce::jmax (kIn + kArrow, outX - kArrow) }; }

    /** A dragged chip was let go: it goes where its centre is. */
    void dropped (Chip& chip)
    {
        int target = 0;
        for (auto& other : chips)
            if (other.get() != &chip && other->getBounds().getCentreX() < chip.getBounds().getCentreX())
                ++target;
        if (target != chip.index)
            owner.move (chip.index, target);
        else
            resized();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        g.setColour (Colours::screenPanel);
        g.fillRect (r);
        g.setColour (Colours::screenLine);
        g.fillRect (r.removeFromBottom (1));

        g.setColour (Colours::textDim);
        g.setFont (wingFontPx (8.0f, WingWeight::SemiBold));
        g.drawText ("IN", juce::Rectangle<int> (6, 0, kIn - 6, getHeight()), juce::Justification::centredLeft);
        g.drawText ("OUT", juce::Rectangle<int> (outX, 0, kOut, getHeight()), juce::Justification::centredLeft);

        // The signal path: a thin rule with arrow heads between the stations.
        const float y = (float) getHeight() * 0.5f;
        for (const int x : arrows)
        {
            juce::Path p;
            const float cx = (float) x + kArrow * 0.5f;
            g.fillRect (juce::Rectangle<float> ((float) x + 1.0f, y - 0.5f, (float) kArrow - 5.0f, 1.0f));
            p.addTriangle (cx + 4.0f, y, cx - 1.0f, y - 3.5f, cx - 1.0f, y + 3.5f);
            g.fillPath (p);
        }
    }

    void resized() override
    {
        const int h = getHeight();
        auto right = getLocalBounds().reduced (6, 5);
        focusKey.setBounds (right.removeFromRight (kLayoutKey));
        rowKey.setBounds (right.removeFromRight (kLayoutKey));
        right.removeFromRight (10);
        fit.setBounds (right.removeFromRight (kFit));
        const int limit = right.getRight() - 10 - kKey - kOut - kArrow;

        // The chips at their width, squeezed alike when they don't fit.
        arrows.clear();
        int total = 0;
        for (auto& chip : chips)
            total += chip->desiredWidth() + kArrow;
        const int room = juce::jmax (0, limit - kIn);
        const float squeeze = total > room && total > 0 ? (float) room / (float) total : 1.0f;

        int x = kIn;
        arrows.push_back (x);
        x += kArrow;
        for (auto& chip : chips)
        {
            const int w = juce::jmax (40, juce::roundToInt ((float) chip->desiredWidth() * squeeze));
            chip->setBounds (x, 0, w, h);
            x += w;
            arrows.push_back (x);
            x += kArrow;
        }
        outX = x;
        x += kOut;
        plus.setBounds (x, 5, kKey, h - 10);
    }

private:
    static constexpr int kIn = 26, kOut = 30, kArrow = 16, kKey = 26, kFit = 36, kLayoutKey = 50;

    PluginChainView& owner;
    std::vector<std::unique_ptr<Chip>> chips;
    std::vector<int> arrows;
    int outX = kIn + kArrow;
    Key plus { Key::Glyph::plus, {}, {} };
    Key fit { Key::Glyph::word, "FIT", "Shrink the plugins so the whole chain fits the window's height" };
    Key rowKey { Key::Glyph::word, "ROW", "Show the plugins side by side" };
    Key focusKey { Key::Glyph::word, "FOCUS", "Show one plugin at a time (the chips are its tabs)" };
};

void PluginChainView::Chip::mouseDrag (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() || owner.cards.size() < 2)
        return;
    if (! dragging && std::abs (e.getDistanceFromDragStartX()) > 5)
    {
        dragging = true;
        toFront (false);
    }
    if (dragging)
    {
        const auto limits = owner.bar->chipRange();
        const int x = juce::jlimit (limits.getStart(), juce::jmax (limits.getStart(), limits.getEnd() - getWidth()), startX + e.getDistanceFromDragStartX());
        setTopLeftPosition (x, getY());
        repaint();
    }
}

void PluginChainView::Chip::mouseUp (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;
    if (dragging)
    {
        dragging = false;
        owner.bar->dropped (*this);
    }
    else if (dotArea().contains (e.getPosition()))
    {
        owner.toggleBypass (index);
    }
    else
    {
        owner.select (index);
    }
}

//==============================================================================
/** The cards, at their natural size (in the viewport), with thin rules between them. */
class PluginChainView::Rack final : public juce::Component
{
public:
    explicit Rack (PluginChainView& o) : owner (o) {}

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colours::screen);
        g.setColour (Colours::screenLine);
        for (auto& card : owner.cards)
            if (card->isVisible() && card->getRight() < getWidth())
                g.fillRect (card->getRight(), 0, 1, getHeight());
    }

private:
    PluginChainView& owner;
};

//==============================================================================
PluginChainView::PluginChainView (FxSlots& f, int s, Callbacks c)
    : fx (f), slot (s), callbacks (std::move (c)), bar (std::make_unique<Bar> (*this)), rack (std::make_unique<Rack> (*this)),
      maxSize (defaultMaxSize())
{
    addAndMakeVisible (*bar);
    viewport.setViewedComponent (rack.get(), false);
    viewport.setScrollBarsShown (true, true, false, false); // only when needed
    addAndMakeVisible (viewport);

    const auto chainChanged = std::move (callbacks.chainChanged); // not while constructing
    refresh();
    callbacks.chainChanged = chainChanged;
    startTimerHz (5);
}

PluginChainView::~PluginChainView()
{
    stopTimer();
    cards.clear(); // the editors go first
}

bool PluginChainView::shows (const juce::AudioProcessor* p) const noexcept
{
    if (p == nullptr)
        return false;
    if (p == shown)
        return true;
    return std::any_of (cards.begin(), cards.end(), [p] (auto& c) { return &c->plugin == p; });
}

void PluginChainView::dropEditorFor (const juce::AudioProcessor* p)
{
    const auto it = std::find_if (cards.begin(), cards.end(), [p] (auto& c) { return &c->plugin == p; });
    if (it == cards.end())
        return;
    (*it)->editor->removeComponentListener (this);
    cards.erase (it);
    layout();
}

void PluginChainView::setMaxSize (juce::Point<int> size)
{
    maxSize = size;
    lastNatural = {};
    layout();
}

void PluginChainView::setLayout (Layout l)
{
    if (l == layoutMode)
        return;
    layoutMode = l;
    lastNatural = {};
    refresh();
    if (callbacks.layoutChanged)
        callbacks.layoutChanged (l);
}

void PluginChainView::select (int index)
{
    if (! juce::isPositiveAndBelow (index, getNumCards()))
        return;
    selected = index;
    refresh();
    if (layoutMode == Layout::row)
        viewport.setViewPosition (cards[(size_t) index]->getX(), viewport.getViewPositionY());
}

int PluginChainView::nextZoom (int percent, int direction) noexcept
{
    const auto& steps = PluginChain::kZoomSteps;
    const int n = (int) std::size (steps);
    int i = 0;
    while (i < n - 1 && steps[i] < PluginChain::snapZoom (percent))
        ++i;
    return steps[juce::jlimit (0, n - 1, i + (direction > 0 ? 1 : direction < 0 ? -1 : 0))];
}

int PluginChainView::fitZoom (int baseHeight, int available) noexcept
{
    if (baseHeight <= 0)
        return 100;
    for (int i = (int) std::size (PluginChain::kZoomSteps); --i >= 0;)
    {
        const int step = PluginChain::kZoomSteps[i];
        if (juce::roundToInt ((float) baseHeight * (float) step / 100.0f) <= available)
            return step;
    }
    return PluginChain::kZoomSteps[0];
}

void PluginChainView::setZoom (int index, int percent)
{
    auto view = fx.getChainMemberView (slot, index);
    view.zoom = PluginChain::snapZoom (percent);
    fx.setChainMemberView (slot, index, view);
    refresh();
}

void PluginChainView::stepZoom (int index, int direction)
{
    setZoom (index, nextZoom (fx.getChainMemberView (slot, index).zoom, direction));
}

void PluginChainView::setCollapsed (int index, bool collapsed)
{
    auto view = fx.getChainMemberView (slot, index);
    view.collapsed = collapsed;
    fx.setChainMemberView (slot, index, view);
    refresh();
}

int PluginChainView::rackHeightAvailable() const
{
    int h = viewport.getHeight();
    if (rack->getWidth() > viewport.getWidth())
        h -= viewport.getScrollBarThickness();
    return h;
}

void PluginChainView::fitToHeight()
{
    const int available = rackHeightAvailable() - kTitleHeight;
    for (int i = 0; i < getNumCards(); ++i)
    {
        auto& card = *cards[(size_t) i];
        if (card.collapsed || card.method == ZoomMethod::fixed)
            continue;
        auto view = fx.getChainMemberView (slot, i);
        view.zoom = fitZoom (card.base.y, available);
        fx.setChainMemberView (slot, i, view);
    }
    refresh();
}

void PluginChainView::toggleBypass (int index)
{
    fx.setChainMemberBypassed (slot, index, ! fx.isChainMemberBypassed (slot, index)); // also clears a fault
    refresh();
}

void PluginChainView::clearFault (int index)
{
    fx.clearChainMemberFault (slot, index);
    refresh();
}

void PluginChainView::move (int from, int to)
{
    if (fx.moveInChain (slot, from, to))
    {
        if (selected == from)
            selected = to;
        else if (selected > from && selected <= to)
            --selected;
        else if (selected < from && selected >= to)
            ++selected;
    }
    refresh();
}

void PluginChainView::remove (int index)
{
    if (callbacks.removeMember)
        callbacks.removeMember (index);
}

void PluginChainView::showMemberMenu (int index, juce::Component& target)
{
    if (! juce::isPositiveAndBelow (index, getNumCards()))
        return;
    auto& card = *cards[(size_t) index];
    const int n = getNumCards();
    const auto view = fx.getChainMemberView (slot, index);
    const bool fixed = card.method == ZoomMethod::fixed;
    juce::Component::SafePointer<PluginChainView> safe (this);
    auto with = [safe] (std::function<void (PluginChainView&)> f)
    {
        return [safe, f] { if (safe != nullptr) f (*safe); };
    };

    juce::PopupMenu menu;
    menu.addSectionHeader (card.plugin.getName() + " (plugin " + juce::String (index + 1) + " of " + juce::String (n) + ")");
    menu.addItem ("Bypass", true, fx.isChainMemberBypassed (slot, index), with ([index] (auto& v) { v.toggleBypass (index); }));
    if (fx.isChainMemberFaulted (slot, index))
        menu.addItem ("Clear fault (run it again)", with ([index] (auto& v) { v.clearFault (index); }));
    menu.addItem ("Move earlier", index > 0, false, with ([index] (auto& v) { v.move (index, index - 1); }));
    menu.addItem ("Move later", index < n - 1, false, with ([index] (auto& v) { v.move (index, index + 1); }));

    juce::PopupMenu zoom;
    for (const int step : PluginChain::kZoomSteps)
        zoom.addItem (juce::String (step) + " %", ! fixed, ! fixed && view.zoom == step, with ([index, step] (auto& v) { v.setZoom (index, step); }));
    zoom.addSeparator();
    zoom.addItem ("Fit all to the window's height", with ([] (auto& v) { v.fitToHeight(); }));
    menu.addSubMenu (fixed ? "Zoom (fixed size: collapse it instead)" : "Zoom", zoom);
    menu.addItem (view.collapsed ? "Expand" : "Collapse", with ([index, c = view.collapsed] (auto& v) { v.setCollapsed (index, ! c); }));
    menu.addSeparator();
    menu.addItem ("Remove", with ([index] (auto& v) { v.remove (index); }));
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target));
}

PluginChainView::ZoomMethod PluginChainView::getZoomMethod (int index) const
{
    return juce::isPositiveAndBelow (index, getNumCards()) ? cards[(size_t) index]->method : ZoomMethod::fixed;
}

juce::Rectangle<int> PluginChainView::getCardBounds (int index) const
{
    return juce::isPositiveAndBelow (index, getNumCards()) ? cards[(size_t) index]->getBounds() : juce::Rectangle<int>();
}

juce::Rectangle<int> PluginChainView::getEditorBounds (int index) const
{
    if (! juce::isPositiveAndBelow (index, getNumCards()))
        return {};
    auto& card = *cards[(size_t) index];
    return card.editor->isVisible() ? card.editor->getBoundsInParent() : juce::Rectangle<int>();
}

bool PluginChainView::isCardVisible (int index) const
{
    return juce::isPositiveAndBelow (index, getNumCards()) && cards[(size_t) index]->isVisible();
}

juce::String PluginChainView::signature() const
{
    juce::String s;
    s << (juce::pointer_sized_int) fx.getPlugin (slot);
    for (int i = 0; i < fx.getNumChainMembers (slot); ++i)
    {
        auto* p = fx.getChainMember (slot, i);
        const auto view = fx.getChainMemberView (slot, i);
        s << '|' << (juce::pointer_sized_int) p << (fx.isChainMemberBypassed (slot, i) ? 'b' : '-') << (fx.isChainMemberFaulted (slot, i) ? 'f' : '-')
          << view.zoom << (view.collapsed ? 'c' : '-') << p->getLatencySamples() << p->getName();
    }
    return s;
}

void PluginChainView::refresh()
{
    shown = fx.getPlugin (slot);
    const int n = fx.getNumChainMembers (slot);

    // Keep the editors of the plugins that stay; new plugins get one.
    std::vector<std::unique_ptr<Card>> next;
    for (int i = 0; i < n; ++i)
    {
        auto* plugin = fx.getChainMember (slot, i);
        const auto it = std::find_if (cards.begin(), cards.end(), [plugin] (auto& c) { return c != nullptr && &c->plugin == plugin; });
        if (it != cards.end())
        {
            next.push_back (std::move (*it));
        }
        else
        {
            next.push_back (std::make_unique<Card> (*this, *plugin));
            rack->addAndMakeVisible (*next.back());
            next.back()->editor->addComponentListener (this);
        }
    }
    for (auto& old : cards)
        if (old != nullptr)
            old->editor->removeComponentListener (this);
    cards = std::move (next);

    selected = juce::jlimit (0, juce::jmax (0, n - 1), selected);
    for (int i = 0; i < n; ++i)
        cards[(size_t) i]->update (i, n, fx.isChainMemberBypassed (slot, i), fx.isChainMemberFaulted (slot, i), fx.getChainMemberView (slot, i),
                                   i == selected && n > 1);
    bar->update();

    lastSignature = signature();
    layout();
    if (callbacks.chainChanged)
        callbacks.chainChanged();
}

juce::Point<int> PluginChainView::naturalSize() const
{
    int w = 0, h = 4 * kTitleHeight;
    if (layoutMode == Layout::row)
    {
        for (auto& c : cards)
            w += c->preferredWidth() + 1;
    }
    else
    {
        // The largest card, so the window keeps its size from tab to tab.
        for (auto& c : cards)
            w = juce::jmax (w, c->preferredWidth());
    }
    for (auto& c : cards)
        h = juce::jmax (h, c->preferredHeight());
    w = juce::jmax (w, bar->minimumWidth());
    h += kBarHeight;

    // A scroll bar takes room from the other direction.
    const int thickness = viewport.getScrollBarThickness();
    if (w > maxSize.x)
        h += thickness;
    if (h > maxSize.y)
        w += thickness;
    return { w, h };
}

void PluginChainView::layout()
{
    if (layingOut)
        return;
    const juce::ScopedValueSetter<bool> guard (layingOut, true);

    // The view takes its natural size when that changes (a window resized by
    // hand keeps its size otherwise).
    if (const auto natural = naturalSize(); natural != lastNatural)
    {
        lastNatural = natural;
        setSize (juce::jmin (natural.x, maxSize.x), juce::jmin (natural.y, maxSize.y));
    }
    bar->setBounds (getLocalBounds().removeFromTop (kBarHeight));
    viewport.setBounds (getLocalBounds().withTrimmedTop (kBarHeight));

    if (layoutMode == Layout::row)
    {
        int h = 4 * kTitleHeight;
        for (auto& c : cards)
            h = juce::jmax (h, c->preferredHeight());
        int x = 0;
        for (auto& c : cards)
        {
            c->setVisible (true);
            c->setBounds (x, 0, c->preferredWidth(), h);
            x += c->preferredWidth() + 1; // the rule
        }
        rack->setSize (juce::jmax (1, x - 1), h);
    }
    else
    {
        int w = 1, h = 1;
        for (int i = 0; i < getNumCards(); ++i)
        {
            auto& c = *cards[(size_t) i];
            c.setVisible (i == selected);
            if (i != selected)
                continue;
            w = juce::jmax (c.preferredWidth(), viewport.getMaximumVisibleWidth());
            h = c.preferredHeight();
            c.setBounds (0, 0, w, h);
        }
        rack->setSize (w, h);
    }
    rack->repaint();
}

void PluginChainView::paint (juce::Graphics& g)
{
    g.fillAll (Colours::screen);
}

void PluginChainView::resized()
{
    if (layingOut)
        return;
    bar->setBounds (getLocalBounds().removeFromTop (kBarHeight));
    viewport.setBounds (getLocalBounds().withTrimmedTop (kBarHeight));

    // A plugin on its own with a resizable editor follows the window.
    if (cards.size() == 1 && cards.front()->method == ZoomMethod::resize && ! cards.front()->collapsed)
    {
        auto& editor = *cards.front()->editor;
        auto w = getWidth(), h = getHeight() - kBarHeight - kTitleHeight;
        if (auto* constrainer = editor.getConstrainer())
        {
            w = juce::jlimit (constrainer->getMinimumWidth(), constrainer->getMaximumWidth(), w);
            h = juce::jlimit (constrainer->getMinimumHeight(), constrainer->getMaximumHeight(), h);
        }
        editor.setSize (juce::jmax (1, w), juce::jmax (1, h)); // re-lays out through componentMovedOrResized
    }
    layout(); // FOCUS: the card takes the width
}

void PluginChainView::componentMovedOrResized (juce::Component&, bool, bool wasResized)
{
    if (wasResized) // an editor changed its size
        layout();
}

void PluginChainView::timerCallback()
{
    bool dropped = false;
    for (auto& c : cards)
        dropped = c->checkHostScale() || dropped;
    if (dropped || signature() != lastSignature)
        refresh();
}

} // namespace livedaw::ui
