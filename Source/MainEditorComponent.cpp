#include "MainEditorComponent.h"

MainEditorComponent::MainEditorComponent()
{
    addAndMakeVisible(midiDeviceBox);
    midiDeviceBox.onChange = [this] { midiDeviceSelected(); };
    refreshMidiDeviceList();

    setSize(900, 600);
}

void MainEditorComponent::refreshMidiDeviceList()
{
    availableMidiDevices = juce::MidiInput::getAvailableDevices();

    midiDeviceBox.clear();
    for (int i = 0; i < availableMidiDevices.size(); ++i)
        midiDeviceBox.addItem(availableMidiDevices[i].name, i + 1);
}

void MainEditorComponent::midiDeviceSelected()
{
    auto index = midiDeviceBox.getSelectedItemIndex();
    if (index < 0 || index >= availableMidiDevices.size())
        return;

    midiInputRouter.setActiveDevice(availableMidiDevices[index].identifier);
}

void MainEditorComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::darkgrey);
}

void MainEditorComponent::resized()
{
    midiDeviceBox.setBounds(10, 10, 300, 24);
}
