#pragma once
#include <JuceHeader.h>
#include "InstrumentPanelComponent.h"

class InstrumentPanelWindow : public juce::DocumentWindow
{
public:
    InstrumentPanelWindow(PluginHost& pluginHost,
                           const juce::String& trackName,
                           const juce::String& currentInstrumentName,
                           std::function<void(const juce::PluginDescription&)> onLoad,
                           std::function<void()> onShowEditor,
                           std::function<void()> onRemove)
        : DocumentWindow("Instrument", juce::Colours::darkgrey, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar(true);

        auto* panel = new InstrumentPanelComponent(pluginHost, trackName, currentInstrumentName,
                                                     std::move(onLoad), std::move(onShowEditor), std::move(onRemove),
                                                     [this] { setVisible(false); });
        setContentOwned(panel, true);
        setResizable(true, false);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);

        // setVisible(true) alone doesn't reliably hand this new window OS
        // keyboard focus -- toFront(true) forces both frontmost and key-
        // window status. Calling focusSearchBox() any earlier than this
        // (e.g. from the component's own visibilityChanged(), which fires
        // as soon as setContentOwned() marks it visible) is too early for
        // grabKeyboardFocus() to stick, since the window isn't actually on
        // screen yet at that point.
        toFront(true);
        panel->focusSearchBox();
    }

    void closeButtonPressed() override
    {
        setVisible(false);
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InstrumentPanelWindow)
};
