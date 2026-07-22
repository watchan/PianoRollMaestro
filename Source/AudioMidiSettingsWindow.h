#pragma once
#include <JuceHeader.h>

// Wraps JUCE's built-in AudioDeviceSelectorComponent for audio output and
// MIDI in/out device selection. Mouse-driven, same "infrequent setup
// action" exception class as MIDI device selection and file dialogs.
class AudioMidiSettingsWindow : public juce::DocumentWindow
{
public:
    explicit AudioMidiSettingsWindow(juce::AudioDeviceManager& outputDeviceManager)
        : DocumentWindow("Audio/MIDI Settings", juce::Colours::darkgrey, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar(true);

        auto content = std::make_unique<Content>(outputDeviceManager);
        content->setSize(500, 400);

        setContentOwned(content.release(), true);
        setResizable(true, false);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
        toFront(true);
    }

    void closeButtonPressed() override
    {
        setVisible(false);
    }

private:
    struct Content : public juce::Component
    {
        explicit Content(juce::AudioDeviceManager& outputDeviceManager)
            : outputSelector(outputDeviceManager,
                              0, 0,   // audio input channels min/max -- this manager is output-only
                              0, 2,   // audio output channels min/max
                              true,   // show MIDI input options
                              true,   // show MIDI output selector
                              true,   // show channels as stereo pairs
                              false)  // don't hide advanced options behind a button
        {
            addAndMakeVisible(outputSelector);
        }

        void resized() override
        {
            outputSelector.setBounds(getLocalBounds());
        }

        juce::AudioDeviceSelectorComponent outputSelector;
    };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioMidiSettingsWindow)
};
