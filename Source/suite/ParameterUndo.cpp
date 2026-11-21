#include "ParameterUndo.h"
#include <cmath>

namespace suite {
struct ParameterChangeAction : juce::UndoableAction {
    ParameterChangeAction(ParameterUndo &ownerIn, int indexIn, float oldIn, float newIn)
        : owner(ownerIn), index(indexIn), oldValue(oldIn), newValue(newIn) {}
    ParameterUndo &owner;
    int index;
    float oldValue, newValue;
    bool perform() override { return owner.applyNormalized(index, newValue); }
    bool undo() override { return owner.applyNormalized(index, oldValue); }
    int getSizeInUnits() override { return int(sizeof(*this)); }
};

struct ParameterTreeAction : juce::UndoableAction {
    ParameterTreeAction(ParameterUndo &ownerIn, juce::AudioProcessorValueTreeState &stateIn, juce::ValueTree beforeIn,
                        juce::ValueTree afterIn)
        : owner(ownerIn), state(stateIn), before(std::move(beforeIn)), after(std::move(afterIn)) {}
    ParameterUndo &owner;
    juce::AudioProcessorValueTreeState &state;
    juce::ValueTree before, after;
    bool apply(const juce::ValueTree &tree) {
        owner.suspend();
        state.replaceState(tree.createCopy());
        owner.rescan();
        owner.resume();
        return true;
    }
    bool perform() override { return apply(after); }
    bool undo() override { return apply(before); }
    int getSizeInUnits() override { return int(sizeof(*this)); }
};

ParameterUndo::ParameterUndo(juce::AudioProcessor &processor, juce::UndoManager &undoIn) : undo(undoIn) {
    for (auto *parameter : processor.getParameters()) {
        parameters.push_back(parameter);
        last.push_back(parameter->getValue());
        parameter->addListener(this);
    }
}
ParameterUndo::~ParameterUndo() {
    for (auto *parameter : parameters)
        parameter->removeListener(this);
}
void ParameterUndo::suspend() { ++suspendCount; }
void ParameterUndo::resume() {
    if (suspendCount > 0)
        --suspendCount;
}
void ParameterUndo::rescan() {
    for (size_t i = 0; i < parameters.size(); ++i)
        last[i] = parameters[i]->getValue();
}
void ParameterUndo::beginGroup(const juce::String &name) {
    if (groupDepth == 0)
        undo.beginNewTransaction(name);
    ++groupDepth;
}
void ParameterUndo::endGroup() {
    if (groupDepth > 0)
        --groupDepth;
}
void ParameterUndo::performTree(juce::AudioProcessorValueTreeState &state, const juce::ValueTree &next,
                                const juce::String &name) {
    undo.beginNewTransaction(name);
    undo.perform(new ParameterTreeAction(*this, state, state.copyState().createCopy(), next.createCopy()));
}
void ParameterUndo::parameterGestureChanged(int, bool starting) {
    if (suspendCount > 0)
        return;
    if (starting) {
        if (openGestures == 0 && groupDepth == 0)
            undo.beginNewTransaction();
        ++openGestures;
    } else if (openGestures > 0) {
        --openGestures;
    }
}
void ParameterUndo::parameterValueChanged(int index, float newValue) {
    if (index < 0 || index >= int(parameters.size()))
        return;
    if (suspendCount > 0) {
        last[size_t(index)] = newValue;
        return;
    }
    const float old = last[size_t(index)];
    if (std::abs(old - newValue) <= 1.0e-6f)
        return;
    last[size_t(index)] = newValue;
    // Knob drags and button clicks send a gesture. Host automation without one stays out of the stack.
    if (openGestures <= 0 && groupDepth <= 0)
        return;
    undo.perform(new ParameterChangeAction(*this, index, old, newValue));
    if (onEdit)
        onEdit(index);
}
bool ParameterUndo::applyNormalized(int index, float value) {
    if (index < 0 || index >= int(parameters.size()))
        return false;
    ++suspendCount;
    parameters[size_t(index)]->setValueNotifyingHost(value);
    last[size_t(index)] = value;
    --suspendCount;
    return true;
}
} // namespace suite
