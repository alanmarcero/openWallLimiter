#include "PresetLibrary.h"
#include <algorithm>

namespace suite {
PresetLibrary::PresetLibrary(juce::File presetDirectory, juce::StringArray factory)
    : dir(std::move(presetDirectory)), factoryNames(std::move(factory)) {}

juce::File PresetLibrary::defaultDirectory(const juce::String &pluginName) {
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("Alan Marcero")
        .getChildFile(pluginName)
        .getChildFile("Presets");
}
juce::String PresetLibrary::sanitize(const juce::String &name) const {
    juce::String clean;
    for (auto character : name.trim()) {
        const bool ok = juce::CharacterFunctions::isLetterOrDigit(character) || character == ' ' || character == '-'
                        || character == '_' || character == '.' || character == '(' || character == ')';
        if (ok)
            clean += character;
        else
            clean += '_';
    }
    clean = clean.trim();
    while (clean.contains("  "))
        clean = clean.replace("  ", " ");
    return clean.substring(0, 80);
}
juce::File PresetLibrary::fileFor(const juce::String &name) const {
    return dir.getChildFile(sanitize(name) + ".preset");
}
juce::File PresetLibrary::favoritesFile() const {
    return dir.getParentDirectory().getChildFile("favorites.txt");
}
bool PresetLibrary::isFactory(const juce::String &name) const {
    return factoryNames.contains(sanitize(name));
}
juce::StringArray PresetLibrary::readFavorites() const {
    return juce::StringArray::fromLines(favoritesFile().loadFileAsString());
}
void PresetLibrary::writeFavorites(const juce::StringArray &names) const {
    favoritesFile().getParentDirectory().createDirectory();
    favoritesFile().replaceWithText(names.joinIntoString("\n"));
}
bool PresetLibrary::save(const juce::String &name, const juce::MemoryBlock &state) {
    const auto clean = sanitize(name);
    if (clean.isEmpty() || isFactory(clean) || state.getSize() == 0)
        return false;
    dir.createDirectory();
    return fileFor(clean).replaceWithData(state.getData(), state.getSize());
}
bool PresetLibrary::rename(const juce::String &from, const juce::String &to) {
    const auto oldName = sanitize(from);
    const auto newName = sanitize(to);
    if (oldName.isEmpty() || newName.isEmpty() || isFactory(oldName) || isFactory(newName))
        return false;
    const auto source = fileFor(oldName);
    const auto dest = fileFor(newName);
    if (!source.existsAsFile())
        return false;
    if (source == dest)
        return true;
    if (dest.exists())
        return false;
    if (!source.moveFileTo(dest))
        return false;
    auto favorites = readFavorites();
    const int index = favorites.indexOf(oldName);
    if (index >= 0) {
        favorites.set(index, newName);
        writeFavorites(favorites);
    }
    return true;
}
bool PresetLibrary::remove(const juce::String &name) {
    const auto clean = sanitize(name);
    if (clean.isEmpty() || isFactory(clean))
        return false;
    const auto file = fileFor(clean);
    if (!file.existsAsFile())
        return false;
    if (!file.deleteFile())
        return false;
    auto favorites = readFavorites();
    favorites.removeString(clean);
    writeFavorites(favorites);
    return true;
}
void PresetLibrary::setFavorite(const juce::String &name, bool favorite) {
    const auto clean = sanitize(name);
    if (clean.isEmpty())
        return;
    if (!isFactory(clean) && !fileFor(clean).existsAsFile())
        return;
    auto favorites = readFavorites();
    favorites.removeEmptyStrings();
    const bool has = favorites.contains(clean);
    if (favorite && !has)
        favorites.add(clean);
    if (!favorite && has)
        favorites.removeString(clean);
    writeFavorites(favorites);
}
juce::MemoryBlock PresetLibrary::loadUser(const juce::String &name) const {
    juce::MemoryBlock block;
    const auto file = fileFor(name);
    if (!isFactory(name) && file.existsAsFile())
        file.loadFileAsData(block);
    return block;
}
std::vector<PresetEntry> PresetLibrary::entries(const juce::String &search) const {
    const auto favorites = readFavorites();
    const auto query = search.trim();
    std::vector<PresetEntry> factory;
    std::vector<PresetEntry> user;
    for (int i = 0; i < factoryNames.size(); ++i) {
        const auto name = factoryNames[i];
        if (query.isNotEmpty() && !name.containsIgnoreCase(query))
            continue;
        factory.push_back({name, true, favorites.contains(name), i});
    }
    if (dir.isDirectory()) {
        for (const auto &file : dir.findChildFiles(juce::File::findFiles, false, "*.preset")) {
            const auto name = file.getFileNameWithoutExtension();
            if (isFactory(name) || (query.isNotEmpty() && !name.containsIgnoreCase(query)))
                continue;
            user.push_back({name, false, favorites.contains(name), -1});
        }
    }
    std::sort(user.begin(), user.end(), [](const PresetEntry &a, const PresetEntry &b) {
        return a.name.compareIgnoreCase(b.name) < 0;
    });
    std::vector<PresetEntry> ordered;
    const auto take = [&](bool wantFavorite, std::vector<PresetEntry> &source) {
        for (const auto &entry : source)
            if (entry.favorite == wantFavorite)
                ordered.push_back(entry);
    };
    take(true, factory);
    take(true, user);
    take(false, factory);
    take(false, user);
    return ordered;
}
} // namespace suite
