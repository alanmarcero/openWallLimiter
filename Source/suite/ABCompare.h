#pragma once
#include <juce_core/juce_core.h>
#include <array>
#include <optional>

namespace suite {
// Two full parameter snapshots. The active slot is the one the listener hears.
class ABCompare {
  public:
    void seed(const juce::MemoryBlock &state);
    int active() const noexcept { return activeSlot; }
    void setActive(int slot) noexcept { activeSlot = slot & 1; }
    const juce::MemoryBlock &slot(int index) const { return slots[static_cast<size_t>(index & 1)]; }
    void setSlot(int index, const juce::MemoryBlock &block) { slots[static_cast<size_t>(index & 1)] = block; }
    juce::MemoryBlock toggle(const juce::MemoryBlock &current);
    // Copies one slot onto the other. Returns the block to restore when the destination is audible.
    std::optional<juce::MemoryBlock> copy(int from, int to, const juce::MemoryBlock &current);

  private:
    std::array<juce::MemoryBlock, 2> slots;
    int activeSlot = 0;
};
} // namespace suite
