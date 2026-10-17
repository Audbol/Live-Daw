#include "ui/Pages.h"
#include "ui/ScopeWindow.h"

namespace livedaw::ui
{

namespace
{
    /** Rows of outlined keys on dark bands, as on the LIBRARY tabs. */
    struct KeyRow
    {
        juce::Rectangle<int> band;
        std::vector<juce::Component*> left, right;
    };

    void layoutKeyRows (juce::Rectangle<int> area, std::vector<KeyRow>& rows, int rowH, int gap)
    {
        for (auto& row : rows)
        {
            row.band = area.removeFromTop (rowH);
            area.removeFromTop (gap);
            auto r = row.band.reduced (4, 4);
            const int w = juce::jmin (150, r.getWidth() * 30 / 100);
            for (auto* c : row.left)
            {
                c->setBounds (r.removeFromLeft (w));
                r.removeFromLeft (4);
            }
            for (auto* c : row.right)
            {
                c->setBounds (r.removeFromRight (w));
                r.removeFromRight (4);
            }
        }
    }

    void paintKeyRows (juce::Graphics& g, const std::vector<KeyRow>& rows)
    {
        for (auto& row : rows)
        {
            g.setColour (Dark::body);
            g.fillRect (row.band);
        }
    }

    WingKey* makeKey (juce::OwnedArray<WingKey>& keys, juce::Component& parent, const juce::String& text, Icon icon, bool enabled = true)
    {
        auto* k = keys.add (new WingKey (text, icon));
        styleIconKey (*k);
        enableKey (*k, enabled);
        parent.addAndMakeVisible (k);
        return k;
    }
} // namespace

//==============================================================================
/** SHOW (fig. 44/45): show files. A show is always open here, so OPEN and
    CREATE stay available; CLOSE starts a new show. */
class LibraryPage::ShowTab final : public juce::Component
{
public:
    explicit ShowTab (LibraryPage& p) : page (p)
    {
        auto& ctx = page.ctx;
        makeKey (keys, *this, "OPEN", Icon::Folder)->onClick = [&ctx] { if (ctx.openShow) ctx.openShow(); };
        makeKey (keys, *this, "CREATE", Icon::Edit)->onClick = [&ctx] { if (ctx.newShow) ctx.newShow(); };
        makeKey (keys, *this, "SAVE", Icon::SaveTray)->onClick = [&ctx] { if (ctx.saveShow) ctx.saveShow (false); };
        makeKey (keys, *this, "CLOSE", Icon::ClearCircle)->onClick = [&ctx] { if (ctx.newShow) ctx.newShow(); };
        makeKey (keys, *this, "SAVE AS", Icon::SaveTray)->onClick = [&ctx] { if (ctx.saveShow) ctx.saveShow (true); };
        makeKey (keys, *this, "ADD ITEM\nTO SHOW", Icon::Plus, false); // needs the file browser (G-N3)
        makeKey (keys, *this, "EDIT\nTAG", Icon::Tag)->onClick = [this] { editTag(); };
        makeKey (keys, *this, "SEND\nMIDI", Icon::Edit, false);        // needs MIDI (G-N1)
        // WING files (Live DAW): snapshots apply to the console, shows add scenes.
        // IMPORT WING SNAP/SHOW... and EXPORT WING SNAP... (the file dialogs say which).
        makeKey (keys, *this, "IMPORT\nWING", Icon::LoadTray)->onClick = [this] { importWing(); };
        makeKey (keys, *this, "EXPORT\nWING", Icon::SaveTray)->onClick = [this] { exportWing(); };
        rows = { { {}, { keys[0], keys[1] }, { keys[2] } }, { {}, { keys[3] }, { keys[4] } }, { {}, { keys[5] }, { keys[7], keys[6] } },
                 { {}, { keys[8] }, { keys[9] } } };
    }

    void paint (juce::Graphics& g) override
    {
        paintKeyRows (g, rows);
        auto& ctx = page.ctx;
        g.setColour (Dark::textDim);
        g.setFont (juce::FontOptions (juce::jmin (16.0f, (float) info.getHeight() * 0.4f), juce::Font::bold));
        g.drawFittedText ("SHOW: " + ctx.show.getName().toUpperCase() + "\n" + juce::String (ctx.show.getNumScenes()) + " SCENES", info,
                          juce::Justification::bottomLeft, 2);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        const int rowH = juce::jlimit (36, 56, getHeight() / 5);
        layoutKeyRows (b, rows, rowH, 4);
        info = b.withTop (rows.back().band.getBottom() + 8).reduced (8, 4);
    }

private:
    /** EDIT TAG: the selected scene's tag, #1..#128 (recalled by tag from MIDI or a custom control). */
    void editTag()
    {
        const int index = page.selectedScene();
        const auto scene = page.ctx.show.getScene (index);
        if (! scene)
            return;
        // The tag number on the keyboard window (empty for none).
        popups::TextOptions options;
        options.icon = Icon::Tag;
        options.maxLength = 3;
        juce::Component::SafePointer<ShowTab> safe (this);
        popups::editText (page, "EDIT TAG (1-" + juce::String (Show::maxSceneTag) + "): " + scene->name.toUpperCase(),
                          scene->tag > 0 ? juce::String (scene->tag) : juce::String(), [safe, index] (const juce::String& text)
        {
            if (safe == nullptr)
                return;
            safe->page.ctx.show.setSceneTag (index, text.retainCharacters ("0123456789").getIntValue());
            safe->page.list->refresh();
        }, options);
    }

