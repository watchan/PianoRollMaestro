#pragma once
#include <JuceHeader.h>
#include "ProjectModel.h"

enum class MidiInputMode
{
    StepRecord,
    PlayMonitor
};

class MidiInputRouter : public juce::MidiInputCallback,
                         private juce::Timer
{
public:
    // Fired on the message thread once a burst of simultaneous note-ons
    // (a chord, or a single note) has finished arriving. StepRecord mode only.
    std::function<void(const std::vector<StepNote>&)> onStepChordCaptured;

    // Fired on the message thread for live preview. PlayMonitor mode only.
    std::function<void(int noteNumber, float velocity, bool isOn)> onLiveNote;

    void setMode(MidiInputMode newMode) { mode = newMode; }
    MidiInputMode getMode() const { return mode; }

    void setActiveDevice(const juce::String& deviceIdentifier);

    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;

private:
    void timerCallback() override;

    std::unique_ptr<juce::MidiInput> currentInput;
    std::vector<StepNote> pendingNotes;
    MidiInputMode mode = MidiInputMode::StepRecord; // touched only on the message thread

    static constexpr int chordCaptureWindowMs = 60;
};
