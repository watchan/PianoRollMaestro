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

    // isActive = hum-listening mode toggled on with 'v'; noteNumber = -1
    // when nothing's been heard yet this session, otherwise the last note
    // detected (kept even after humming stops, so Shift+F can commit it at
    // your own pace -- see MainEditorComponent's humInputListener.onNoteChange);
    // durationSteps is the Shift+Z/Shift+X commit-duration preset (1/2/4 =
    // 16th/8th/quarter).
    void setHumStatus(bool isActive, int noteNumber, int durationSteps)
    {
        humActive = isActive;
        humNote = noteNumber;
        humDurationSteps = durationSteps;
        repaint();
    }

    void paint(juce::Graphics& g) override;

private:
    bool playing = false;
    double bpmValue = 120.0;
    MidiInputMode mode = MidiInputMode::StepRecord;
    int octaveShift = 0;

    bool humActive = false;
    int humNote = -1;
    int humDurationSteps = 1;
};