    /** IMPORT WING SNAP/SHOW: a .snap is applied to the console, a .show's
        snapshots become scenes. The log gets what could not be applied. */
    void importWing()
    {
        chooser = std::make_unique<juce::FileChooser> ("Import a WING snapshot or show",
                                                       juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.snap;*.show");
        juce::Component::SafePointer<ShowTab> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            if (safe == nullptr || file == juce::File())
                return;
            auto& ctx = safe->page.ctx;
            juce::String report;
            const auto result = file.hasFileExtension ("show") ? ctx.engine.getWing().importShow (ctx.show, file, report)
                                                               : ctx.engine.getWing().importSnapshot (file, report);
            juce::StringArray lines;
            lines.addLines (report);
            lines.removeEmptyStrings();
            if (result.failed())
                ctx.log.error (file.getFileName() + ": " + lines.joinIntoString ("; "));
            else if (! lines.isEmpty())
            {
                ctx.log.info (file.getFileName() + ": " + lines[0]);
                for (int i = 1; i < lines.size(); ++i)
                    ctx.log.warning (lines[i].trim());
            }
            if (ctx.structureChanged)
                ctx.structureChanged();
            safe->page.list->refresh();
            safe->repaint();
        });
    }

    /** EXPORT WING SNAP: the console as a WING snapshot (experimental). */
    void exportWing()
    {
        auto& ctx = page.ctx;
        const auto name = juce::File::createLegalFileName (ctx.show.getName().isNotEmpty() ? ctx.show.getName() : juce::String ("Live DAW"));
        chooser = std::make_unique<juce::FileChooser> ("Export a WING snapshot (experimental)",
                                                       juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (name + ".snap"),
                                                       "*.snap");
        juce::Component::SafePointer<ShowTab> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safe] (const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (safe == nullptr || file == juce::File())
                return;
            file = file.withFileExtension ("snap");
            auto& c = safe->page.ctx;
            const auto result = c.engine.getWing().exportSnapshot (file);
            if (result.wasOk())
                c.log.info ("Exported WING snapshot (experimental): " + file.getFullPathName());
            else
                c.log.error ("WING snapshot export failed: " + result.getErrorMessage());
        });
    }

    LibraryPage& page;
    juce::OwnedArray<WingKey> keys;
    std::vector<KeyRow> rows;
    juce::Rectangle<int> info;
    std::unique_ptr<juce::FileChooser> chooser;
};

