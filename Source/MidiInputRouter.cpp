#include "MidiInputRouter.h"

void MidiInputRouter::setActiveDevice(const juce::String& deviceIdentifier)
{
    currentInput = juce::MidiInput::openDevice(deviceIdentifier, this);

    if (currentInput != nullptr)
        currentInput->start();
}

void MidiInputRouter::handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& message)
{
    // Called on the MIDI thread; hop to the message thread before touching
    // pendingNotes, mode, or the (message-thread-only) juce::Timer.
    if (message.isNoteOn())
    {
        juce::MessageManager::callAsync([this,
                                          pitch = message.getNoteNumber(),
                                          velocity = message.getFloatVelocity()]
        {
            if (mode == MidiInputMode::StepRecord)
            {
                pendingNotes.push_back({ pitch, velocity });
                startTimer(chordCaptureWindowMs);
            }
            else if (onLiveNote)
            {
                onLiveNote(pitch, velocity, true);
            }
        });
    }
    else if (message.isNoteOff())
    {
        juce::MessageManager::callAsync([this, pitch = message.getNoteNumber()]
        {
            if (mode == MidiInputMode::PlayMonitor && onLiveNote)
                onLiveNote(pitch, 0.0f, false);
        });
    }
}

void MidiInputRouter::timerCallback()
{
    stopTimer();

    if (onStepChordCaptured && !pendingNotes.empty())
        onStepChordCaptured(pendingNotes);

    pendingNotes.clear();
}
