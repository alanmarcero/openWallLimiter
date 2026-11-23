#pragma once
#include <juce_core/juce_core.h>
#include <vector>

namespace suite {
struct PresetEntry {
    juce::String name;
    bool factory = false;
    bool favorite = false;
    int factoryIndex = -1;
};

// Factory names are read-only. User presets are files under the preset directory.
class PresetLibrary {
  public:
    PresetLibrary(juce::File presetDirectory, juce::StringArray factory);
    static juce::File defaultDirectory(const juce::String &pluginName);

    const juce::File &directory() const noexcept { return dir; }
    bool save(const juce::String &name, const juce::MemoryBlock &state);
    bool rename(const juce::String &from, const juce::String &to);
    bool remove(const juce::String &name);
    void setFavorite(const juce::String &name, bool favorite);
    bool isFactory(const juce::String &name) const;
    juce::String sanitize(const juce::String &name) const;
    std::vector<PresetEntry> entries(const juce::String &search) const;
    juce::MemoryBlock loadUser(const juce::String &name) const;

  private:
    juce::File fileFor(const juce::String &name) const;
    juce::File favoritesFile() const;
    juce::StringArray readFavorites() const;
    void writeFavorites(const juce::StringArray &names) const;

    juce::File dir;
    juce::StringArray factoryNames;
};
} // namespace suite
