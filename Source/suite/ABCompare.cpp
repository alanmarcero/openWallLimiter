#include "ABCompare.h"

namespace suite {
void ABCompare::seed(const juce::MemoryBlock &state) {
    slots[0] = state;
    slots[1] = state;
    activeSlot = 0;
}
juce::MemoryBlock ABCompare::toggle(const juce::MemoryBlock &current) {
    slots[static_cast<size_t>(activeSlot)] = current;
    activeSlot ^= 1;
    return slots[static_cast<size_t>(activeSlot)];
}
std::optional<juce::MemoryBlock> ABCompare::copy(int from, int to, const juce::MemoryBlock &current) {
    from &= 1;
    to &= 1;
    if (from == to)
        return std::nullopt;
    if (activeSlot == from)
        slots[static_cast<size_t>(from)] = current;
    slots[static_cast<size_t>(to)] = slots[static_cast<size_t>(from)];
    if (activeSlot == to)
        return slots[static_cast<size_t>(to)];
    return std::nullopt;
}
} // namespace suite
