#pragma once
#include <JuceHeader.h>
#include <functional>
#include <map>
#include <vector>

// Visual-only reference of the main editor's ENTIRE keyboard command layer,
// laid out as a physical QWERTY grid instead of a scrolling text list (see
// ShortcutHelpBarComponent) -- lets every key's current meaning be seen at
// a glance, live-updating as modifiers are held/released so the label
// always matches what actually pressing that key right now would do.
// Purely a display: never sends synthetic key events, no mouse handling.
class KeyboardOverlayComponent : public juce::Component,
                                  private juce::Timer
{
public:
    // Getters, not direct values -- polled every tick (see timerCallback())
    // rather than pushed, so this component never needs MainEditorComponent
    // to remember to notify it; it just asks. highlightedKeyCodesIn can
    // return more than one code at once -- e.g. a held chord on the
    // virtual keyboard highlights every one of its keys simultaneously, not
    // just whichever was struck most recently.
    KeyboardOverlayComponent(std::function<bool()> isSessionViewIn,
                              std::function<bool()> isHumActiveIn,
                              std::function<int()> transposeSemitonesIn,
                              std::function<std::vector<int>()> highlightedKeyCodesIn);

    void paint(juce::Graphics& g) override;

private:
    void timerCallback() override;

    std::function<bool()> isSessionView;
    std::function<bool()> isHumActive;
    std::function<int()> transposeSemitones;
    std::function<std::vector<int>()> highlightedKeyCodes;

    // Change-detection for the ~15Hz poll -- only repaint() when something
    // actually differs from last tick, not on every single poll.
    juce::ModifierKeys lastMods;
    bool lastSessionView = false;
    bool lastHumActive = false;
    std::vector<int> lastHighlighted;
};
