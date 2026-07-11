#pragma once
#include <JuceHeader.h>
#include "ProjectModel.h"

// Visual-only mini piano-roll: columns = steps, rows = pitch. No mouse handlers
// are implemented on purpose -- this view is never meant to be clicked.
class StepGridComponent : public juce::Component
{
public:
    void setClip(const MidiClip* clipIn, int cursorStepIn);

    // pitchOrMinusOne = the note Shift+F would commit right now (e.g. hum
    // input's last-heard pitch), drawn as a hollow outline at the cursor
    // column so it's visible where it will land before actually committing.
    // -1 = nothing pending, draws nothing.
    void setPreviewNote(int pitchOrMinusOne);

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

    void paint(juce::Graphics& g) override;

private:
    const MidiClip* clip = nullptr;
    int cursorStep = 0;
    int previewNote = -1;
    int playbackStep = -1;

    // Pans via scrollPitchView(); default centres the same C3-C6 range the
    // view used to be hard-limited to.
    int lowestVisiblePitch = 48;

    // Zoom levels -- row/column counts, not fixed constants anymore.
    int visiblePitchRows = 37; // was highestPitch(84) - lowestPitch(48) + 1
    int visibleStepsCount = 96;

    static constexpr float labelGutterWidth = 34.0f; // left-edge pitch-name axis
};
