#include "ui/Pages.h"
#include "ui/ScopeWindow.h"
#include "engine/Models.h"

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

    WingKey* makeKey (juce::OwnedArray<WingKey>& keys, juce::Component& parent, const juce::String& text, Icon icon, bool enabled = true,
                      juce::Colour on = Colours::gold)
    {
        auto* k = keys.add (new WingKey (text, icon, on));
        styleIconKey (*k);
        enableKey (*k, enabled);
        parent.addAndMakeVisible (k);
        return k;
    }

    /** Asks for a preset name, then calls `save` with the file in `folder` (after
        an overwrite question when it exists). */
    void savePresetAs (juce::Component& host, const juce::String& title, const juce::String& initial, const juce::File& folder,
                       const juce::String& extension, std::function<void (const juce::File&, const juce::String&)> save)
    {
        popups::TextOptions options;
        options.maxLength = 32;
        options.actionText = "SAVE";
        juce::Component::SafePointer<juce::Component> safe (&host);
        popups::editText (host, title, initial, [safe, folder, extension, save] (const juce::String& text)
        {
            const auto name = text.trim();
            const auto fileName = juce::File::createLegalFileName (name);
            if (safe == nullptr || fileName.isEmpty())
                return;
            folder.createDirectory();
            const auto file = folder.getChildFile (fileName + extension);
            if (file.exists())
                popups::confirm (*safe, "SAVE", "Overwrite \"" + file.getFileName() + "\"?", "SAVE", [save, file, name] { save (file, name); });
            else
                save (file, name);
        }, options);
    }

    /** "CH 3 INSERT PRE": the strip using FX slot `slot`, if any. */
    juce::String fxUser (UiContext& ctx, int slot)
    {
        for (int k = 0; k < kNumStripKinds; ++k)
            for (auto& s : ctx.engine.getStrips (static_cast<StripKind> (k)))
            {
                if (s->params.preInsert.fx.load() == slot)
                    return s->getLabel().toUpperCase() + "  INSERT PRE";
                if (s->params.postInsert.fx.load() == slot && s->params.postInsert.mode.load() == 0)
                    return s->getLabel().toUpperCase() + "  INSERT POST";
            }
        return "NOT INSERTED";
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
        // OPEN: the show selected at INT / USB, else a file dialog.
        makeKey (keys, *this, "OPEN", Icon::Folder)->onClick = [this]
        {
            auto& c = page.ctx;
            const auto file = page.list->getSelectedFile();
            if (file.hasFileExtension (Show::fileExtension) && c.openShowFile)
                page.confirmLoad ("the show \"" + file.getFileNameWithoutExtension() + "\"", [&c, file] { c.openShowFile (file); });
            else if (c.openShow)
                c.openShow();
        };
        makeKey (keys, *this, "CREATE", Icon::Edit)->onClick = [&ctx] { if (ctx.newShow) ctx.newShow(); };
        makeKey (keys, *this, "SAVE", Icon::SaveTray)->onClick = [&ctx] { if (ctx.saveShow) ctx.saveShow (false); };
        makeKey (keys, *this, "CLOSE", Icon::ClearCircle)->onClick = [&ctx] { if (ctx.newShow) ctx.newShow(); };
        // SAVE AS: into the folder shown at INT / USB, else a file dialog.
        makeKey (keys, *this, "SAVE AS", Icon::SaveTray)->onClick = [this]
        {
            auto& c = page.ctx;
            if (page.list->getLocation() == ShowPanel::Location::Show || ! c.saveShowTo)
            {
                if (c.saveShow)
                    c.saveShow (true);
                return;
            }
            savePresetAs (page, "SAVE SHOW AS", c.show.getName(), page.list->getFolder(), Show::fileExtension,
                          [this] (const juce::File& file, const juce::String&)
                          {
                              page.ctx.saveShowTo (file);
                              page.list->selectFile (file);
                          });
        };
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
        partialUpdate = makeKey (keys, *this, "PARTIAL\nUPDATE", Icon::Edit);
        editScope = makeKey (keys, *this, "EDIT\nSCOPE", Icon::Overview);
        loadKey = makeKey (keys, *this, "LOAD", Icon::LoadTray);
        partialLoad = makeKey (keys, *this, "PARTIAL\nLOAD", Icon::LoadTray);
        restore = makeKey (keys, *this, "RESTORE", Icon::Reset);
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
        // PARTIAL UPDATE: a scope window; only its selection is stored (the scope is kept for next time).
        partialUpdate->onClick = [this]
        {
            const int index = page.selectedScene();
            const auto scene = page.ctx.show.getScene (index);
            if (! scene)
                return;
            page.openScope ((int) ScopeWindow::Mode::PartialUpdate, "PARTIAL UPDATE: " + scene->name.toUpperCase(), page.ctx.show.getPartialScope(),
                            [this, index] (const state::Scope& scope, bool)
                            {
                                page.ctx.show.storeScenePartial (index, scope);
                                page.ctx.log.info ("Updated part of scene " + juce::String (index + 1));
                            });
        };
        editScope->onClick = [this] { openScope (page.selectedScene()); };
        loadKey->onClick = [this]
        {
            if (page.selectedScene() >= 0)
                page.load ({ page.selectedScene(), -1 });
        };
        // PARTIAL LOAD: a scope window; NO GLOB ignores the GLOBAL safes for this load.
        partialLoad->onClick = [this]
        {
            const int index = page.selectedScene();
            const auto scene = page.ctx.show.getScene (index);
            if (! scene)
                return;
            page.openScope ((int) ScopeWindow::Mode::PartialLoad, "PARTIAL LOAD: " + scene->name.toUpperCase(), page.ctx.show.getPartialScope(),
                            [this, index] (const state::Scope& scope, bool noGlobal)
                            {
                                logRecall (page.ctx, "part of scene " + juce::String (index + 1) + (noGlobal ? " (NO GLOB)" : ""),
                                           page.ctx.show.recallScenePartial (index, scope, noGlobal));
                                page.list->refresh();
                            });
        };
        // RESTORE: the whole scene regardless of its scope, GLOBAL safes included (a red question).
        restore->onClick = [this]
        {
            const int index = page.selectedScene();
            if (const auto scene = page.ctx.show.getScene (index))
                popups::confirm (page, "RESTORE", "Restore all of \"" + scene->name + "\", ignoring its scope and the GLOBAL safes?", "RESTORE",
                                 [this, index]
                                 {
                                     logRecall (page.ctx, "all of scene " + juce::String (index + 1), page.ctx.show.restoreScene (index));
                                     page.list->refresh();
                                 },
                                 true);
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
        for (auto* k : { update, partialUpdate, editScope, loadKey, partialLoad, restore })
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

    /** EDIT SCOPE: what the scene recalls (strips, DCAs, mute groups, FX, console settings, contents). */
    void openScope (int index)
    {
        const auto scene = page.ctx.show.getScene (index);
        if (! scene)
            return;
        page.openScope ((int) ScopeWindow::Mode::SceneScope, "EDIT SCOPE: " + scene->name.toUpperCase(), page.ctx.show.getSceneRecallScope (index),
                        [this, index] (const state::Scope& scope, bool) { page.ctx.show.setSceneRecallScope (index, scope); });
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
/** SNIP (fig. 49): snippets of the selected scene. REC, change parameters,
    STOP and SAVE stores the changed parameters (within REC FOCUS when it was
    edited); without a recording SAVE captures the REC FOCUS. */
class LibraryPage::SnipTab final : public juce::Component, private juce::Timer
{
public:
    explicit SnipTab (LibraryPage& p) : page (p)
    {
        save = makeKey (keys, *this, "SAVE", Icon::SaveTray);
        update = makeKey (keys, *this, "UPDATE", Icon::Edit);
        loadFocus = makeKey (keys, *this, "LOAD\nFOCUS", Icon::LoadTray);
        loadKey = makeKey (keys, *this, "LOAD", Icon::LoadTray);
        partialLoad = makeKey (keys, *this, "PARTIAL\nLOAD", Icon::LoadTray);
        rec = makeKey (keys, *this, "REC", Icon::Record, true, Colours::mute);
        stop = makeKey (keys, *this, "STOP", Icon::Stop);
        recFocus = makeKey (keys, *this, "REC\nFOCUS", Icon::Overview, true, Dark::choice);
        addItems = makeKey (keys, *this, "ADD\nITEMS", Icon::Plus);
        removeItems = makeKey (keys, *this, "REMOVE\nITEMS", Icon::MinusCircle);
        clearAll = makeKey (keys, *this, "CLEAR\nALL", Icon::Trash);
        // REC FOCUS turns light blue once edited; REC lights red while recording.
        recFocus->isOn = [this] { return page.focusEdited; };
        rec->isOn = [this] { return page.ctx.show.isRecordingSnippet(); };
        rec->iconColour = Colours::text;
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
                page.focus = sn->scope;
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
        partialLoad->onClick = [this]
        {
            const auto item = page.list->getSelected();
            const auto sn = page.ctx.show.getSnippet (item.scene, item.snippet);
            if (! sn)
                return;
            page.openScope ((int) ScopeWindow::Mode::PartialLoad, "PARTIAL LOAD: " + sn->name.toUpperCase(), page.ctx.show.getPartialScope(),
                            [this, item] (const state::Scope& scope, bool)
                            {
                                logRecall (page.ctx, "part of snippet " + juce::String (item.scene + 1) + "." + juce::String (item.snippet + 1),
                                           page.ctx.show.recallSnippetPartial (item.scene, item.snippet, scope));
                            });
        };
        rec->onClick = [this]
        {
            if (page.ctx.show.isRecordingSnippet())
                return;
            page.ctx.show.startSnippetRecording();
            page.ctx.log.info ("Snippet recording started");
            refresh();
        };
        stop->onClick = [this]
        {
            const int n = page.ctx.show.stopSnippetRecording();
            page.ctx.log.info ("Snippet recording stopped: " + juce::String (n) + " parameters changed (SAVE stores them)");
            refresh();
        };
        recFocus->onClick = [this]
        {
            page.openScope ((int) ScopeWindow::Mode::SnippetFocus, "REC FOCUS", currentFocus(), [this] (const state::Scope& s, bool)
            {
                page.focus = s;
                page.focusEdited = true;
            });
        };
        addItems->onClick = [this] { editItems (true); };
        removeItems->onClick = [this] { editItems (false); };
        // CLEAR ALL: the parameters being recorded, else every item of the snippet.
        clearAll->onClick = [this]
        {
            auto& show = page.ctx.show;
            if (show.isRecordingSnippet() || show.hasRecordedSnippet())
            {
                show.clearSnippetRecording();
                refresh();
                return;
            }
            const auto item = page.list->getSelected();
            if (const auto sn = show.getSnippet (item.scene, item.snippet))
                popups::confirm (page, "CLEAR ALL", "Remove every item from \"" + sn->name + "\"?", "CLEAR", [this, item]
                {
                    auto everything = state::Scope::everything();
                    everything.allStrips = false;
                    page.ctx.show.removeSnippetItems (item.scene, item.snippet, everything);
                    page.list->refresh();
                }, true);
        };
        startTimerHz (4);
    }

    void refresh()
    {
        auto& show = page.ctx.show;
        const auto item = page.list->getSelected();
        const bool snippet = item.isSnippet();
        const bool recording = show.isRecordingSnippet(), recorded = show.hasRecordedSnippet();
        enableKey (*save, show.getScene (item.scene).has_value() && ! recording);
        for (auto* k : { update, loadFocus, loadKey, partialLoad, addItems, removeItems })
            enableKey (*k, snippet && ! recording);
        enableKey (*stop, recording); // REC stays lit (red) while recording
        enableKey (*clearAll, snippet || recording || recorded);
        rec->repaint();
        repaint (counter);
    }

    void paint (juce::Graphics& g) override
    {
        paintKeyRows (g, rows);
        // The parameter counter: changed since REC (or by the last STOP), else the selected snippet's.
        auto& show = page.ctx.show;
        int count = 0;
        if (show.isRecordingSnippet() || show.hasRecordedSnippet())
            count = show.getRecordedParameterCount();
        else if (const auto item = page.list->getSelected(); item.isSnippet())
            if (const auto sn = show.getSnippet (item.scene, item.snippet))
                count = sn->parameterCount;
        g.setColour (juce::Colours::black);
        g.fillRect (counter);
        auto c = counter.reduced (2);
        g.setColour (show.isRecordingSnippet() ? Colours::mute : Dark::text);
        g.setFont (juce::FontOptions (juce::jmin (22.0f, (float) c.getHeight() * 0.6f), juce::Font::bold));
        g.drawText (juce::String (count), c.removeFromTop (c.getHeight() * 62 / 100), juce::Justification::centred);
        g.setColour (Dark::orange.brighter (0.4f));
        g.setFont (juce::FontOptions (juce::jmin (11.0f, (float) c.getHeight() * 0.9f), juce::Font::bold));
        g.drawText (count == 1 ? "PARAMETER" : "PARAMETERS", c, juce::Justification::centredTop);
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
        // Narrow keys: the icon over the caption.
        for (auto* k : { rec, stop })
            k->iconAbove = w < 96;
        counter = juce::Rectangle<int> (stop->getRight() + 8, band.getY(), juce::jmin (100, recFocus->getX() - stop->getRight() - 16), band.getHeight());
    }

private:
    void timerCallback() override { refresh(); }

    /** The REC FOCUS: as edited, else the selected channel's fader and sends. */
    state::Scope currentFocus() const
    {
        if (page.focusEdited)
            return page.focus;
        std::vector<state::StripRef> strips;
        if (auto* s = page.ctx.getSelectedStrip ? page.ctx.getSelectedStrip() : nullptr)
            strips.push_back (state::StripRef::of (*s));
        return state::Scope::only (state::Fader | state::Sends, strips);
    }

    void addSnippet()
    {
        auto& show = page.ctx.show;
        const auto item = page.list->getSelected();
        const auto scene = show.getScene (item.scene);
        if (! scene)
            return;
        const auto name = "Snippet " + juce::String (scene->numSnippets + 1);
        int index = -1;
        if (show.hasRecordedSnippet())
        {
            const auto focus = currentFocus();
            index = show.addRecordedSnippet (item.scene, name, page.focusEdited ? &focus : nullptr);
        }
        else
            index = show.addSnippet (item.scene, name, currentFocus());
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
                        state::Scope::none(), [this, item, add] (const state::Scope& items, bool)
                        {
                            // Added items take the console's current settings.
                            if (add)
                                page.ctx.show.addSnippetItems (item.scene, item.snippet, items);
                            else
                                page.ctx.show.removeSnippetItems (item.scene, item.snippet, items);
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
/** FX (fig. 51): SAVE stores the chosen FX slot as a preset file in the folder
    shown on the left; LOAD loads the selected preset into it. The graphic shows
    the selected preset, then the slot (its device and the strip using it) with
    the slot chooser. */
class LibraryPage::FxTab final : public juce::Component, private juce::Timer
{
public:
    explicit FxTab (LibraryPage& p) : page (p)
    {
        save = makeKey (keys, *this, "SAVE", Icon::SaveTray);
        loadKey = makeKey (keys, *this, "LOAD", Icon::LoadTray);
        rows = { { {}, { save }, {} }, { {}, { loadKey }, {} } };
        styleIconKey (slotKey);
        addAndMakeVisible (slotKey);
        slotKey.onClick = [this] { chooseSlot(); };

        save->onClick = [this]
        {
            auto& fx = page.ctx.engine.getFx();
            auto* plugin = fx.getPlugin (slot);
            const auto device = plugin != nullptr ? plugin->getName() : models::model (models::Fx, fx.getModel (slot)).name;
            savePresetAs (page, "SAVE FX " + juce::String (slot + 1) + " AS PRESET", device, page.list->getFolder(), Show::fxPresetExtension,
                          [this] (const juce::File& file, const juce::String& name)
                          {
                              const auto r = page.ctx.show.saveFxPreset (slot, file, name);
                              if (r.failed())
                                  page.ctx.log.error (r.getErrorMessage());
                              else
                                  page.ctx.log.info ("Saved FX " + juce::String (slot + 1) + " as " + file.getFileName());
                              page.list->selectFile (file);
                              repaint();
                          });
        };
        loadKey->onClick = [this]
        {
            const auto file = page.list->getSelectedFile();
            if (! file.hasFileExtension (Show::fxPresetExtension))
                return;
            const auto r = page.ctx.show.loadFxPreset (slot, file);
            page.ctx.log.info ("Loaded " + file.getFileName() + " into FX " + juce::String (slot + 1));
            if (r.failed())
                for (auto& line : juce::StringArray::fromLines (r.getErrorMessage()))
                    page.ctx.log.warning (line);
            repaint();
        };
        startTimerHz (4);
    }

    void refresh()
    {
        const auto file = page.list->getSelectedFile();
        enableKey (*loadKey, file.hasFileExtension (Show::fxPresetExtension));
        enableKey (*save, page.ctx.engine.getFx().isLoaded (slot));
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        paintKeyRows (g, rows);
        auto& ctx = page.ctx;
        auto& fx = ctx.engine.getFx();

        // The selected preset.
        const auto file = page.list->getSelectedFile();
        const auto info = file.hasFileExtension (Show::fxPresetExtension) ? Show::readPreset (file) : Show::PresetInfo {};
        g.setColour (Dark::body);
        g.fillRect (presetBox);
        auto pb = presetBox.reduced (6);
        const auto presetHeader = pb.removeFromTop (18);
        g.setColour (Dark::header);
        g.fillRect (presetHeader);
        g.setColour (Dark::textDim);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText ("PRESET", presetHeader, juce::Justification::centred);
        g.setColour (info.kind == Show::PresetInfo::Kind::Fx ? Dark::text : Dark::textDim);
        g.setFont (juce::FontOptions (juce::jmin (18.0f, (float) pb.getHeight() * 0.3f), juce::Font::bold));
        g.drawFittedText (info.kind == Show::PresetInfo::Kind::Fx ? info.name.toUpperCase() : juce::String ("NO PRESET SELECTED"),
                          pb.removeFromTop (pb.getHeight() / 2), juce::Justification::centred, 2);
        g.setColour (FileColours::fx);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawFittedText (info.detail.toUpperCase(), pb, juce::Justification::centredTop, 1);

        g.setColour (Dark::text);
        g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
        g.drawText (">>", arrow, juce::Justification::centred);

        // The slot: purple header, device, the strip using it.
        g.setColour (Dark::body);
        g.fillRect (slotBox);
        auto sb = slotBox.reduced (6);
        auto header = sb.removeFromTop (20);
        g.setColour (FileColours::fx.darker (0.3f));
        g.fillRect (header);
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawText ("FX " + juce::String (slot + 1) + (slot < 8 ? "  PREMIUM" : ""), header.reduced (4, 0), juce::Justification::centredLeft);
        auto* plugin = fx.getPlugin (slot);
        const auto device = plugin != nullptr ? plugin->getName() : models::model (models::Fx, fx.getModel (slot)).name;
        g.setColour (fx.isLoaded (slot) ? Dark::text : Dark::textDim);
        g.setFont (juce::FontOptions (juce::jmin (18.0f, (float) sb.getHeight() * 0.3f), juce::Font::bold));
        g.drawFittedText (fx.isLoaded (slot) ? device.toUpperCase() : juce::String ("EMPTY"), sb.removeFromTop (sb.getHeight() / 2),
                          juce::Justification::centred, 2);
        g.setColour (Dark::textDim);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawFittedText (fxUser (ctx, slot), sb, juce::Justification::centredTop, 1);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        const int rowH = juce::jlimit (36, 56, getHeight() / 6);
        layoutKeyRows (b, rows, rowH, 4);
        auto g = b.withTop (rows.back().band.getBottom() + 12).withHeight (juce::jmin (140, b.getHeight() / 3)).reduced (4, 0);
        slotKey.setBounds (g.removeFromRight (juce::jmin (44, g.getWidth() / 10)).withSizeKeepingCentre (juce::jmin (44, g.getWidth() / 10), 40));
        presetBox = g.removeFromLeft (g.getWidth() * 44 / 100);
        arrow = g.removeFromLeft (36);
        slotBox = g;
    }

private:
    void timerCallback() override { refresh(); }

    void chooseSlot()
    {
        auto& fx = page.ctx.engine.getFx();
        juce::StringArray items;
        for (int i = 0; i < kNumFx; ++i)
        {
            auto* plugin = fx.getPlugin (i);
            const auto device = plugin != nullptr ? plugin->getName() : (fx.isLoaded (i) ? models::model (models::Fx, fx.getModel (i)).name : juce::String ("-"));
            items.add ("FX " + juce::String (i + 1) + "  " + device.toUpperCase());
        }
        popups::ListOptions options;
        options.columns = 2;
        options.caption = "FX SLOT";
        juce::Component::SafePointer<FxTab> safe (this);
        popups::chooseFrom (slotKey, items, slot, [safe] (int i)
        {
            if (safe != nullptr)
            {
                safe->slot = i;
                safe->refresh();
            }
        }, options);
    }

    LibraryPage& page;
    juce::OwnedArray<WingKey> keys;
    WingKey *save, *loadKey;
    WingKey slotKey { {}, Icon::ChevronDown };
    std::vector<KeyRow> rows;
    juce::Rectangle<int> presetBox, arrow, slotBox;
    int slot = 0;
};

//==============================================================================
/** CHAN (fig. 52): SAVE stores the selected strip with the scope below as a
    preset file in the folder on the left; UPDATE SCOPE changes the selected
    preset's scope; LOAD applies the selected preset (within the scope) to the
    selected strip. The box at the top right is the selected strip (▼ chooses
    another). The scope grid is laid out like a channel strip, yellow when
    included; keys that share one of our sections light together, CONN (the
    input patch) is never part of a channel preset. */
class LibraryPage::ChanTab final : public juce::Component, private juce::Timer
{
public:
    explicit ChanTab (LibraryPage& p) : page (p)
    {
        save = makeKey (keys, *this, "SAVE", Icon::SaveTray);
        updateScope = makeKey (keys, *this, "UPDATE\nSCOPE", Icon::Overview);
        loadKey = makeKey (keys, *this, "LOAD", Icon::LoadTray);
        rows = { { {}, { save, updateScope }, {} }, { {}, { loadKey }, {} } };
        styleIconKey (stripKey);
        addAndMakeVisible (stripKey);
        stripKey.onClick = [this] { chooseStrip(); };

        save->onClick = [this]
        {
            auto* s = selectedStrip();
            if (s == nullptr)
                return;
            savePresetAs (page, "SAVE " + s->getLabel().toUpperCase() + " AS PRESET", stripName (*s), page.list->getFolder(), Show::channelPresetExtension,
                          [this, ref = state::StripRef::of (*s)] (const juce::File& file, const juce::String& name)
                          {
                              const auto r = page.ctx.show.saveChannelPreset (ref, scope, file, name);
                              if (r.failed())
                                  page.ctx.log.error (r.getErrorMessage());
                              else
                                  page.ctx.log.info ("Saved channel preset " + file.getFileName());
                              page.list->selectFile (file);
                              shownFile = file;
                              repaint();
                          });
        };
        updateScope->onClick = [this]
        {
            const auto file = page.list->getSelectedFile();
            if (file.hasFileExtension (Show::channelPresetExtension) && Show::setPresetScope (file, scope))
                page.ctx.log.info ("Updated the scope of " + file.getFileName());
        };
        loadKey->onClick = [this]
        {
            const auto file = page.list->getSelectedFile();
            auto* s = selectedStrip();
            if (s == nullptr || ! file.hasFileExtension (Show::channelPresetExtension))
                return;
            const auto r = page.ctx.show.loadChannelPreset (state::StripRef::of (*s), file, scope);
            if (r.failed())
                page.ctx.log.warning (file.getFileName() + ": " + r.getErrorMessage());
            else
                page.ctx.log.info ("Loaded " + file.getFileName() + " onto " + s->getLabel());
        };
        startTimerHz (4);
    }

    void refresh()
    {
        // A newly selected preset shows its own scope.
        const auto file = page.list->getSelectedFile();
        if (file != shownFile)
        {
            shownFile = file;
            if (file.hasFileExtension (Show::channelPresetExtension))
            {
                const auto info = Show::readPreset (file);
                if (info.kind == Show::PresetInfo::Kind::Channel)
                    scope = info.scope & ~(juce::uint32) state::Routing;
            }
        }
        const bool preset = file.hasFileExtension (Show::channelPresetExtension);
        enableKey (*loadKey, preset && selectedStrip() != nullptr);
        enableKey (*updateScope, preset);
        enableKey (*save, selectedStrip() != nullptr);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        paintKeyRows (g, rows);

        // The selected strip: a header in its colour, its number and name.
        g.setColour (Dark::body);
        g.fillRect (stripBox);
        if (auto* s = selectedStrip())
        {
            auto b = stripBox.reduced (4);
            auto header = b.removeFromTop (16);
            g.setColour (s->getColour());
            g.fillRect (header);
            g.setColour (juce::Colours::black);
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            g.drawFittedText (stripName (*s).toUpperCase(), header.reduced (3, 0), juce::Justification::centred, 1);
            g.setColour (Dark::text);
            g.setFont (juce::FontOptions (juce::jmin (22.0f, (float) b.getHeight() * 0.7f), juce::Font::bold));
            g.drawFittedText (s->getLabel().removeCharacters (" "), b, juce::Justification::centred, 1);
        }

        // The scope grid.
        for (auto& c : cells)
        {
            const bool active = c.bits != 0 && c.bits != kConn;
            const bool on = c.bits == kNone || c.bits == kAll ? false : (scope & c.bits) != 0 && active;
            auto r = c.area.toFloat().reduced (1.5f);
            g.setColour (on ? Dark::on : (active ? Dark::key : Dark::key.darker (0.5f)));
            g.fillRect (r);
            g.setColour (on ? juce::Colours::black : (active ? Dark::text : Dark::textDim.withAlpha (0.5f)));
            g.setFont (juce::FontOptions (juce::jlimit (8.0f, 12.0f, r.getHeight() * 0.36f), juce::Font::bold));
            g.drawFittedText (c.name, r.toNearestInt().reduced (1), juce::Justification::centred, 1, 0.6f);
        }
    }

    void resized() override
    {
        auto b = getLocalBounds();
        const int rowH = juce::jlimit (36, 56, getHeight() / 6);
        layoutKeyRows (b, rows, rowH, 4);
        // The strip box at the top right of the first row, with its chooser.
        const auto band = rows.front().band.reduced (4);
        stripKey.setBounds (band.withLeft (band.getRight() - 36));
        stripBox = band.withRight (band.getRight() - 40).withLeft (band.getRight() - 40 - juce::jmin (110, band.getWidth() / 4));

        cells.clear();
        auto grid = b.withTop (rows.back().band.getBottom() + 10).reduced (4, 0);
        const float w = (float) grid.getWidth() / 10.0f;
        const int h = juce::jmin (40, grid.getHeight() / 4);
        auto cell = [&] (int col, int row, const char* name, juce::uint32 bits)
        {
            cells.push_back ({ juce::Rectangle<int> (grid.getX() + juce::roundToInt (w * (float) col), grid.getY() + row * h, juce::roundToInt (w), h), name, bits });
        };
        const juce::uint32 top[] = { kConn, state::Inserts, state::Name, state::Groups, 0, state::Sends, state::Sends, kNone, kAll };
        const char* topNames[] = { "CONN", "CONFIG", "CUST", "TAGS", "", "SEND", "MAIN", "NONE", "ALL" };
        for (int i = 0; i < 9; ++i)
            if (*topNames[i] != 0)
                cell (i + (i >= 4 ? 1 : 0), 0, topNames[i], top[i]);
        const juce::uint32 second[] = { state::Preamp, state::Preamp, state::Preamp, state::Gate, state::Dynamics, state::Eq, state::Fader, state::Fader, state::Fader };
        const char* secondNames[] = { "IN", "FILTER", "DELAY", "GATE", "DYN", "EQ", "PAN", "FDR", "MUTE" };
        for (int i = 0; i < 9; ++i)
            cell (i + (i >= 6 ? 1 : 0), 1, secondNames[i], second[i]);
        cell (0, 2, "PRE", state::Inserts);
        cell (0, 3, "POST", state::Inserts);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        for (auto& c : cells)
            if (c.area.contains (e.getPosition()) && c.bits != 0 && c.bits != kConn)
            {
                if (c.bits == kNone)
                    scope = 0;
                else if (c.bits == kAll)
                    scope = state::AllSections & ~(juce::uint32) state::Routing;
                else
                    scope ^= c.bits;
                repaint();
                return;
            }
    }

private:
    static constexpr juce::uint32 kConn = 0x80000000u, kNone = 0x40000000u, kAll = 0x20000000u;

    struct Cell
    {
        juce::Rectangle<int> area;
        juce::String name;
        juce::uint32 bits;
    };

    void timerCallback() override { refresh(); }

    Strip* selectedStrip() const { return page.ctx.getSelectedStrip ? page.ctx.getSelectedStrip() : nullptr; }

    void chooseStrip()
    {
        juce::StringArray items;
        std::vector<Strip*> strips;
        int selected = -1;
        for (int k = 0; k < kNumStripKinds; ++k)
            for (auto& s : page.ctx.engine.getStrips (static_cast<StripKind> (k)))
            {
                if (s.get() == selectedStrip())
                    selected = items.size();
                items.add (describeStrip (*s).toUpperCase());
                strips.push_back (s.get());
            }
        popups::ListOptions options;
        options.columns = 4;
        options.caption = "CHANNEL";
        juce::Component::SafePointer<ChanTab> safe (this);
        popups::chooseFrom (stripKey, items, selected, [safe, strips] (int i)
        {
            if (safe != nullptr && juce::isPositiveAndBelow (i, (int) strips.size()) && safe->page.ctx.selectStrip)
                safe->page.ctx.selectStrip (strips[(size_t) i]);
        }, options);
    }

    LibraryPage& page;
    juce::OwnedArray<WingKey> keys;
    WingKey *save, *updateScope, *loadKey;
    WingKey stripKey { {}, Icon::ChevronDown };
    std::vector<KeyRow> rows;
    std::vector<Cell> cells;
    juce::Rectangle<int> stripBox;
    juce::uint32 scope = state::AllSections & ~(juce::uint32) state::Routing;
    juce::File shownFile;
};

//==============================================================================
/** CLIP: the clip player (chapter 8) isn't built; the tab shows the console's
    layout dimmed. */
class LibraryPage::PlaceholderTab final : public juce::Component
{
public:
    explicit PlaceholderTab (LibraryPage&)
    {
        for (auto [text, icon] : { std::pair { "PLAY", Icon::Play }, std::pair { "PAUSE", Icon::Pause }, std::pair { "STOP", Icon::Stop } })
            makeKey (keys, *this, text, icon, false);
        rows = { { {}, { keys[0], keys[1] }, { keys[2] } } };
    }

    void paint (juce::Graphics& g) override
    {
        paintKeyRows (g, rows);
        auto r = graphic;
        const float dim = 0.4f;
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
        g.setColour (Dark::textDim);
        g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        g.drawFittedText ("THE CLIP PLAYER IS NOT AVAILABLE YET.", note, juce::Justification::topLeft, 2);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        const int rowH = juce::jlimit (36, 56, getHeight() / 6);
        graphic = b.removeFromTop (juce::jmin (120, b.getHeight() / 3));
        b.removeFromTop (4);
        layoutKeyRows (b, rows, rowH, 4);
        note = b.withTop (rows.back().band.getBottom() + 8).reduced (8, 6);
    }

private:
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
    // The file list: a selected preset shows on its tab; a double-click opens a show.
    list->onFileSelected = [this] (const juce::File&)
    {
        if (auto* fx = dynamic_cast<FxTab*> (tabPages[TabFx]))
            fx->refresh();
        if (auto* chan = dynamic_cast<ChanTab*> (tabPages[TabChan]))
            chan->refresh();
    };
    list->onFileActivated = [this] (const juce::File& file)
    {
        if (file.hasFileExtension (Show::fileExtension))
            confirmLoad ("the show \"" + file.getFileNameWithoutExtension() + "\"", [this, file]
            {
                if (ctx.openShowFile)
                    ctx.openShowFile (file);
            });
    };
    addAndMakeVisible (*list);

    tabs.tabFraction = 0.0f;
    tabs.setTabs ({ { "SHOW", FileColours::show }, { "SNAP", FileColours::snap }, { "SNIP", FileColours::snip }, { "CLIP", FileColours::clip },
                    { "FX", FileColours::fx }, { "CHAN", FileColours::chan } });
    tabs.onChange = [this] (int t) { showTab (t); };
    addAndMakeVisible (tabs);

    tabPages.add (new ShowTab (*this));
    tabPages.add (new SnapTab (*this));
    tabPages.add (new SnipTab (*this));
    tabPages.add (new PlaceholderTab (*this));
    tabPages.add (new FxTab (*this));
    tabPages.add (new ChanTab (*this));
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
    t.prefix = ctx.show.getName().toUpperCase();
    if (const auto scene = ctx.show.getScene (ctx.show.getCurrentSceneIndex()))
    {
        // The show's name is the small caption over the scene ("INT:" on the console).
        t.prefix << ":";
        t.text = scene->name.toUpperCase();
        t.fill = FileColours::snap;
        t.ink = juce::Colours::black;
    }
    else
    {
        t.icon = Icon::ShowFile;
        t.text = "NO SCENE";
    }
    return t;
}

bool LibraryPage::anyGlobalSafe() const
{
    if (ctx.show.hasGlobalSafes())
        return true;
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
    list->setFileFilter (t == TabShow ? Show::fileExtension : t == TabFx ? Show::fxPresetExtension : t == TabChan ? Show::channelPresetExtension : "");
    // Presets are files, scenes and snippets live in the show.
    if ((t == TabFx || t == TabChan) && list->getLocation() == ShowPanel::Location::Show)
        list->setLocation (ShowPanel::Location::Internal);
    else if ((t == TabSnap || t == TabSnip) && list->getLocation() != ShowPanel::Location::Show)
        list->setLocation (ShowPanel::Location::Show);
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

void LibraryPage::openScope (int scopeMode, const juce::String& scopeTitle, state::Scope initial,
                             std::function<void (const state::Scope&, bool noGlobal)> save)
{
    auto window = std::make_unique<ScopeWindow> (ctx, static_cast<ScopeWindow::Mode> (scopeMode), scopeTitle, std::move (initial));
    window->onCancel = [this] { closeScope(); };
    window->onSave = [this, save] (const state::Scope& s, bool noGlobal)
    {
        if (save)
            save (s, noGlobal);
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
    // GLOBAL safes: what scene recalls leave alone. Strips keep their own
    // recall safe; DCAs, mute groups, FX slots, contents and the console
    // settings are the show's global safes.
    auto safe = ctx.show.getGlobalSafe();
    safe.allStrips = false;
    safe.strips.clear();
    for (int k = 0; k < kNumStripKinds; ++k)
        for (auto& s : ctx.engine.getStrips (static_cast<StripKind> (k)))
            if (s->params.recallSafe.load())
                safe.strips.push_back (state::StripRef::of (*s));
    openScope ((int) ScopeWindow::Mode::Global, "GLOBAL SAFE", safe, [this] (const state::Scope& chosen, bool)
    {
        for (int k = 0; k < kNumStripKinds; ++k)
            for (auto& s : ctx.engine.getStrips (static_cast<StripKind> (k)))
                s->params.recallSafe = chosen.includes (state::StripRef::of (*s));
        ctx.show.setGlobalSafe (chosen);
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
    if (auto* fx = dynamic_cast<FxTab*> (tabPages[TabFx]))
        fx->refresh();
    if (auto* chan = dynamic_cast<ChanTab*> (tabPages[TabChan]))
        chan->refresh();
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