//==============================================================================
/** SNAP (fig. 46): our scenes are the show's snapshots. */
class LibraryPage::SnapTab final : public juce::Component, private juce::Timer
{
public:
    explicit SnapTab (LibraryPage& p) : page (p)
    {
        save = makeKey (keys, *this, "SAVE", Icon::SaveTray);
        saveScope = makeKey (keys, *this, "SAVE\n+SCOPE", Icon::SaveTray);
        update = makeKey (keys, *this, "UPDATE", Icon::Edit);
        partialUpdate = makeKey (keys, *this, "PARTIAL\nUPDATE", Icon::Edit, false); // engine E12
        editScope = makeKey (keys, *this, "EDIT\nSCOPE", Icon::Overview);
        loadKey = makeKey (keys, *this, "LOAD", Icon::LoadTray);
        partialLoad = makeKey (keys, *this, "PARTIAL\nLOAD", Icon::LoadTray, false); // engine E12
        restore = makeKey (keys, *this, "RESTORE", Icon::Reset, false);              // engine E12
        rows = { { {}, { save, saveScope }, {} }, { {}, { update, partialUpdate }, { editScope } }, { {}, { loadKey, partialLoad }, { restore } } };

        save->onClick = [this] { addScene (false); };
        saveScope->onClick = [this] { addScene (true); };
        update->onClick = [this]
        {
            const int index = page.selectedScene();
            if (const auto scene = page.ctx.show.getScene (index))
                popups::confirm (page, "UPDATE", "Overwrite \"" + scene->name + "\" with the current console?", "UPDATE", [this, index]
                {
                    page.ctx.show.storeScene (index);
                    page.ctx.log.info ("Updated scene " + juce::String (index + 1));
                });
        };
        editScope->onClick = [this] { openScope (page.selectedScene()); };
        loadKey->onClick = [this]
        {
            if (page.selectedScene() >= 0)
                page.load ({ page.selectedScene(), -1 });
        };

        // App extras: the fade time and notes of the selected scene.
        fade.setSliderStyle (juce::Slider::LinearHorizontal);
        fade.setRange (0.0, 30.0, 0.1);
        fade.setSkewFactorFromMidPoint (3.0);
        fade.setTextValueSuffix (" s");
        fade.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 24);
        fade.onValueChange = [this]
        {
            if (page.selectedScene() >= 0)
            {
                page.ctx.show.setSceneFadeSeconds (page.selectedScene(), fade.getValue());
                page.list->repaint();
            }
        };
        notes.setMultiLine (true);
        notes.setReturnKeyStartsNewLine (true);
        notes.setColour (juce::TextEditor::backgroundColourId, Dark::well);
        notes.setColour (juce::TextEditor::outlineColourId, Dark::key);
        notes.onTextChange = [this]
        {
            if (page.selectedScene() >= 0)
                page.ctx.show.setSceneNotes (page.selectedScene(), notes.getText());
        };
        addAndMakeVisible (fade);
        addAndMakeVisible (notes);
        startTimerHz (2);
    }

    void refresh()
    {
        const auto scene = page.ctx.show.getScene (page.selectedScene());
        for (auto* k : { update, editScope, loadKey })
            enableKey (*k, scene.has_value());
        fade.setEnabled (scene.has_value());
        notes.setEnabled (scene.has_value());
        if (! fade.isMouseButtonDown())
            fade.setValue (scene ? scene->fadeSeconds : 0.0, juce::dontSendNotification);
        if (! notes.hasKeyboardFocus (true))
            notes.setText (scene ? scene->notes : juce::String(), false);
    }

    void paint (juce::Graphics& g) override
    {
        paintKeyRows (g, rows);
        auto body = drawDarkPanel (g, extras, "SCENE");
        juce::ignoreUnused (body);
        g.setColour (Dark::textDim);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText ("FADE (FADERS, PANS, SENDS)", fadeLabel, juce::Justification::centredLeft);
        g.drawText ("NOTES", notesLabel, juce::Justification::bottomLeft);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        const int rowH = juce::jlimit (36, 56, getHeight() / 6);
        layoutKeyRows (b, rows, rowH, 4);
        extras = b.withTop (rows.back().band.getBottom() + 8);
        auto e = darkPanelBody (extras, true).reduced (8, 4);
        auto f = e.removeFromTop (28);
        fadeLabel = f.removeFromLeft (juce::jmin (190, f.getWidth() / 2));
        fade.setBounds (f);
        notesLabel = e.removeFromTop (18);
        notes.setBounds (e.withTrimmedTop (2));
    }

private:
    void timerCallback() override { refresh(); }

    void addScene (bool withScope)
    {
        auto& show = page.ctx.show;
        const int at = page.selectedScene() >= 0 ? page.selectedScene() + 1 : show.getNumScenes();
        const int index = show.addScene ("Scene " + juce::String (show.getNumScenes() + 1), at);
        page.list->refresh();
        page.list->select ({ index, -1 });
        if (withScope)
            openScope (index);
    }

    void openScope (int index)
    {
        const auto scene = page.ctx.show.getScene (index);
        if (! scene)
            return;
        page.openScope ((int) ScopeWindow::Mode::SceneScope, "EDIT SCOPE: " + scene->name.toUpperCase(), scene->scope, {},
                        [this, index] (juce::uint32 sections, const std::vector<state::StripRef>&)
                        {
                            page.ctx.show.setSceneScope (index, sections);
                        });
    }

    LibraryPage& page;
    juce::OwnedArray<WingKey> keys;
    WingKey *save, *saveScope, *update, *partialUpdate, *editScope, *loadKey, *partialLoad, *restore;
    std::vector<KeyRow> rows;
    juce::Slider fade;
    juce::TextEditor notes;
    juce::Rectangle<int> extras, fadeLabel, notesLabel;
};

//==============================================================================
/** SNIP (fig. 49): snippets of the selected scene. SAVE captures the REC FOCUS
    (strips x sections); REC / STOP wait for parameter recording (engine E14). */
