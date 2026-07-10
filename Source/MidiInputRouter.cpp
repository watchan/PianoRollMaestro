#include "MidiInputRouter.h"

void MidiInputRouter::setActiveDevice(const juce::String& deviceIdentifier)
{
    currentInput = juce::MidiInput::openDevice(deviceIdentifier, this);

    if (currentInput != nullptr)
        currentInput->start();
}

void MidiInputRouter::handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& message)
{
    // Called on the MIDI thread. For this step, just log to console via a
    // thread-safe hand-off to the message thread.
    juce::MessageManager::callAsync([desc = message.getDescription()]
    {
        DBG(desc);
    });
}
