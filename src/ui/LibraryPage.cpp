#include "ui/Pages.h"
#include "ui/ShowPanel.h"

namespace livedaw::ui
{

namespace
{
    /** Show file: name, new/open/save. */
    class ShowTab final : public juce::Component, private juce::Timer
    {
    public:
        explicit ShowTab (UiContext& c) : ctx (c)
        {
            caption.setText ("SHOW NAME", juce::dontSendNotification);
            caption.setColour (juce::Label::textColourId, Colours::accent);
            caption.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            name.setFont (juce::FontOptions (20.0f, juce::Font::bold));
            name.onTextChange = [this] { ctx.show.setName (name.getText()); };
            name.onReturnKey = [this] { unfocusAllComponents(); };
            info.setColour (juce::Label::textColourId, Colours::textDim);
            info.setJustificationType (juce::Justification::topLeft);

            newButton.onClick = [this] { if (ctx.newShow) ctx.newShow(); };
            openButton.onClick = [this] { if (ctx.openShow) ctx.openShow(); };
            saveButton.onClick = [this] { if (ctx.saveShow) ctx.saveShow (false); };
            saveAsButton.onClick = [this] { if (ctx.saveShow) ctx.saveShow (true); };
            for (juce::Component* comp : { (juce::Component*) &caption, (juce::Component*) &name, (juce::Component*) &info,
                                           (juce::Component*) &newButton, (juce::Component*) &openButton,
                                           (juce::Component*) &saveButton, (juce::Component*) &saveAsButton })
                addAndMakeVisible (comp);
            startTimerHz (2);
            timerCallback();
        }

        void timerCallback() override
        {
            if (! name.hasKeyboardFocus (true) && name.getText() != ctx.show.getName())
                name.setText (ctx.show.getName(), false);

            juce::String text;
            text << ctx.show.getNumScenes() << " scenes.  A show holds the whole console: "
                 << (int) ctx.engine.getChannels().size() << " channels, " << (int) ctx.engine.getBuses().size()
                 << " buses, the FX slot plugins, DCAs, mute groups, fader banks and every scene.\n\n"
                 << "Shows are saved atomically with a backup (.bak) and autosaved every 30 seconds. "
                 << "Older Live DAW shows are converted to the WING layout when they load.";
            info.setText (text, juce::dontSendNotification);
        }

        void paint (juce::Graphics& g) override { g.fillAll (Colours::screen); }

        void resized() override
        {
            auto b = getLocalBounds().reduced (14);
            caption.setBounds (b.removeFromTop (18));
            name.setBounds (b.removeFromTop (36).removeFromLeft (juce::jmin (520, b.getWidth())));
            b.removeFromTop (12);
            auto row = b.removeFromTop (34);
            for (auto* button : { &newButton, &openButton, &saveButton, &saveAsButton })
                button->setBounds (row.removeFromLeft (130).reduced (3, 0));
            b.removeFromTop (14);
            info.setBounds (b.removeFromTop (100));
        }

    private:
        UiContext& ctx;
        juce::Label caption, info;
        juce::TextEditor name;
        juce::TextButton newButton { "NEW" }, openButton { "OPEN..." }, saveButton { "SAVE" }, saveAsButton { "SAVE AS..." };
    };

    juce::String describeFader (UiContext& ctx, const state::FaderRef& f)
    {
        if (f.dca)
            return "DCA " + juce::String (f.dcaIndex + 1) + " " + ctx.engine.getDcaName (f.dcaIndex);
        if (auto* s = state::findStrip (ctx.engine, f.strip))
            return describeStrip (*s);
        return f.strip.toString() + " (missing)";
    }