class LibraryPage::SnipTab final : public juce::Component, private juce::Timer
{
public:
    explicit SnipTab (LibraryPage& p) : page (p)
    {
        save = makeKey (keys, *this, "SAVE", Icon::SaveTray);
        update = makeKey (keys, *this, "UPDATE", Icon::Edit);
        loadFocus = makeKey (keys, *this, "LOAD\nFOCUS", Icon::LoadTray);
        loadKey = makeKey (keys, *this, "LOAD", Icon::LoadTray);
        partialLoad = makeKey (keys, *this, "PARTIAL\nLOAD", Icon::LoadTray, false); // engine E12
        rec = makeKey (keys, *this, "REC", Icon::Record, false);                      // engine E14
        stop = makeKey (keys, *this, "STOP", Icon::Stop, false);                      // engine E14
        recFocus = makeKey (keys, *this, "REC\nFOCUS", Icon::Overview);
        addItems = makeKey (keys, *this, "ADD\nITEMS", Icon::Plus);
        removeItems = makeKey (keys, *this, "REMOVE\nITEMS", Icon::MinusCircle);
        clearAll = makeKey (keys, *this, "CLEAR\nALL", Icon::Trash);
        recFocus->isOn = [this] { return page.focusEdited; };
        rows = { { {}, { save }, {} }, { {}, { update }, { loadFocus } }, { {}, { loadKey, partialLoad }, {} }, { {}, { rec, stop }, { recFocus } },
                 { {}, { addItems, removeItems }, { clearAll } } };

        save->onClick = [this] { addSnippet(); };
        update->onClick = [this]
        {
            const auto item = page.list->getSelected();
            if (item.isSnippet())
            {
                page.ctx.show.storeSnippet (item.scene, item.snippet);
                page.ctx.log.info ("Updated snippet " + juce::String (item.scene + 1) + "." + juce::String (item.snippet + 1));
            }
        };
        loadFocus->onClick = [this]
        {
            const auto item = page.list->getSelected();
            if (const auto sn = page.ctx.show.getSnippet (item.scene, item.snippet))
            {
                page.focusSections = sn->sections;
                page.focusStrips = sn->strips;
                page.focusEdited = true;
                recFocus->repaint();
            }
        };
        loadKey->onClick = [this]
        {
            const auto item = page.list->getSelected();
            if (item.isSnippet())
                page.load (item);
        };
        recFocus->onClick = [this]
        {
            auto [sections, strips] = defaultFocus();
            page.openScope ((int) ScopeWindow::Mode::SnippetFocus, "REC FOCUS", sections, strips,
                            [this] (juce::uint32 s, const std::vector<state::StripRef>& st)
                            {
                                page.focusSections = s;
                                page.focusStrips = st;
                                page.focusEdited = true;
                            });
        };
        addItems->onClick = [this] { editItems (true); };
        removeItems->onClick = [this] { editItems (false); };
        clearAll->onClick = [this]
        {
            const auto item = page.list->getSelected();
            if (const auto sn = page.ctx.show.getSnippet (item.scene, item.snippet))
                popups::confirm (page, "CLEAR ALL", "Remove every item from \"" + sn->name + "\"?", "CLEAR", [this, item]
                {
                    page.ctx.show.setSnippetStrips (item.scene, item.snippet, {});
                    page.list->refresh();
                }, true);
        };
        startTimerHz (2);
    }

    void refresh()
    {
        const auto item = page.list->getSelected();
        const bool snippet = item.isSnippet();
        enableKey (*save, page.ctx.show.getScene (item.scene).has_value());
        for (auto* k : { update, loadFocus, loadKey, addItems, removeItems, clearAll })
            enableKey (*k, snippet);
        repaint (counter);
    }

    void paint (juce::Graphics& g) override
    {
        paintKeyRows (g, rows);
        // The parameter counter (the selected snippet's strips x sections).
        const auto item = page.list->getSelected();
        const auto sn = page.ctx.show.getSnippet (item.scene, item.snippet);
        int count = 0;
        if (sn)
            for (int i = 0; i < state::kNumSections; ++i)
                count += (sn->sections & state::sectionAt (i)) != 0 ? (int) sn->strips.size() : 0;
        g.setColour (juce::Colours::black);
        g.fillRect (counter);
        auto c = counter.reduced (2);
        g.setColour (Dark::text);
        g.setFont (juce::FontOptions (juce::jmin (22.0f, (float) c.getHeight() * 0.6f), juce::Font::bold));
        g.drawText (juce::String (count), c.removeFromTop (c.getHeight() * 62 / 100), juce::Justification::centred);
        g.setColour (Dark::orange.brighter (0.4f));
        g.setFont (juce::FontOptions (juce::jmin (11.0f, (float) c.getHeight() * 0.9f), juce::Font::bold));
        g.drawText ("PARAMETER", c, juce::Justification::centredTop);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        const int rowH = juce::jlimit (32, 52, getHeight() / 6);
        layoutKeyRows (b, rows, rowH, 4);
        // REC and STOP are narrower, to make room for the parameter counter.
        auto band = rows[3].band.reduced (4, 4);
        const int w = juce::jmin (rec->getWidth(), (recFocus->getX() - band.getX() - 90) / 2 - 4);
        rec->setBounds (rec->getBounds().withWidth (w));
        stop->setBounds (stop->getBounds().withX (rec->getRight() + 4).withWidth (w));
        counter = juce::Rectangle<int> (stop->getRight() + 8, band.getY(), juce::jmin (100, recFocus->getX() - stop->getRight() - 16), band.getHeight());
    }

private:
    void timerCallback() override { refresh(); }

    std::pair<juce::uint32, std::vector<state::StripRef>> defaultFocus() const
    {
        if (page.focusEdited)
            return { page.focusSections, page.focusStrips };
        // Until the focus is edited: the selected channel's fader and sends.
        std::vector<state::StripRef> strips;
        if (auto* s = page.ctx.getSelectedStrip ? page.ctx.getSelectedStrip() : nullptr)
            strips.push_back (state::StripRef::of (*s));
        return { state::Fader | state::Sends, strips };
    }

