#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>

namespace suite {
struct ParameterChangeAction;
struct ParameterTreeAction;

// Records parameter edits in a JUCE UndoManager. Drags and explicit groups become one step.
class ParameterUndo : private juce::AudioProcessorParameter::Listener {
  public:
    ParameterUndo(juce::AudioProcessor &processor, juce::UndoManager &undoIn);
    ~ParameterUndo() override;

    void suspend();
    void resume();
    void rescan();
    void beginGroup(const juce::String &name);
    void endGroup();
    void performTree(juce::AudioProcessorValueTreeState &state, const juce::ValueTree &next, const juce::String &name);

    class Group {
      public:
        Group(ParameterUndo &ownerIn, const juce::String &name) : owner(ownerIn) { owner.beginGroup(name); }
        ~Group() { owner.endGroup(); }

      private:
        ParameterUndo &owner;
        JUCE_DECLARE_NON_COPYABLE(Group)
    };

  private:
    friend struct ParameterChangeAction;
    friend struct ParameterTreeAction;
    void parameterValueChanged(int index, float newValue) override;
    void parameterGestureChanged(int index, bool starting) override;
    bool applyNormalized(int index, float value);

    juce::UndoManager &undo;
    std::vector<juce::AudioProcessorParameter *> parameters;
    std::vector<float> last;
    int suspendCount = 0, openGestures = 0, groupDepth = 0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ParameterUndo)
};
} // namespace suite
