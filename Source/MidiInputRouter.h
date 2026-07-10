#pragma once
#include <JuceHeader.h>

class MidiInputRouter : public juce::MidiInputCallback
{
public:
    void setActiveDevice(const juce::String& deviceIdentifier);

    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;

private:
    std::unique_ptr<juce::MidiInput> currentInput;
};
