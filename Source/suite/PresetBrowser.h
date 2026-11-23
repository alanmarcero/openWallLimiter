#pragma once
#include "PresetLibrary.h"
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

namespace suite {
class PresetBrowser : public juce::Component, private juce::ListBoxModel, private juce::Timer {
  public:
    std::function<void(int)> onFactory;
    std::function<void(const juce::String &, const juce::MemoryBlock &)> onUser;
    std::function<void(const juce::String &)> onSaved;
    std::function<void(const juce::String &)> onRenamed;
    std::function<juce::String()> activePreset;
    std::function<void()> onClose;
    std::function<juce::MemoryBlock()> capture;

    explicit PresetBrowser(PresetLibrary &libraryIn);
    ~PresetBrowser() override;
    void refresh();
    // Puts Save and Delete back after an unconfirmed "Replace?" or "Delete?".
    void disarm();
    void setSearchText(const juce::String &text);
    int rowCount() const { return int(shown.size()); }
    void paint(juce::Graphics &) override;
    void resized() override;
    void visibilityChanged() override;
    bool keyPressed(const juce::KeyPress &) override;

  private:
    int getNumRows() override { return rowCount(); }
    void paintListBoxItem(int row, juce::Graphics &, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent &) override;
    void selectedRowsChanged(int row) override;
    void timerCallback() override { disarm(); }
    void updateButtons();
    void loadRow(int row);

    PresetLibrary &library;
    std::vector<PresetEntry> shown;
    bool suppressLoad = false;
    juce::TextEditor search, nameBox;
    juce::ListBox list;
    juce::TextButton save{"Save"}, renameButton{"Rename"}, removeButton{"Delete"}, favoriteButton{"Favorite"};
    juce::Label hint;
    int selected = -1;
    juce::TextButton *armed = nullptr;
};
} // namespace suite
