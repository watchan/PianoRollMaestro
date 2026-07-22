#pragma once
#include <JuceHeader.h>
#include "ProjectModel.h"

// Physical MIDI keyboard (and PC-keyboard, via injectNote()) input --
// purely a live monitor, it never writes into the step grid on its own.
// Committing a note into the grid is a separate action (manual 'f', or
// auto-commit -- see MainEditorComponent::handleForwardKey()/
// handleMidiNoteChange()), so this is just the source feeding the pending
// chord.
class MidiInputRouter : public juce::MidiInputCallback
{
public:
    // Fired on the message thread for live audio preview, and to update
    // whatever's pending to be committed with 'f'.
    std::function<void(int noteNumber, float velocity, bool isOn)> onLiveNote;

    // Non-note messages (sustain pedal, other CCs, pitch bend, aftertouch)
    // relevant to live preview -- e.g. a held sustain pedal should keep
    // notes ringing on whatever's loaded.
    std::function<void(const juce::MidiMessage&)> onLiveControllerMessage;

    void setActiveDevice(const juce::String& deviceIdentifier);

    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;

    // Message-thread entry point shared by real MIDI keyboard input and any
    // other note source that wants to feed the same live-preview path.
    // Callers on other threads must hop to the message thread themselves
    // before calling this.
    void injectNote(int pitch, float velocity, bool isOn);

private:
    std::unique_ptr<juce::MidiInput> currentInput;
};
