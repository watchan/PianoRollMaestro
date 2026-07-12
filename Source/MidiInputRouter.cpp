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
    // anything else (onLiveNote's targets aren't thread-safe).
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
    if (onLiveNote)
        onLiveNote(pitch, isOn ? velocity : 0.0f, isOn);
}
