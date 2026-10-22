#include "ui/PluginChainView.h"
#include "ui/LookAndFeel.h"

#include <algorithm>

namespace livedaw::ui
{

namespace
{
    /** The header strip's keys: BYP, move left / right, remove. */
    class HeaderKey final : public juce::Button
    {
    public:
        enum class Kind
        {
            bypass,
            left,
            right,
            remove
        };

        explicit HeaderKey (Kind k) : juce::Button ({}), kind (k)
        {
            setTooltip (k == Kind::bypass ? "Bypass this plugin (it adds no latency while bypassed)"
                        : k == Kind::left ? "Move this plugin one place earlier in the chain"
                        : k == Kind::right ? "Move this plugin one place later in the chain"
                                           : "Remove this plugin from the chain");
        }

        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            auto r = getLocalBounds().toFloat().reduced (1.5f);
            const bool lit = kind == Kind::bypass && getToggleState();
            auto fill = lit ? Colours::amber : Colours::screenTile;
            if (over || down)
                fill = fill.brighter (down ? 0.25f : 0.12f);
            g.setColour (fill.withMultipliedAlpha (isEnabled() ? 1.0f : 0.4f));
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (Colours::screenLine);
            g.drawRoundedRectangle (r, 3.0f, 1.0f);

            const auto ink = (lit ? juce::Colours::black : Colours::text).withMultipliedAlpha (isEnabled() ? 1.0f : 0.35f);
            g.setColour (ink);
            const auto c = r.getCentre();
            const float s = juce::jmin (r.getWidth(), r.getHeight()) * 0.28f;
            juce::Path p;
            switch (kind)
            {
                case Kind::bypass:
                    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
                    g.drawText ("BYP", r, juce::Justification::centred);
                    return;
                case Kind::left:
                    p.addTriangle (c.x - s, c.y, c.x + s * 0.8f, c.y - s, c.x + s * 0.8f, c.y + s);
                    break;
                case Kind::right:
                    p.addTriangle (c.x + s, c.y, c.x - s * 0.8f, c.y - s, c.x - s * 0.8f, c.y + s);
                    break;
                case Kind::remove:
                    p.startNewSubPath (c.x - s, c.y - s);
                    p.lineTo (c.x + s, c.y + s);
                    p.startNewSubPath (c.x + s, c.y - s);
                    p.lineTo (c.x - s, c.y + s);
                    g.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                    return;
            }
            g.fillPath (p);
        }

    private:
        Kind kind;
    };

    juce::Point<int> defaultMaxSize()
    {
        if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            return { juce::roundToInt (display->userBounds.getWidth() * 0.9f), juce::roundToInt (display->userBounds.getHeight() * 0.85f) };
        return { 1600, 1000 };
    }
} // namespace

//==============================================================================
/** One plugin: its header strip over its editor. */
class PluginChainView::Column final : public juce::Component
{
public:
    Column (PluginChainView& o, juce::AudioPluginInstance& p) : owner (o), plugin (p)
    {
        editor.reset (plugin.hasEditor() ? plugin.createEditorAndMakeActive() : nullptr);
        if (editor == nullptr)
            editor = std::make_unique<juce::GenericAudioProcessorEditor> (plugin);
        addAndMakeVisible (*editor);

        name.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        name.setColour (juce::Label::textColourId, Colours::text);
        name.setMinimumHorizontalScale (0.6f);
        name.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (name);

        for (auto* key : { &bypass, &left, &right, &remove })
            addAndMakeVisible (*key);
        bypass.onClick = [this] { owner.fx.setChainMemberBypassed (owner.slot, index, ! bypass.getToggleState()); owner.refresh(); };
        left.onClick = [this] { if (owner.fx.moveInChain (owner.slot, index, index - 1)) owner.refresh(); };
        right.onClick = [this] { if (owner.fx.moveInChain (owner.slot, index, index + 1)) owner.refresh(); };
        remove.onClick = [this]
        {
            if (owner.callbacks.removeMember)
                owner.callbacks.removeMember (index);
        };
    }

    ~Column() override
    {
        // Before the plugin may go (plugin formats' editors tell it themselves; others don't).
        plugin.editorBeingDeleted (editor.get());
        editor = nullptr;
    }