    void addSnippet()
    {
        auto& show = page.ctx.show;
        const auto item = page.list->getSelected();
        const auto scene = show.getScene (item.scene);
        if (! scene)
            return;
        auto [sections, strips] = defaultFocus();
        const int index = show.addSnippet (item.scene, "Snippet " + juce::String (scene->numSnippets + 1), sections, strips);
        page.list->refresh();
        page.list->select ({ item.scene, index });
    }

    void editItems (bool add)
    {
        const auto item = page.list->getSelected();
        const auto sn = page.ctx.show.getSnippet (item.scene, item.snippet);
        if (! sn)
            return;
        page.openScope ((int) (add ? ScopeWindow::Mode::SnippetAdd : ScopeWindow::Mode::SnippetRemove), (add ? "ADD ITEMS: " : "REMOVE ITEMS: ") + sn->name.toUpperCase(),
                        0, {}, [this, item, add] (juce::uint32 sections, const std::vector<state::StripRef>& chosen)
                        {
                            auto& show = page.ctx.show;
                            const auto current = show.getSnippet (item.scene, item.snippet);
                            if (! current)
                                return;
                            auto strips = current->strips;
                            for (auto& r : chosen)
                            {
                                strips.erase (std::remove (strips.begin(), strips.end(), r), strips.end());
                                if (add)
                                    strips.push_back (r);
                            }
                            show.setSnippetStrips (item.scene, item.snippet, strips);
                            show.setSnippetSections (item.scene, item.snippet, add ? (current->sections | sections) : (current->sections & ~sections));
                            // Added items take the console's current settings.
                            if (add)
                                show.storeSnippet (item.scene, item.snippet);
                            page.list->refresh();
                        });
    }

    LibraryPage& page;
    juce::OwnedArray<WingKey> keys;
    WingKey *save, *update, *loadFocus, *loadKey, *partialLoad, *rec, *stop, *recFocus, *addItems, *removeItems, *clearAll;
    std::vector<KeyRow> rows;
    juce::Rectangle<int> counter;
};

//==============================================================================
/** CLIP, FX and CHAN: the clip player (chapter 8), FX presets (G-N4) and
    channel presets (G-N5) aren't built yet; their tabs show the console's
    layout dimmed. */
class LibraryPage::PlaceholderTab final : public juce::Component
{
public:
    PlaceholderTab (LibraryPage& p, int which) : page (p), tab (which)
    {
        if (tab == TabClip)
        {
            for (auto [text, icon] : { std::pair { "PLAY", Icon::Play }, std::pair { "PAUSE", Icon::Pause }, std::pair { "STOP", Icon::Stop } })
                makeKey (keys, *this, text, icon, false);
            rows = { { {}, { keys[0], keys[1] }, { keys[2] } } };
        }
        else if (tab == TabFx)
        {
            makeKey (keys, *this, "SAVE", Icon::SaveTray, false);
            makeKey (keys, *this, "LOAD", Icon::LoadTray, false);
            rows = { { {}, { keys[0] }, {} }, { {}, { keys[1] }, {} } };
        }
        else
        {
            makeKey (keys, *this, "SAVE", Icon::SaveTray, false);
            makeKey (keys, *this, "UPDATE\nSCOPE", Icon::Overview, false);
            makeKey (keys, *this, "LOAD", Icon::LoadTray, false);
            rows = { { {}, { keys[0], keys[1] }, {} }, { {}, { keys[2] }, {} } };
        }
    }

