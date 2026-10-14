#pragma once

#include "ui/UiContext.h"

namespace livedaw::ui
{

/** Layer editor: pick a layer (it becomes the mixer's current layer), create
    user layers and fill them with any strips of the board in any order. */
class LayersPanel final : public juce::Component
{
public:
    explicit LayersPanel (UiContext&);
    ~LayersPanel() override;

    void refresh();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class LayerList;
    class MemberList;
    class BoardList;

    Layer currentLayer() const;
    Layer* currentUserLayer() const;
    void changed();
    void addSelectedBoardStrips();

    UiContext& ctx;
    std::unique_ptr<LayerList> layerList;
    std::unique_ptr<MemberList> memberList;
    std::unique_ptr<BoardList> boardList;

    juce::TextButton newButton { "+ New" }, duplicateButton { "Duplicate" }, deleteButton { "Delete" },
        layerUpButton { "Up" }, layerDownButton { "Down" };
    juce::TextButton removeButton { "Remove" }, memberUpButton { "Up" }, memberDownButton { "Down" };
    juce::TextButton addButton { "< Add" };
    juce::Label nameLabel, bankLabel, hintLabel;
    juce::ComboBox bankSize;

    std::vector<Layer> layers;
    juce::Rectangle<int> layersArea, membersArea, boardArea;
};

} // namespace livedaw::ui
