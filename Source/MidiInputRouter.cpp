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
    // Live audio preview (onLiveNote/onLiveControllerMessage) always fires,
    // regardless of mode -- you should be able to hear what you're playing
    // while step-recording, not just in PlayMonitor. StepRecord additionally
    // captures note-ons into the pending chord for the current step.
    if (message.isNoteOn())
    {
        juce::MessageManager::callAsync([this,
                                          pitch = message.getNoteNumber(),
                                          velocity = message.getFloatVelocity()]
        {
            injectNote(pitch, velocity, true);
        });
    }
    else if (message.isNoteOff())
    {
        juce::MessageManager::callAsync([this, pitch = message.getNoteNumber()]
        {
            injectNote(pitch, 0.0f, false);
        });
    }
    else if (message.isController() || message.isPitchWheel() || message.isAftertouch() || message.isChannelPressure())
    {
        juce::MessageManager::callAsync([this, message]
        {
            if (onLiveControllerMessage)
                onLiveControllerMessage(message);
        });
    }
}

void MidiInputRouter::injectNote(int pitch, float velocity, bool isOn)
{
    // StepRecord additionally captures note-ons into the pending chord for
    // the current step. Live audio preview (onLiveNote) always fires,
    // regardless of mode -- you should be able to hear what you're playing
    // while step-recording, not just in PlayMonitor.
    if (isOn)
    {
        if (mode == MidiInputMode::StepRecord)
        {
            pendingNotes.push_back({ pitch, velocity });
            startTimer(chordCaptureWindowMs);
        }

        if (onLiveNote)
            onLiveNote(pitch, velocity, true);
    }
    else if (onLiveNote)
    {
        onLiveNote(pitch, 0.0f, false);
    }
}

void MidiInputRouter::timerCallback()
{
    stopTimer();

    if (onStepChordCaptured && !pendingNotes.empty())
        onStepChordCaptured(pendingNotes);

    pendingNotes.clear();
}