    void paint (juce::Graphics& g) override
    {
        paintKeyRows (g, rows);
        auto r = graphic;
        const float dim = 0.4f;
        if (tab == TabClip)
        {
            // The 4-track player: meter, status, time, reels and format chips.
            g.setColour (Dark::body);
            g.fillRect (r);
            auto meter = r.removeFromLeft (r.getHeight() * 2 / 3).reduced (6);
            g.setColour (juce::Colours::black);
            g.fillRect (meter);
            auto body = r.reduced (8, 6);
            auto status = body.removeFromLeft (14).withHeight (14).toFloat();
            g.setColour (Dark::on.withAlpha (dim));
            g.fillRect (status);
            body.removeFromLeft (8);
            g.setColour (Dark::pre.withAlpha (dim));
            g.setFont (juce::FontOptions (juce::jmin (30.0f, (float) body.getHeight() * 0.36f), juce::Font::bold));
            g.drawText ("0:00:00", body.removeFromTop (body.getHeight() * 45 / 100), juce::Justification::centredLeft);
            for (int i = 0; i < 2; ++i)
            {
                const float d = (float) juce::jmin (body.getHeight() - 16, 44);
                auto reel = juce::Rectangle<float> ((float) body.getX() + (float) i * (d + 8.0f), (float) body.getY() + 4.0f, d, d);
                g.setColour (Dark::text.withAlpha (dim));
                g.drawEllipse (reel, 2.0f);
                g.fillEllipse (reel.withSizeKeepingCentre (d * 0.2f, d * 0.2f));
            }
            g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
            g.drawText ("4-TRACK PLAYER", body, juce::Justification::bottomRight);
            auto chips = juce::Rectangle<float> ((float) body.getRight() - 90.0f, (float) body.getY(), 90.0f, 34.0f);
            for (auto [i, text] : { std::pair { 0, "WAV" }, std::pair { 1, "4-CH" }, std::pair { 2, "48K" }, std::pair { 3, "24 bit" } })
                drawChip (g, juce::Rectangle<float> (chips.getX() + (float) (i % 2) * 46.0f, chips.getY() + (float) (i / 2) * 18.0f, 43.0f, 15.0f), text,
                          Dark::key.withAlpha (dim), Dark::text.withAlpha (dim));
        }
        else if (tab == TabFx)
        {
            // The preset and the slot it loads into.
            auto preset = r.removeFromLeft (r.getWidth() * 42 / 100).reduced (4, r.getHeight() / 5);
            auto arrow = r.removeFromLeft (30);
            auto slot = r.reduced (4, r.getHeight() / 6);
            g.setColour (Dark::key.withAlpha (dim));
            g.fillRect (preset);
            g.fillRect (slot);
            g.setColour (FileColours::fx.withAlpha (dim));
            g.fillRect (slot.removeFromTop (14));
            g.setColour (Dark::text.withAlpha (dim));
            g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
            g.drawText ("PRESET", preset, juce::Justification::centred);
            g.drawText (">>", arrow, juce::Justification::centred);
            g.drawText ("FX SLOT", slot, juce::Justification::centred);
        }
        else
        {
            // The channel preset's scope, laid out like a channel strip.
            const char* top[] = { "CONN", "CONFIG", "CUST", "TAGS", "", "SEND", "MAIN", "NONE", "ALL" };
            const char* second[] = { "IN", "FILTER", "DELAY", "GATE", "DYN", "EQ", "", "PAN", "FDR", "MUTE" };
            const float w = (float) r.getWidth() / 10.0f, h = juce::jmin (34.0f, (float) r.getHeight() / 4.0f);
            auto cell = [&] (int col, int row, const char* text)
            {
                if (*text == 0)
                    return;
                auto c = juce::Rectangle<float> ((float) r.getX() + w * (float) col, (float) r.getY() + h * (float) row, w, h).reduced (1.5f);
                g.setColour (Dark::on.withAlpha (0.25f));
                g.fillRect (c);
                g.setColour (Dark::text.withAlpha (dim));
                g.setFont (juce::FontOptions (juce::jmin (11.0f, h * 0.4f), juce::Font::bold));
                g.drawFittedText (text, c.toNearestInt(), juce::Justification::centredBottom, 1, 0.6f);
            };
            for (int i = 0; i < 9; ++i)
                cell (i, 0, top[i]);
            for (int i = 0; i < 10; ++i)
                cell (i, 1, second[i]);
            cell (0, 2, "PRE");
            cell (0, 3, "POST");
        }
        g.setColour (Dark::textDim);
        g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        const char* what = tab == TabClip ? "THE CLIP PLAYER IS NOT AVAILABLE YET." : tab == TabFx ? "FX PRESETS ARE NOT AVAILABLE YET." : "CHANNEL PRESETS ARE NOT AVAILABLE YET.";
        g.drawFittedText (what, note, juce::Justification::topLeft, 2);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        const int rowH = juce::jlimit (36, 56, getHeight() / 6);
        if (tab == TabClip)
        {
            graphic = b.removeFromTop (juce::jmin (120, b.getHeight() / 3));
            b.removeFromTop (4);
        }
        layoutKeyRows (b, rows, rowH, 4);
        b = b.withTop (rows.back().band.getBottom() + 8);
        if (tab != TabClip)
            graphic = b.removeFromTop (juce::jmin (150, b.getHeight() * 2 / 3)).reduced (4, 0);
        note = b.reduced (8, 6);
    }

private:
    LibraryPage& page;
    int tab;
    juce::OwnedArray<WingKey> keys;
    std::vector<KeyRow> rows;
    juce::Rectangle<int> graphic, note;
};

