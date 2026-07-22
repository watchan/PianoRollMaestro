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
    // recModeIn: 0 = Off (Browse), 1 = Manual, 2 = Auto, 3 = Realtime --
    // matches MainEditorComponent::RecMode's underlying values. isDrumGridActiveIn:
    // matches MainEditorComponent::drumGridModeActive (Enter toggles it).
    // keyRootPitchClassIn/keyShownIn: MainEditorComponent::scaleRootPitchClass
    // and (currentScaleType != Off) -- when shown, every melodic-keyboard key
    // whose note is the estimated key's root gets a distinct highlight colour.
    KeyboardOverlayComponent(std::function<bool()> isSessionViewIn,
                              std::function<int()> recModeIn,
                              std::function<int()> transposeSemitonesIn,
                              std::function<std::vector<int>()> highlightedKeyCodesIn,
                              std::function<bool()> isDrumGridActiveIn,
                              std::function<int()> keyRootPitchClassIn,
                              std::function<bool()> keyShownIn);

    void paint(juce::Graphics& g) override;

private:
    void timerCallback() override;

    std::function<bool()> isSessionView;
    std::function<int()> recMode;
    std::function<int()> transposeSemitones;
    std::function<std::vector<int>()> highlightedKeyCodes;
    std::function<bool()> isDrumGridActive;
    std::function<int()> keyRootPitchClass;
    std::function<bool()> keyShown;

    // Change-detection for the ~15Hz poll -- only repaint() when something
    // actually differs from last tick, not on every single poll.
    juce::ModifierKeys lastMods;
    bool lastSessionView = false;
    int lastRecMode = 0;
    std::vector<int> lastHighlighted;
    bool lastDrumGridActive = false;
    int lastKeyRootPitchClass = 0;
    bool lastKeyShown = false;
};
