#pragma once
#include <JuceHeader.h>
#include "ProjectModel.h"

// Visual-only mini piano-roll: columns = steps, rows = pitch. No mouse handlers
// are implemented on purpose -- this view is never meant to be clicked.
class StepGridComponent : public juce::Component
{
public:
    void setClip(const MidiClip* clipIn, int cursorStepIn);

    void paint(juce::Graphics& g) override;

private:
    const MidiClip* clip = nullptr;
    int cursorStep = 0;

    static constexpr int lowestPitch = 48;  // C3
    static constexpr int highestPitch = 84; // C6
    static constexpr int visibleSteps = 32;
};