    void update (int newIndex, int count, bool bypassed)
    {
        index = newIndex;
        name.setText (plugin.getName(), juce::dontSendNotification);
        const auto desc = plugin.getPluginDescription();
        name.setTooltip (plugin.getName() + " (" + desc.manufacturerName + ", " + desc.pluginFormatName + "), plugin "
                         + juce::String (index + 1) + " of " + juce::String (count));
        bypass.setToggleState (bypassed, juce::dontSendNotification);
        left.setEnabled (index > 0);
        right.setEnabled (index < count - 1);
        editor->setAlpha (bypassed ? 0.55f : 1.0f);
        repaint();
    }

    int preferredWidth() const { return juce::jmax (kMinColumnWidth, editor->getWidth()); }
    int preferredHeight() const { return kHeaderHeight + editor->getHeight(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colours::screenPanel);
        auto header = getLocalBounds().removeFromTop (kHeaderHeight);
        g.setColour (bypass.getToggleState() ? Colours::amber.withAlpha (0.25f) : Colours::screenHeader.brighter (0.08f));
        g.fillRect (header);
        g.setColour (Colours::screenLine);
        g.fillRect (header.removeFromBottom (1));
    }

    void resized() override
    {
        auto header = getLocalBounds().removeFromTop (kHeaderHeight).reduced (3, 2);
        remove.setBounds (header.removeFromRight (26));
        header.removeFromRight (6);
        right.setBounds (header.removeFromRight (26));
        left.setBounds (header.removeFromRight (26));
        header.removeFromRight (4);
        bypass.setBounds (header.removeFromRight (40));
        name.setBounds (header);
        editor->setTopLeftPosition (0, kHeaderHeight);
    }

    PluginChainView& owner;
    juce::AudioPluginInstance& plugin;
    std::unique_ptr<juce::AudioProcessorEditor> editor;

private:
    int index = 0;
    juce::Label name;
    HeaderKey bypass { HeaderKey::Kind::bypass }, left { HeaderKey::Kind::left }, right { HeaderKey::Kind::right },
        remove { HeaderKey::Kind::remove };
};

/** The columns and the +, at their natural size (in the viewport). */
class PluginChainView::Row final : public juce::Component
{
public:
    void paint (juce::Graphics& g) override { g.fillAll (Colours::screen); }
};

//==============================================================================
PluginChainView::PluginChainView (FxSlots& f, int s, Callbacks c)
    : fx (f), slot (s), callbacks (std::move (c)), row (std::make_unique<Row>()), maxSize (defaultMaxSize())
{
    viewport.setViewedComponent (row.get(), false);
    viewport.setScrollBarsShown (true, true, false, false); // only when needed
    addAndMakeVisible (viewport);

    plus.setTooltip ("Add a plugin at the end of the chain (up to " + juce::String (PluginChain::kMaxMembers) + ")");
    plus.setColour (juce::TextButton::buttonColourId, Colours::screenTile);
    plus.setColour (juce::TextButton::textColourOffId, Colours::text);
    plus.onClick = [this]
    {
        if (callbacks.addPlugin)
            callbacks.addPlugin (plus);
    };
    row->addAndMakeVisible (plus);

    const auto chainChanged = std::move (callbacks.chainChanged); // not while constructing
    refresh();
    callbacks.chainChanged = chainChanged;
    startTimerHz (5);
}

PluginChainView::~PluginChainView()
{
    stopTimer();
    columns.clear(); // the editors go first
}

bool PluginChainView::shows (const juce::AudioProcessor* p) const noexcept
{
    if (p == nullptr)
        return false;
    if (p == shown)
        return true;
    return std::any_of (columns.begin(), columns.end(), [p] (auto& c) { return &c->plugin == p; });
}

bool PluginChainView::wantsResizableWindow() const
{
    return columns.size() != 1 || columns.front()->editor->isResizable();
}

void PluginChainView::dropEditorFor (const juce::AudioProcessor* p)
{
    const auto it = std::find_if (columns.begin(), columns.end(), [p] (auto& c) { return &c->plugin == p; });
    if (it == columns.end())
        return;
    (*it)->editor->removeComponentListener (this);
    columns.erase (it);
    layout();
}

void PluginChainView::setMaxSize (juce::Point<int> size)
{
    maxSize = size;
    lastNatural = {};
    layout();
}

