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

    // Fired on the message thread for live audio preview -- always, in both
    // StepRecord and PlayMonitor, so you can hear what you're playing while
    // recording, not just while auditioning.
    std::function<void(int noteNumber, float velocity, bool isOn)> onLiveNote;

    // Non-note messages (sustain pedal, other CCs, pitch bend, aftertouch)
    // relevant to live preview -- e.g. a held sustain pedal should keep
    // notes ringing on whatever's loaded. Also fires in both modes.
    std::function<void(const juce::MidiMessage&)> onLiveControllerMessage;

    void setMode(MidiInputMode newMode) { mode = newMode; }
    MidiInputMode getMode() const { return mode; }

    void setActiveDevice(const juce::String& deviceIdentifier);

    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;

    // Message-thread entry point shared by real MIDI keyboard input and any
    // other note source (e.g. hum-to-MIDI pitch detection) that wants to
    // participate in the same StepRecord chord-capture / live-preview logic
    // without duplicating it. Callers on other threads must hop to the
    // message thread themselves before calling this.
    void injectNote(int pitch, float velocity, bool isOn);

private:
    void timerCallback() override;

    std::unique_ptr<juce::MidiInput> currentInput;
    std::vector<StepNote> pendingNotes;
    MidiInputMode mode = MidiInputMode::StepRecord; // touched only on the message thread

    static constexpr int chordCaptureWindowMs = 60;
};
