#pragma once
#include <JuceHeader.h>
#include "KeyboardOverlayComponent.h"

// Same DocumentWindow pattern as InstrumentPanelWindow -- but deliberately
// never steals keyboard focus (no toFront(true) anywhere here): this window
// is meant to stay open and visible WHILE the main editor keeps receiving
// every keystroke, not to be interacted with directly.
class KeyboardOverlayWindow : public juce::DocumentWindow
{
public:
    KeyboardOverlayWindow(std::function<bool()> isSessionViewIn,
                           std::function<bool()> isHumActiveIn,
                           std::function<int()> transposeSemitonesIn,
                           std::function<std::vector<int>()> highlightedKeyCodesIn)
        : DocumentWindow("Keyboard", juce::Colours::darkgrey, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar(true);
        setContentOwned(new KeyboardOverlayComponent(std::move(isSessionViewIn), std::move(isHumActiveIn),
                                                        std::move(transposeSemitonesIn), std::move(highlightedKeyCodesIn)),
                          true);
        setResizable(false, false);

        // Stays above every other window (including the main editor) at
        // all times -- purely a z-order flag, distinct from toFront(true),
        // which also steals keyboard focus. This window must never do that
        // (see the class comment), so this is the non-stealing way to keep
        // it from ever getting buried behind the main window.
        setAlwaysOnTop(true);

        // Parked in the bottom-right corner by default, out of the way of
        // the main editor window, rather than centred on top of it -- this
        // is meant to sit alongside the main window, not obscure it.
        if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        {
            auto area = display->userArea;
            setTopLeftPosition(area.getRight() - getWidth() - 20, area.getBottom() - getHeight() - 20);
        }

        setVisible(true); // no toFront(true) -- must not take keyboard focus from the main editor
    }

    void closeButtonPressed() override
    {
        setVisible(false);
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KeyboardOverlayWindow)
};