juce::String PluginChainView::signature() const
{
    juce::String s;
    s << (juce::pointer_sized_int) fx.getPlugin (slot);
    for (int i = 0; i < fx.getNumChainMembers (slot); ++i)
        s << '|' << (juce::pointer_sized_int) fx.getChainMember (slot, i) << (fx.isChainMemberBypassed (slot, i) ? 'b' : '-');
    return s;
}

void PluginChainView::refresh()
{
    shown = fx.getPlugin (slot);
    const int n = fx.getNumChainMembers (slot);

    // Keep the editors of the plugins that stay; new plugins get one.
    std::vector<std::unique_ptr<Column>> next;
    for (int i = 0; i < n; ++i)
    {
        auto* plugin = fx.getChainMember (slot, i);
        const auto it = std::find_if (columns.begin(), columns.end(), [plugin] (auto& c) { return c != nullptr && &c->plugin == plugin; });
        if (it != columns.end())
        {
            next.push_back (std::move (*it));
        }
        else
        {
            next.push_back (std::make_unique<Column> (*this, *plugin));
            row->addAndMakeVisible (*next.back());
            next.back()->editor->addComponentListener (this);
        }
    }
    for (auto& old : columns)
        if (old != nullptr)
            old->editor->removeComponentListener (this);
    columns = std::move (next);

    for (int i = 0; i < n; ++i)
        columns[(size_t) i]->update (i, n, fx.isChainMemberBypassed (slot, i));
    plus.setEnabled (n > 0 && n < PluginChain::kMaxMembers);

    lastSignature = signature();
    layout();
    if (callbacks.chainChanged)
        callbacks.chainChanged();
}

juce::Point<int> PluginChainView::naturalSize() const
{
    int w = kPlusWidth, h = 3 * kHeaderHeight;
    for (auto& c : columns)
    {
        w += c->preferredWidth() + kGap;
        h = juce::jmax (h, c->preferredHeight());
    }
    // A scroll bar takes room from the other direction.
    const int bar = viewport.getScrollBarThickness();
    if (w > maxSize.x)
        h += bar;
    if (h > maxSize.y)
        w += bar;
    return { w, h };
}

void PluginChainView::layout()
{
    if (layingOut)
        return;
    const juce::ScopedValueSetter<bool> guard (layingOut, true);

    int h = 3 * kHeaderHeight;
    for (auto& c : columns)
        h = juce::jmax (h, c->preferredHeight());
    int x = 0;
    for (auto& c : columns)
    {
        c->setBounds (x, 0, c->preferredWidth(), h);
        x += c->preferredWidth() + kGap;
    }
    plus.setBounds (x, 0, kPlusWidth, h);
    row->setSize (x + kPlusWidth, h);

    // The view takes its natural size when that changes (a window resized by
    // hand keeps its size otherwise).
    if (const auto natural = naturalSize(); natural != lastNatural)
    {
        lastNatural = natural;
        setSize (juce::jmin (natural.x, maxSize.x), juce::jmin (natural.y, maxSize.y));
    }
}

void PluginChainView::paint (juce::Graphics& g)
{
    g.fillAll (Colours::screen);
}

void PluginChainView::resized()
{
    viewport.setBounds (getLocalBounds());

    // A plugin on its own with a resizable editor follows the window.
    if (! layingOut && columns.size() == 1 && columns.front()->editor->isResizable())
    {
        auto& editor = *columns.front()->editor;
        auto w = getWidth() - kPlusWidth - kGap, h = getHeight() - kHeaderHeight;
        if (auto* constrainer = editor.getConstrainer())
        {
            w = juce::jlimit (constrainer->getMinimumWidth(), constrainer->getMaximumWidth(), w);
            h = juce::jlimit (constrainer->getMinimumHeight(), constrainer->getMaximumHeight(), h);
        }
        editor.setSize (juce::jmax (1, w), juce::jmax (1, h)); // re-lays out through componentMovedOrResized
    }
}

void PluginChainView::componentMovedOrResized (juce::Component&, bool, bool wasResized)
{
    if (wasResized) // an editor changed its size
        layout();
}

void PluginChainView::timerCallback()
{
    if (signature() != lastSignature)
        refresh();
}

} // namespace livedaw::ui
