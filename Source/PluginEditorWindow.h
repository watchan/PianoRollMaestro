#pragma once
#include <JuceHeader.h>

// Wraps a hosted plugin's own native editor (patch browser, etc.) in a
// window. Deliberately mouse-driven -- third-party plugin GUIs are out of
// scope for the app's keyboard-only editing philosophy.
class PluginEditorWindow : public juce::DocumentWindow
{
public:
    PluginEditorWindow(juce::AudioProcessor& processor, const juce::String& name)
        : DocumentWindow(name, juce::Colours::darkgrey, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar(true);

        if (processor.hasEditor())
        {
            if (auto* editor = processor.createEditorIfNeeded())
            {
                setContentOwned(editor, true);
                setResizable(editor->isResizable(), false);
            }
        }

        centreWithSize(juce::jmax(getWidth(), 200), juce::jmax(getHeight(), 100));
        setVisible(true);
    }

    void closeButtonPressed() override
    {
        setVisible(false);
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginEditorWindow)
};