    /** User fader banks: any strips and DCAs in any order. */
    class BanksTab final : public juce::Component
    {
    public:
        explicit BanksTab (UiContext& c) : ctx (c)
        {
            banks.model.getNumRowsFn = [this] { return (int) userLayers.size(); };
            banks.model.paintFn = [this] (int row, juce::Graphics& g, int w, int h, bool sel)
            {
                paintRow (g, w, h, sel, userLayers[(size_t) row].name, juce::String (userLayers[(size_t) row].faders.size()) + " faders", Colours::block);
            };
            banks.model.selectedFn = [this] (int) { refreshMembers(); };
            members.model.getNumRowsFn = [this] { const auto* l = currentLayer(); return l != nullptr ? (int) l->faders.size() : 0; };
            members.model.paintFn = [this] (int row, juce::Graphics& g, int w, int h, bool sel)
            {
                if (const auto* l = currentLayer(); l != nullptr && row < (int) l->faders.size())
                    paintRow (g, w, h, sel, describeFader (ctx, l->faders[(size_t) row]), {}, Colours::accent);
            };
            board.model.getNumRowsFn = [this] { return (int) boardFaders.size(); };
            board.model.paintFn = [this] (int row, juce::Graphics& g, int w, int h, bool sel)
            {
                paintRow (g, w, h, sel, describeFader (ctx, boardFaders[(size_t) row]), {}, Colours::textDim);
            };
            board.model.doubleClickFn = [this] (int) { addSelected(); };
            board.list.setMultipleSelectionEnabled (true);

            for (auto* l : { &banks, &members, &board })
                addAndMakeVisible (l->list);

            newButton.onClick = [this]
            {
                const auto id = ctx.show.layers.addUserLayer ("User " + juce::String ((int) userLayers.size() + 1));
                refresh();
                for (size_t i = 0; i < userLayers.size(); ++i)
                    if (userLayers[i].id == id)
                        banks.list.selectRow ((int) i);
                notifyLayersChangedAsync (ctx);
            };
            deleteButton.onClick = [this]
            {
                if (const auto* l = currentLayer())
                {
                    ctx.show.layers.removeUserLayer (l->id);
                    refresh();
                    notifyLayersChangedAsync (ctx);
                }
            };
            renameButton.onClick = [this]
            {
                const auto* l = currentLayer();
                if (l == nullptr)
                    return;
                auto* w = new juce::AlertWindow ("Rename bank", {}, juce::MessageBoxIconType::NoIcon);
                w->addTextEditor ("name", l->name);
                w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
                w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
                const auto id = l->id;
                w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w, id] (int r)
                {
                    if (r != 0)
                    {
                        ctx.show.layers.renameUserLayer (id, w->getTextEditorContents ("name"));
                        refresh();
                        notifyLayersChangedAsync (ctx);
                    }
                }), true);
            };
            addButton.onClick = [this] { addSelected(); };
            removeButton.onClick = [this]
            {
                const auto* l = currentLayer();
                const int row = members.list.getSelectedRow();
                if (l != nullptr && juce::isPositiveAndBelow (row, (int) l->faders.size()))
                {
                    ctx.show.layers.removeFromUserLayer (l->id, l->faders[(size_t) row]);
                    refreshAfterEdit();
                }
            };
            upButton.onClick = [this] { moveMember (-1); };
            downButton.onClick = [this] { moveMember (1); };

