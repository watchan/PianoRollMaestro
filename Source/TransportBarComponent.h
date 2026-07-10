#pragma once
#include <JuceHeader.h>
#include "MidiInputRouter.h"

// Visual-only readout; nothing here is mouse-interactive.
class TransportBarComponent : public juce::Component
{
public:
    void setPlaying(bool isPlaying) { playing = isPlaying; repaint(); }
    void setBpm(double bpm) { bpmValue = bpm; repaint(); }
    void setMode(MidiInputMode m) { mode = m; repaint(); }
    void setOctaveShift(int octaves) { octaveShift = octaves; repaint(); }

    void paint(juce::Graphics& g) override;

private:
    bool playing = false;
    double bpmValue = 120.0;
    MidiInputMode mode = MidiInputMode::StepRecord;
    int octaveShift = 0;
};
