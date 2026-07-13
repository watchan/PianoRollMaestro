#pragma once
#include <array>
#include <vector>
#include <JuceHeader.h>
#include "ProjectModel.h"

// Visual-only mini piano-roll: columns = steps, rows = pitch. No mouse handlers
// are implemented on purpose -- this view is never meant to be clicked.
class StepGridComponent : public juce::Component
{
public:
    void setClip(const MidiClip* clipIn, int cursorStepIn);

    // The note(s) 'f' would commit right now (e.g. hum's last-heard pitch,
    // or every currently-held MIDI key), each drawn as a hollow outline at
    // the cursor column plus a full-row tint, so it's visible where they'll
    // land before actually committing. Empty = nothing pending, draws nothing.
    void setPreviewNotes(const std::vector<int>& pitches);

    // stepIndexOrMinusOne = the step currently sounding during playback,
    // drawn as a bright playhead line distinct from the (blue) edit-cursor
    // column. While set to >= 0, the visible window follows the playhead
    // instead of the edit cursor, so playback stays on screen even if the
    // edit cursor hasn't moved. -1 = not playing, draws nothing and the view
    // re-centres on the edit cursor again.
    void setPlaybackStep(int stepIndexOrMinusOne);

    // View controls -- purely how much of the grid is visible/where, doesn't
    // touch any note data. The fixed 48-84 (C3-C6) pitch window and fixed
    // 96-step-wide window were a hard ceiling: a note outside that range
    // couldn't be scrolled to at all. scrollPitchView pans the visible
    // window; zoomVertical/zoomHorizontal change how many rows/columns fit
    // in it (factor > 1 = zoom out/see more, < 1 = zoom in/see less).
    void scrollPitchView(int deltaSemitones);
    void zoomVertical(float factor);
    void zoomHorizontal(float factor);

    // Auto-pans so pitch sits in the vertical centre of the view -- used to
    // keep the hum-input preview note on screen without needing manual 1/2
    // scrolling to go find it.
    void centerPitchView(int pitch);

    // inScaleByPitchClass[0] = C, [1] = C#, ... [11] = B. Rows whose pitch
    // class is in-scale get a faint background tint across the whole row,
    // so it's visible at a glance which rows are "in key" without having to
    // read note names. Purely visual, doesn't restrict what can be entered.
    void setScale(const std::array<bool, 12>& inScaleByPitchClass);

    // Loop/cycle region (project-wide, in step-grid coordinates), drawn as a
    // thin bar along the very top of the grid -- bright orange while
    // enabled, a dim outline when a region is set but toggled off, so it's
    // visible either way without being mistaken for an active loop.
    void setLoopRegion(int startStepIn, int endStepIn, bool enabledIn);

    // Individual-note selection within the chord at cursorStep (HUM-off
    // only -- see MainEditorComponent::effectiveSelectedPitches()). Drawn
    // as a bright outline on top of the normal note-block fill, for
    // whichever pitch(es) at cursorStep are in this list. Empty (the
    // default, and always while HUM is on) draws no outline at all.
    void setSelectedPitches(const std::vector<int>& pitches);

    // Exposed so ChordEstimateBarComponent can align its bar labels to
    // exactly the same horizontal window/column width this view is
    // currently showing -- see paint()'s firstVisibleStep/colWidth math,
    // which these mirror.
    int getFirstVisibleStep() const;
    int getVisibleStepsCount() const { return visibleStepsCount; }
    static constexpr float getLabelGutterWidth() { return labelGutterWidth; }

    void paint(juce::Graphics& g) override;

private:
    const MidiClip* clip = nullptr;
    int cursorStep = 0;
    std::vector<int> previewNotes;
    int playbackStep = -1;

    std::array<bool, 12> inScale { { true, false, true, false, true, true, false, true, false, true, false, true } }; // C major

    int loopStartStep = 0;
    int loopEndStep = 0;
    bool loopRegionEnabled = false;

    std::vector<int> selectedPitches;

    // Pans via scrollPitchView(); default centres the same C3-C6 range the
    // view used to be hard-limited to.
    int lowestVisiblePitch = 48;

    // Zoom levels -- row/column counts, not fixed constants anymore.
    int visiblePitchRows = 37; // was highestPitch(84) - lowestPitch(48) + 1
    int visibleStepsCount = 96;

    static constexpr float labelGutterWidth = 34.0f; // left-edge pitch-name axis
    static constexpr float measureLabelHeight = 16.0f; // top strip showing measure numbers
    static constexpr float velocityLaneHeight = 28.0f; // bottom strip showing per-note velocity bars
};
