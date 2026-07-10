#pragma once
#include <JuceHeader.h>
#include "MidiInputRouter.h"

class MainEditorComponent : public juce::Component
{
public:
    MainEditorComponent();

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void refreshMidiDeviceList();
    void midiDeviceSelected();

    MidiInputRouter midiInputRouter;
    juce::ComboBox midiDeviceBox;
    juce::Array<juce::MidiDeviceInfo> availableMidiDevices;
};
