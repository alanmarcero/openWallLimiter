#include "PresetBrowser.h"

namespace {
const juce::Colour ink{0xff0d1317}, panel{0xff152026}, brass{0xffffbe3b}, paper{0xffe6f0f5}, muted{0xff869ca8};
void styleField(juce::TextEditor &field) {
    field.setColour(juce::TextEditor::backgroundColourId, ink);
    field.setColour(juce::TextEditor::textColourId, paper);
    field.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff283842));
    field.setColour(juce::TextEditor::focusedOutlineColourId, brass);
}
} // namespace

namespace suite {
PresetBrowser::PresetBrowser(PresetLibrary &libraryIn) : library(libraryIn) {
    setComponentID("preset_browser");
    styleField(search);
    search.setTextToShowWhenEmpty("Search presets", muted);
    search.setComponentID("preset_search");
    search.setTitle("Search presets");
    search.onTextChange = [this] { refresh(); };
    addAndMakeVisible(search);
    styleField(nameBox);
    nameBox.setTextToShowWhenEmpty("Preset name", muted);
    nameBox.setComponentID("preset_name");
    nameBox.setTitle("Preset name");
    addAndMakeVisible(nameBox);
    list.setModel(this);
    list.setComponentID("preset_list");
    list.setTitle("Presets");
    list.setRowHeight(28);
    list.setColour(juce::ListBox::backgroundColourId, ink);
    list.setColour(juce::ListBox::outlineColourId, juce::Colour(0xff283842));
    list.setOutlineThickness(1);
    addAndMakeVisible(list);
    for (auto *button : {&save, &renameButton, &removeButton, &favoriteButton}) {
        button->setTitle(button->getButtonText());
        addAndMakeVisible(button);
    }
    save.setComponentID("preset_save");
    renameButton.setComponentID("preset_rename");
    removeButton.setComponentID("preset_delete");
    favoriteButton.setComponentID("preset_favorite");
    save.onClick = [this] {
        if (!capture)
            return;
        const auto label = nameBox.getText().trim();
        const auto name = label.isEmpty() ? juce::String("User preset") : label;
        if (library.save(name, capture())) {
            hint.setText("Saved", juce::dontSendNotification);
            if (onSaved)
                onSaved(name);
            refresh();
        } else {
            hint.setText("Factory presets stay read-only", juce::dontSendNotification);
        }
    };
    renameButton.onClick = [this] {
        if (selected < 0 || selected >= rowCount())
            return;
        if (library.rename(shown[size_t(selected)].name, nameBox.getText())) {
            hint.setText("Renamed", juce::dontSendNotification);
            refresh();
        } else {
            hint.setText("Factory presets stay read-only", juce::dontSendNotification);
        }
    };
    removeButton.onClick = [this] {
        if (selected < 0 || selected >= rowCount())
            return;
        if (library.remove(shown[size_t(selected)].name)) {
            hint.setText("Deleted", juce::dontSendNotification);
            refresh();
        } else {
            hint.setText("Factory presets stay read-only", juce::dontSendNotification);
        }
    };
    favoriteButton.onClick = [this] {
        if (selected < 0 || selected >= rowCount())
            return;
        const auto &entry = shown[size_t(selected)];
        library.setFavorite(entry.name, !entry.favorite);
        refresh();
    };
    hint.setColour(juce::Label::textColourId, muted);
    hint.setText("Factory presets stay read-only", juce::dontSendNotification);
    hint.setFont(juce::Font(juce::FontOptions(12.f)));
    addAndMakeVisible(hint);
    refresh();
}
PresetBrowser::~PresetBrowser() { list.setModel(nullptr); }
void PresetBrowser::setSearchText(const juce::String &text) {
    search.setText(text, juce::dontSendNotification);
    refresh();
}
void PresetBrowser::refresh() {
    const juce::ScopedValueSetter<bool> guard(suppressLoad, true);
    const auto keep = selected >= 0 && selected < rowCount() ? shown[size_t(selected)].name : juce::String();
    shown = library.entries(search.getText());
    list.updateContent();
    selected = -1;
    for (int i = 0; i < rowCount(); ++i)
        if (shown[size_t(i)].name == keep)
            selected = i;
    if (selected >= 0)
        list.selectRow(selected, false, true);
    else
        list.deselectAllRows();
    updateButtons();
    repaint();
}
void PresetBrowser::updateButtons() {
    const bool user = selected >= 0 && selected < rowCount() && !shown[size_t(selected)].factory;
    renameButton.setEnabled(user);
    removeButton.setEnabled(user);
    favoriteButton.setEnabled(selected >= 0 && selected < rowCount());
    if (selected >= 0 && selected < rowCount())
        favoriteButton.setButtonText(shown[size_t(selected)].favorite ? "Unfavourite" : "Favorite");
    else
        favoriteButton.setButtonText("Favorite");
    favoriteButton.setTitle(favoriteButton.getButtonText());
}
void PresetBrowser::loadRow(int row) {
    if (suppressLoad || row < 0 || row >= rowCount())
        return;
    const auto &entry = shown[size_t(row)];
    nameBox.setText(entry.name, juce::dontSendNotification);
    if (entry.factory) {
        if (onFactory)
            onFactory(entry.factoryIndex);
    } else if (onUser) {
        const auto block = library.loadUser(entry.name);
        if (block.getSize() > 0)
            onUser(entry.name, block);
    }
}
void PresetBrowser::selectedRowsChanged(int row) {
    selected = row;
    updateButtons();
    loadRow(row);
}
void PresetBrowser::paintListBoxItem(int row, juce::Graphics &g, int width, int height, bool isSelected) {
    if (row < 0 || row >= rowCount())
        return;
    const auto &entry = shown[size_t(row)];
    g.fillAll(isSelected ? brass : (row % 2 ? panel : ink));
    g.setColour(isSelected ? ink : paper);
    g.setFont(juce::Font(juce::FontOptions(14.f)));
    const juce::String mark = entry.favorite ? juce::String::fromUTF8("★ ") : juce::String::fromUTF8("☆ ");
    g.drawText(mark + entry.name, 8, 0, width - 78, height, juce::Justification::centredLeft, true);
    g.setColour(isSelected ? ink : muted);
    g.setFont(juce::Font(juce::FontOptions(11.f)));
    g.drawText(entry.factory ? "Factory" : "User", width - 70, 0, 62, height, juce::Justification::centredRight);
}
void PresetBrowser::paint(juce::Graphics &g) {
    g.fillAll(panel);
    g.setColour(juce::Colour(0xff283842));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(.5f), 6.f, 1.f);
}
void PresetBrowser::resized() {
    auto area = getLocalBounds().reduced(8);
    search.setBounds(area.removeFromTop(28));
    area.removeFromTop(6);
    hint.setBounds(area.removeFromBottom(16));
    area.removeFromBottom(4);
    auto buttons = area.removeFromBottom(36);
    area.removeFromBottom(4);
    nameBox.setBounds(area.removeFromBottom(28));
    area.removeFromBottom(6);
    list.setBounds(area);
    const int width = buttons.getWidth() / 4;
    save.setBounds(buttons.removeFromLeft(width).reduced(2, 0));
    renameButton.setBounds(buttons.removeFromLeft(width).reduced(2, 0));
    removeButton.setBounds(buttons.removeFromLeft(width).reduced(2, 0));
    favoriteButton.setBounds(buttons.reduced(2, 0));
}
} // namespace suite
