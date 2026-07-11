#pragma once
#include <JuceHeader.h>

// Wraps JUCE's built-in AudioDeviceSelectorComponent for audio in/out and
// MIDI in/out device selection. Mouse-driven, same "infrequent setup
// action" exception class as MIDI device selection and file dialogs.
//
// Shows TWO independent selectors stacked vertically: the main manager
// (output + MIDI in/out) and a second, fully separate manager used only for
// the hum-input mic (see MainEditorComponent's micDeviceManager). They're
// deliberately two different AudioDeviceManager instances, not one shared
// duplex stream -- combining audio output and input into a single stream
// produced a persistent audible artifact on this machine regardless of
// which specific devices were chosen, so output and mic input are opened as
// fully independent streams instead.
class AudioMidiSettingsWindow : public juce::DocumentWindow
{
public:
    AudioMidiSettingsWindow(juce::AudioDeviceManager& outputDeviceManager, juce::AudioDeviceManager& micDeviceManager)
        : DocumentWindow("Audio/MIDI Settings", juce::Colours::darkgrey, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar(true);

        auto content = std::make_unique<Content>(outputDeviceManager, micDeviceManager);
        content->setSize(500, 760);

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
        Content(juce::AudioDeviceManager& outputDeviceManager, juce::AudioDeviceManager& micDeviceManager)
            : outputSelector(outputDeviceManager,
                              0, 0,   // audio input channels min/max -- this manager is output-only
                              0, 2,   // audio output channels min/max
                              true,   // show MIDI input options
                              true,   // show MIDI output selector
                              true,   // show channels as stereo pairs
                              false), // don't hide advanced options behind a button
              micSelector(micDeviceManager,
                          1, 1,   // audio input channels min/max -- this manager is mic-input-only
                          0, 0,   // audio output channels min/max
                          false,  // no MIDI input here, already shown above
                          false,  // no MIDI output here
                          true,
                          false)
        {
            addAndMakeVisible(outputSelector);
            addAndMakeVisible(micSelector);
        }

        void resized() override
        {
            auto bounds = getLocalBounds();
            outputSelector.setBounds(bounds.removeFromTop(bounds.getHeight() / 2));
            micSelector.setBounds(bounds);
        }

        juce::AudioDeviceSelectorComponent outputSelector;
        juce::AudioDeviceSelectorComponent micSelector;
    };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioMidiSettingsWindow)
};