//==============================================================================
LibraryPage::LibraryPage (UiContext& c) : ctx (c)
{
    list = std::make_unique<ShowPanel> (ctx);
    list->onSelectionChanged = [this] (ShowPanel::Item item)
    {
        // A scene row opens SNAP, a snippet row SNIP (SHOW stays).
        if (tabs.getCurrent() == TabSnap && item.isSnippet())
            tabs.setCurrent (TabSnip);
        else if (tabs.getCurrent() == TabSnip && item.isValid() && ! item.isSnippet())
            tabs.setCurrent (TabSnap);
        if (auto* snap = dynamic_cast<SnapTab*> (tabPages[TabSnap]))
            snap->refresh();
        if (auto* snip = dynamic_cast<SnipTab*> (tabPages[TabSnip]))
            snip->refresh();
    };
    list->onActivated = [this] (ShowPanel::Item item) { load (item); };
    addAndMakeVisible (*list);

    tabs.tabFraction = 0.0f;
    tabs.setTabs ({ { "SHOW", FileColours::show }, { "SNAP", FileColours::snap }, { "SNIP", FileColours::snip }, { "CLIP", FileColours::clip },
                    { "FX", FileColours::fx }, { "CHAN", FileColours::chan } });
    tabs.onChange = [this] (int t) { showTab (t); };
    addAndMakeVisible (tabs);

    tabPages.add (new ShowTab (*this));
    tabPages.add (new SnapTab (*this));
    tabPages.add (new SnipTab (*this));
    tabPages.add (new PlaceholderTab (*this, TabClip));
    tabPages.add (new PlaceholderTab (*this, TabFx));
    tabPages.add (new PlaceholderTab (*this, TabChan));
    for (auto* p : tabPages)
        addChildComponent (p);

    for (auto* k : { &goPrevKey, &goNextKey, &prevKey, &nextKey, &goKey })
    {
        styleIconKey (*k);
        addAndMakeVisible (k);
    }
    goPrevKey.onClick = [this]
    {
        confirmLoad ("the previous scene", [this]
        {
            logRecall (ctx, "previous scene", ctx.show.recallPrevious());
            list->refresh();
            list->select ({ ctx.show.getCurrentSceneIndex(), -1 });
        });
    };
    goNextKey.onClick = [this]
    {
        confirmLoad ("the next scene", [this]
        {
            logRecall (ctx, "next scene", ctx.show.recallNext());
            list->refresh();
            list->select ({ ctx.show.getCurrentSceneIndex(), -1 });
        });
    };
    prevKey.onClick = [this] { list->select ({ juce::jmax (0, selectedScene() - 1), -1 }); };
    nextKey.onClick = [this] { list->select ({ juce::jmin (ctx.show.getNumScenes() - 1, selectedScene() + 1), -1 }); };
    goKey.onClick = [this] { load (list->getSelected()); };

    showTab (TabSnap);
    tabs.setCurrent (TabSnap, false);
    shownGlobal = anyGlobalSafe();
    startTimerHz (2);
}

LibraryPage::~LibraryPage() = default;

std::vector<StatusKey> LibraryPage::getStatusKeys()
{
    // GLOBAL: an open padlock without global safes, closed (lit) with some.
    const bool safe = anyGlobalSafe();
    return { { "GLOBAL", safe ? Icon::LockClosed : Icon::LockOpen, safe, [this] { openGlobal(); } } };
}

StatusTitle LibraryPage::getStatusTitle()
{
    // The active scene in the SNAP colour, with the show's name as the caption
    // (ALWAYS SHOW ACTIVE SCENE shows it on the other screens too).
    StatusTitle t;
    t.kind = StatusTitle::Kind::Text;
    t.icon = Icon::ShowFile;
    t.prefix = ctx.show.getName().toUpperCase();
    if (const auto scene = ctx.show.getScene (ctx.show.getCurrentSceneIndex()))
    {
        t.text = scene->name.toUpperCase();
        t.fill = FileColours::snap;
        t.ink = juce::Colours::black;
    }
    else
        t.text = "NO SCENE";
    return t;
}

bool LibraryPage::anyGlobalSafe() const
{
    for (int k = 0; k < kNumStripKinds; ++k)
        for (auto& s : ctx.engine.getStrips (static_cast<StripKind> (k)))
            if (s->params.recallSafe.load())
                return true;
    return false;
}

void LibraryPage::timerCallback()
{
    // The HOME SAFE key changes the safes too.
    if (anyGlobalSafe() != shownGlobal)
    {
        shownGlobal = ! shownGlobal;
        notifyStatusKeysChanged();
    }
    // The title tile shows the active scene.
    if (const auto title = getStatusTitle(); title.prefix + title.text != shownTitle)
    {
        shownTitle = title.prefix + title.text;
        notifyStatusKeysChanged();
    }
    updateTransport();
}

void LibraryPage::showScenes()
{
    tabs.setCurrent (TabSnap);
}

void LibraryPage::showTab (int t)
{
    for (int i = 0; i < tabPages.size(); ++i)
        tabPages[i]->setVisible (i == t);
    list->setTypeFilter (t == TabShow ? 0 : t == TabSnap ? 1 : t == TabSnip ? 2 : -1);
}

int LibraryPage::selectedScene() const
{
    const auto item = list->getSelected();
    if (item.isValid())
        return item.scene;
    return ctx.show.getCurrentSceneIndex();
}

void LibraryPage::updateTransport()
{
    // Dimmed while the show has no scenes.
    const bool any = ctx.show.getNumScenes() > 0;
    for (auto* k : { &goPrevKey, &goNextKey, &prevKey, &nextKey, &goKey })
        enableKey (*k, any);
}

void LibraryPage::confirmLoad (const juce::String& what, std::function<void()> action)
{
    if (! prefs::confirmLoad (ctx))
    {
        action();
        return;
    }
    popups::confirm (*this, "LOAD", "Load " + what + "?", "LOAD", std::move (action));
}

