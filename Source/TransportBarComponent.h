#pragma once
#include <vector>
#include <JuceHeader.h>

// Visual-only readout; nothing here is mouse-interactive.
class TransportBarComponent : public juce::Component
{
public:
    void setPlaying(bool isPlaying) { playing = isPlaying; repaint(); }
    void setBpm(double bpm) { bpmValue = bpm; repaint(); }
    void setOctaveShift(int octaves) { octaveShift = octaves; repaint(); }
    // Ctrl+Shift+Z/X -- fixed velocity used by both PC-keyboard note
    // sources (the melodic virtual keyboard and the drum grid), since
    // neither can report a real physical press-force the way a MIDI
    // keyboard's velocity byte does.
    void setVirtualKeyboardVelocity(float velocity) { virtualKeyboardVelocity = velocity; repaint(); }

    // 'v' toggles this -- while ON, 'f'/'d' actually place/delete notes
    // (see MainEditorComponent::handleForwardKey()/handleBackwardKey());
    // while OFF they're pure navigation. Drawn as a filled badge, not just
    // text, so the current state is readable at a glance.
    void setHumInputActive(bool isActive) { humInputActive = isActive; repaint(); }

    // 'c' toggles this -- whether the loop region (set with Shift+C/Cmd+C,
    // drawn in stepGrid) actually makes playback wrap. Same filled-badge
    // treatment as the HUM indicator.
    void setLoopEnabled(bool isEnabled) { loopEnabled = isEnabled; repaint(); }

    // 'w' toggles this -- whether PlaybackEngine::renderMetronomeClicks()
    // actually sounds during playback. Same filled-badge treatment.
    void setMetronomeEnabled(bool isEnabled) { metronomeEnabled = isEnabled; repaint(); }

    // Empty = nothing's been heard yet, otherwise the last note(s) detected
    // from EITHER the hum monitor (always one) or the MIDI keyboard
    // (possibly a chord) -- kept even after the note(s) stop sounding, so
    // 'f' can commit at your own pace -- see
    // MainEditorComponent::handleHumNoteChange()/handleMidiNoteChange().
    // durationSteps is the Shift+Z/Shift+X commit-duration preset.
    void setPendingNoteStatus(const std::vector<int>& pitches, int durationSteps)
    {
        pendingNotePitches = pitches;
        pendingNoteDurationSteps = durationSteps;
        repaint();
    }

    void paint(juce::Graphics& g) override;

private:
    bool playing = false;
    double bpmValue = 120.0;
    int octaveShift = 0;
    float virtualKeyboardVelocity = 0.8f;
    bool humInputActive = false;
    bool loopEnabled = false;
    bool metronomeEnabled = false;

    std::vector<int> pendingNotePitches;
    int pendingNoteDurationSteps = 1;
};