            for (auto* b : { &newButton, &deleteButton, &renameButton, &addButton, &removeButton, &upButton, &downButton })
                addAndMakeVisible (b);
            refresh();
        }

        void refresh()
        {
            const int selected = banks.list.getSelectedRow();
            userLayers = ctx.show.layers.getUserLayers();
            boardFaders.clear();
            for (int k = 0; k < kNumStripKinds; ++k)
                for (auto& s : ctx.engine.getStrips (static_cast<StripKind> (k)))
                    boardFaders.push_back (state::FaderRef::of (state::StripRef::of (*s)));
            for (int i = 0; i < kNumDcas; ++i)
                boardFaders.push_back (state::FaderRef::ofDca (i));

            banks.list.updateContent();
            board.list.updateContent();
            if (selected >= 0 && selected < (int) userLayers.size())
                banks.list.selectRow (selected);
            else if (! userLayers.empty())
                banks.list.selectRow (0);
            refreshMembers();
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Colours::screen);
            drawSectionBox (g, bankArea, "USER BANKS");
            drawSectionBox (g, memberArea, "FADERS IN THE BANK");
            drawSectionBox (g, boardArea, "CONSOLE (double-click to add)");
        }

        void resized() override
        {
            auto b = getLocalBounds().reduced (4);
            bankArea = b.removeFromLeft (b.getWidth() / 4);
            memberArea = b.removeFromLeft (b.getWidth() / 2);
            boardArea = b;

            auto r = bankArea.reduced (8).withTrimmedTop (16);
            auto buttons = r.removeFromBottom (28);
            layoutButtons (buttons, { &newButton, &renameButton, &deleteButton });
            banks.list.setBounds (r.withTrimmedBottom (4));

            r = memberArea.reduced (8).withTrimmedTop (16);
            buttons = r.removeFromBottom (28);
            layoutButtons (buttons, { &removeButton, &upButton, &downButton });
            members.list.setBounds (r.withTrimmedBottom (4));

            r = boardArea.reduced (8).withTrimmedTop (16);
            addButton.setBounds (r.removeFromBottom (28).removeFromLeft (120));
            board.list.setBounds (r.withTrimmedBottom (4));
        }

    private:
        struct ListModel final : public juce::ListBoxModel
        {
            std::function<int()> getNumRowsFn;
            std::function<void (int, juce::Graphics&, int, int, bool)> paintFn;
            std::function<void (int)> selectedFn, doubleClickFn;

            int getNumRows() override { return getNumRowsFn ? getNumRowsFn() : 0; }
            void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool sel) override
            {
                if (paintFn && row < getNumRows())
                    paintFn (row, g, w, h, sel);
            }
            void selectedRowsChanged (int row) override { if (selectedFn) selectedFn (row); }
            void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { if (doubleClickFn) doubleClickFn (row); }
        };

        struct List
        {
            List()
            {
                list.setModel (&model);
                list.setRowHeight (22);
                list.setColour (juce::ListBox::backgroundColourId, juce::Colours::black);
            }
            ~List() { list.setModel (nullptr); }
            ListModel model;
            juce::ListBox list;
        };

        static void paintRow (juce::Graphics& g, int w, int h, bool sel, const juce::String& text, const juce::String& right, juce::Colour c)
        {
            if (sel)
                g.fillAll (Colours::accent.withAlpha (0.35f));
            auto r = juce::Rectangle<int> (w, h).reduced (8, 0);
            g.setColour (c);
            g.fillRect (r.removeFromLeft (3).reduced (0, 4));
            r.removeFromLeft (6);
            g.setColour (Colours::text);
            g.setFont (juce::FontOptions (13.0f));
            g.drawText (text, r, juce::Justification::centredLeft);
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText (right, r, juce::Justification::centredRight);
        }

        static void layoutButtons (juce::Rectangle<int> r, std::initializer_list<juce::Button*> buttons)
        {
            const int w = r.getWidth() / (int) buttons.size();
            for (auto* b : buttons)
                b->setBounds (r.removeFromLeft (w).reduced (2, 0));
        }

        const Layer* currentLayer() const
        {
            const int row = banks.list.getSelectedRow();
            return juce::isPositiveAndBelow (row, (int) userLayers.size()) ? &userLayers[(size_t) row] : nullptr;
        }

        void refreshMembers()
        {
            members.list.updateContent();
            members.list.repaint();
            const bool has = currentLayer() != nullptr;
            for (auto* b : { &deleteButton, &renameButton, &addButton, &removeButton, &upButton, &downButton })
                b->setEnabled (has);
        }

        void refreshAfterEdit()
        {
            const int memberRow = members.list.getSelectedRow();
            userLayers = ctx.show.layers.getUserLayers();
            banks.list.repaint();
            refreshMembers();
            members.list.selectRow (memberRow);
            notifyLayersChangedAsync (ctx);
        }

        void addSelected()
        {
            const auto* l = currentLayer();
            if (l == nullptr)
                return;
            const auto id = l->id;
            const auto rows = board.list.getSelectedRows();
            for (int i = 0; i < rows.size(); ++i)
                if (juce::isPositiveAndBelow (rows[i], (int) boardFaders.size()))
                    ctx.show.layers.addToUserLayer (id, boardFaders[(size_t) rows[i]]);
            refreshAfterEdit();
        }

        void moveMember (int delta)
        {
            const auto* l = currentLayer();
            const int row = members.list.getSelectedRow();
            if (l == nullptr || row < 0)
                return;
            ctx.show.layers.moveWithinUserLayer (l->id, row, delta);
            userLayers = ctx.show.layers.getUserLayers();
            refreshMembers();
            members.list.selectRow (juce::jlimit (0, juce::jmax (0, members.list.getListBoxModel()->getNumRows() - 1), row + delta));
            notifyLayersChangedAsync (ctx);
        }

        UiContext& ctx;
        List banks, members, board;
        std::vector<Layer> userLayers;
        std::vector<state::FaderRef> boardFaders;
        juce::TextButton newButton { "+ NEW" }, deleteButton { "DELETE" }, renameButton { "RENAME" }, addButton { "< ADD" },
            removeButton { "REMOVE" }, upButton { "UP" }, downButton { "DOWN" };
        juce::Rectangle<int> bankArea, memberArea, boardArea;
    };
} // namespace

LibraryPage::LibraryPage (UiContext& c) : ctx (c)
{
    tabs.setTabBarDepth (30);
    const auto tabColour = Colours::screenPanel;
    tabs.addTab ("SHOW", tabColour, new ShowTab (ctx), true);
    showPanel = new ShowPanel (ctx);
    tabs.addTab ("SCENES / SNIPPETS", tabColour, showPanel, true);
    tabs.addTab ("USER BANKS", tabColour, new BanksTab (ctx), true);
    tabs.setCurrentTabIndex (1);
    addAndMakeVisible (tabs);
}

LibraryPage::~LibraryPage() = default;

void LibraryPage::showScenes()
{
    tabs.setCurrentTabIndex (1);
}

void LibraryPage::refresh()
{
    showPanel->refresh();
    for (int i = 0; i < tabs.getNumTabs(); ++i)
        if (auto* b = dynamic_cast<BanksTab*> (tabs.getTabContentComponent (i)))
            b->refresh();
}

void LibraryPage::paint (juce::Graphics& g)
{
    g.fillAll (Colours::screen);
}

void LibraryPage::resized()
{
    tabs.setBounds (getLocalBounds());
}

} // namespace livedaw::ui