void LibraryPage::load (ShowPanel::Item item)
{
    if (item.isSnippet())
    {
        if (const auto sn = ctx.show.getSnippet (item.scene, item.snippet))
            confirmLoad ("\"" + sn->name + "\"", [this, item]
            {
                logRecall (ctx, "snippet " + juce::String (item.scene + 1) + "." + juce::String (item.snippet + 1), ctx.show.recallSnippet (item.scene, item.snippet));
            });
    }
    else if (const auto scene = ctx.show.getScene (item.scene))
        confirmLoad ("\"" + scene->name + "\"", [this, item]
        {
            logRecall (ctx, "scene " + juce::String (item.scene + 1), ctx.show.recallScene (item.scene));
            list->refresh();
        });
}

void LibraryPage::openScope (int scopeMode, const juce::String& scopeTitle, juce::uint32 sections, std::vector<state::StripRef> strips,
                             std::function<void (juce::uint32, const std::vector<state::StripRef>&)> save)
{
    auto window = std::make_unique<ScopeWindow> (ctx, static_cast<ScopeWindow::Mode> (scopeMode), scopeTitle, ScopeWindow::Selection { sections, std::move (strips) });
    window->onCancel = [this] { closeScope(); };
    window->onSave = [this, save] (const ScopeWindow::Selection& s)
    {
        if (save)
            save (s.sections, s.strips);
        closeScope();
    };
    scope = std::move (window);
    addAndMakeVisible (*scope);
    scope->setBounds (getLocalBounds());
}

void LibraryPage::closeScope()
{
    // Called from the window's own keys: delete it once the click is done.
    juce::Component::SafePointer<LibraryPage> safe (this);
    juce::MessageManager::callAsync ([safe]
    {
        if (safe != nullptr)
        {
            safe->scope.reset();
            safe->list->refresh();
            safe->notifyStatusKeysChanged();
        }
    });
}

void LibraryPage::openGlobal()
{
    // GLOBAL safes: strips that scene recalls leave alone (DCA, mute group, FX
    // and content safes are engine item E17).
    std::vector<state::StripRef> safes;
    for (int k = 0; k < kNumStripKinds; ++k)
        for (auto& s : ctx.engine.getStrips (static_cast<StripKind> (k)))
            if (s->params.recallSafe.load())
                safes.push_back (state::StripRef::of (*s));
    openScope ((int) ScopeWindow::Mode::Global, "GLOBAL SAFE", 0, safes, [this] (juce::uint32, const std::vector<state::StripRef>& chosen)
    {
        for (int k = 0; k < kNumStripKinds; ++k)
            for (auto& s : ctx.engine.getStrips (static_cast<StripKind> (k)))
                s->params.recallSafe = std::find (chosen.begin(), chosen.end(), state::StripRef::of (*s)) != chosen.end();
        shownGlobal = anyGlobalSafe();
    });
}

void LibraryPage::refresh()
{
    list->refresh();
    if (auto* snap = dynamic_cast<SnapTab*> (tabPages[TabSnap]))
        snap->refresh();
    if (auto* snip = dynamic_cast<SnipTab*> (tabPages[TabSnip]))
        snip->refresh();
    tabPages[TabShow]->repaint();
    updateTransport();
}

void LibraryPage::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
    g.setColour (Dark::header);
    g.fillRect (transportArea);
}

void LibraryPage::resized()
{
    auto b = getLocalBounds();
    list->setBounds (b.removeFromLeft (b.getWidth() * 51 / 100));
    b.removeFromLeft (6);
    rightPanel = b;
    tabs.setBounds (b.removeFromTop (juce::jlimit (36, 52, getHeight() * 11 / 100)));
    b.removeFromTop (4);
    transportArea = b.removeFromBottom (juce::jlimit (52, 96, getHeight() * 16 / 100));
    b.removeFromBottom (4);
    for (auto* p : tabPages)
        p->setBounds (b);

    // GO PREV, GO NEXT, a gap, PREV over NEXT, GO (icons over the text when narrow).
    auto t = transportArea.reduced (4);
    const int w = t.getWidth() / 5;
    for (auto* k : { &goPrevKey, &goNextKey, &goKey })
    {
        k->iconAbove = w < 120;
        k->setButtonText (k == &goKey ? "GO" : (w < 120 ? juce::String (k == &goPrevKey ? "GO PREV" : "GO NEXT") : juce::String (k == &goPrevKey ? "GO\nPREV" : "GO\nNEXT")));
    }
    goPrevKey.setBounds (t.removeFromLeft (w).reduced (2));
    goNextKey.setBounds (t.removeFromLeft (w).reduced (2));
    goKey.setBounds (t.removeFromRight (w).reduced (2));
    auto steps = t.removeFromRight (w);
    prevKey.setBounds (steps.removeFromTop (steps.getHeight() / 2).reduced (2, 1));
    nextKey.setBounds (steps.reduced (2, 1));

    if (scope != nullptr)
        scope->setBounds (getLocalBounds());
}

} // namespace livedaw::ui
