#pragma once
#include <array>
#include <map>
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
    // durationSteps sizes the outline box to the currently-selected commit
    // duration preset (Shift+Z/X) instead of a fixed single step, so the
    // preview already shows how long the note will actually be written.
    void setPreviewNotes(const std::vector<int>& pitches, int durationSteps);

    // Multiplies both preview elements' alpha (1 = full strength, 0 =
    // invisible) -- MainEditorComponent::timerCallback() ramps this down
    // over the last ~0.2s before a forgotten pendingChord auto-clears (see
    // pendingChordIdleSinceMs's declaration), so the color fades out
    // visibly instead of the preview just vanishing outright.
    void setPreviewAlpha(float alpha);

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
    // window; the zoom methods change how many rows/columns fit in it
    // (factor > 1 = zoom out/see more, < 1 = zoom in/see less).
    void scrollPitchView(int deltaSemitones);
    // Vertical zoom is split into two independent controls -- the note
    // grid's pitch rows and the sustain/pitch-bend/filter-cutoff lanes'
    // own pixel heights, deliberately NOT scaled together anymore. They
    // used to move as one (zoomVertical()), but that meant zooming the
    // note grid also resized the automation lanes and vice versa, which
    // the user found undesirable enough to call out by name. Horizontal
    // zoom stays a single shared control (zoomHorizontal()) since every
    // lane shares the same step axis -- desyncing that would make lanes
    // no longer line up.
    void zoomVerticalNoteRows(float factor);
    void zoomVerticalAutomationLanes(float factor);
    void zoomHorizontal(float factor);

    // Auto-pans so pitch sits in the vertical centre of the view -- used to
    // keep the hum-input preview note on screen without needing manual 1/2
    // scrolling to go find it.
    void centerPitchView(int pitch);

    // inScaleByPitchClass[0] = C, [1] = C#, ... [11] = B -- the current
    // estimated-key scale (KeyEstimator). Only drives the red out-of-key
    // outline drawn on individual note blocks now; the background row
    // stripe is a FIXED piano-key pattern independent of this (see
    // paint()'s pianoWhiteKeyByPitchClass). Purely visual, doesn't
    // restrict what can be entered.
    void setScale(const std::array<bool, 12>& inScaleByPitchClass);

    // Loop/cycle region (project-wide, in step-grid coordinates), drawn as a
    // thin bar along the very top of the grid -- bright orange while
    // enabled, a dim outline when a region is set but toggled off, so it's
    // visible either way without being mistaken for an active loop.
    void setLoopRegion(int startStepIn, int endStepIn, bool enabledIn);

    // Individual-note selection within the chord at cursorStep -- see
    // MainEditorComponent::effectiveSelectedPitches()). Drawn as a bright
    // outline on top of the normal note-block fill, for whichever
    // pitch(es) at cursorStep are in this list. Empty (the default) draws
    // no outline at all.
    void setSelectedPitches(const std::vector<int>& pitches);

    // Time-axis multi-note selection (Shift+D/Shift+F extend, quantize
    // target) -- see MainEditorComponent::effectiveSelectedNoteStarts().
    // A list of note-START step indices; each gets a cyan outline around
    // its whole block (every pitch, full tied duration), regardless of
    // where the cursor currently is -- distinct from setSelectedPitches()
    // above, which only ever marks pitches at the single cursor step.
    void setSelectedNoteStarts(const std::vector<int>& stepIndices);

    // Range marked for duplication (Shift+R/Cmd+R, MainEditorComponent::
    // duplicateSelectedRange()) -- a translucent cyan wash across the full
    // height of [startStep, endStep), distinct from the loop region's
    // orange top-bar so the two "start/end marker pair" features (this one
    // and the loop) never look like the same thing. endStep <= startStep
    // (the default) draws nothing.
    void setRangeSelection(int startStepIn, int endStepIn);

    // One entry per pitch still part of the open real-time-REC gesture
    // (MainEditorComponent::pendingChord) -- each grows/freezes
    // INDEPENDENTLY, not as one shared span for the whole chord: a pitch
    // still actually held extends endStep live to the current playhead
    // every tick, while a pitch that's already been released (but the
    // whole gesture hasn't ended yet -- e.g. some OTHER note is still being
    // sustained) stops growing exactly where its own note-off happened
    // instead of visually continuing to stretch alongside whatever's still
    // held. startStep is likewise each note's OWN onset (not
    // necessarily the gesture's very first note-on), matching how
    // commitPendingNoteAt() ultimately writes each onset group. endStep is
    // exclusive per entry. Empty = nothing pending, draws nothing.
    struct LiveRecordingPreviewNote
    {
        int pitch;
        int startStep;
        int endStep;
        bool operator==(const LiveRecordingPreviewNote& other) const
        {
            return pitch == other.pitch && startStep == other.startStep && endStep == other.endStep;
        }
    };
    void setLiveRecordingPreview(const std::vector<LiveRecordingPreviewNote>& notes);

    // Cmd+Ctrl+A/L (MainEditorComponent::AutomationLane) -- controls
    // whether the pitch-bend/filter-cutoff lanes take up any height at all
    // (kept collapsed the rest of the time so the normal piano roll's look
    // is unaffected), and which one gets an accent outline to show it's
    // the one Cmd+Ctrl+Z/X/I/D currently act on. Mirrors MainEditorComponent
    // ::AutomationLane's own enum values/order rather than including that
    // header here.
    enum class AutomationLane { Sustain, PitchBend, FilterCutoff, Parameter };
    // parameterLaneIndex: only meaningful while lane == Parameter -- which
    // entry of MidiClip::parameterLanes gets the accent outline (see
    // MainEditorComponent::automationEditParameterLaneIndex's declaration).
    void setAutomationEditMode(bool active, AutomationLane lane, int parameterLaneIndex = 0);
    // The value Ctrl+V/Cmd+Ctrl+I would place at the cursor right now, in
    // automationEditLane's units (see MainEditorComponent::pitchBendPendingValue/
    // filterCutoffPendingValue) -- -1 means "nothing to preview" (Sustain
    // lane, or automation edit mode off; that lane has no continuous value).
    // Draws a faint dashed preview of the exact line/curve that would
    // result from committing right now, so the effect of Ctrl+V is visible
    // BEFORE pressing it, not just after.
    void setAutomationPendingValue(int value);
    // The curve type Cmd+Ctrl+V is currently cycling for the NEXT point
    // Ctrl+V/Cmd+Ctrl+I would place -- see AutomationCurveType's
    // declaration (curve-on-arrival): this is what shapes the ghost
    // preview's incoming segment, distinct from any real point's own
    // curveType. Ignored while a real point already sits at the cursor
    // (that point's own curveType governs the preview instead).
    void setAutomationPendingCurveType(AutomationCurveType type);
    // The continuous curveAmount (-1..+1, see AutomationPoint's
    // declaration) Cmd+Ctrl+Z/X is currently adjusting for the NEXT point
    // -- mirrors setAutomationPendingCurveType() exactly (same "ignored
    // while a real point sits at the cursor" rule), just for the signed
    // strength half of the shape instead of the Curve/Step half.
    void setAutomationPendingCurveAmount(float amount);
    // Parameter lane's own pending-value preview -- same role as
    // setAutomationPendingValue() above (a faint dashed marker showing
    // where Cmd+Ctrl+I would place a point right now, before pressing it),
    // just float-valued (0.0-1.0, see ParameterAutomationPoint's
    // declaration) and only ever meaningful while automationEditLane ==
    // Parameter. -1.0f means "nothing to preview". Fed by either a
    // keyboard-driven pending value (Cmd+Ctrl+Z/X) or, new, a live Touch
    // gesture while the transport is stopped
    // (MainEditorComponent::previewTouchedParameterValue() -- Manual-mode
    // automation authoring: watch the point track the knob, commit it with
    // Cmd+Ctrl+I once it's where you want).
    void setParameterAutomationPendingValue(float value);
    // Every parameter lane (by index into MidiClip::parameterLanes) that's
    // currently holding a live touch-preview value of its own -- see
    // MainEditorComponent::touchPreviewValues' declaration. A single
    // physical touch can drive several plugin parameters simultaneously (a
    // macro control, linked/morphed parameters), so unlike
    // setParameterAutomationPendingValue() above (which only ever tracks
    // the ONE currently Cmd+Ctrl+L-selected lane), drawParameterLane()
    // shows a ghost marker for EVERY lane with an entry here, whether or
    // not it's the selected one.
    void setParameterAutomationPreviewValues(const std::map<int, float>& valuesByLaneIndex);
    // Parameter lane's own pending curve type/amount, mirroring
    // setAutomationPendingCurveType()/setAutomationPendingCurveAmount()
    // above but kept separate (not folded into that PitchBend/FilterCutoff-
    // only pair) since Parameter has its own independent
    // MainEditorComponent::parameterPendingCurveType/CurveAmount. Shapes
    // the ghost-preview segment for the Cmd+Ctrl+L-selected Parameter lane
    // exactly like Pitch Bend/Filter Cutoff's own ghost does -- a Parameter
    // lane is a full automation lane, not a curve-less approximation of one.
    void setParameterAutomationPendingCurveType(AutomationCurveType type);
    void setParameterAutomationPendingCurveAmount(float amount);
    // Shift+D/F multi-selection (MainEditorComponent::multiSelectedAutomationSteps)
    // -- an outline ring around each selected point's marker, mirroring
    // setSelectedNoteStarts()'s cyan note-block outline.
    void setSelectedAutomationSteps(const std::vector<int>& stepIndices);

    // Exposed so ChordEstimateBarComponent can align its bar labels to
    // exactly the same horizontal window/column width this view is
    // currently showing -- see paint()'s colWidth math, which this mirrors.
    // Just returns the stored, sticky firstVisibleStep (see its
    // declaration) -- no longer recomputed from the cursor every call.
    int getFirstVisibleStep() const { return firstVisibleStep; }
    int getVisibleStepsCount() const { return visibleStepsCount; }
    static constexpr float getLabelGutterWidth() { return labelGutterWidth; }

    void paint(juce::Graphics& g) override;

private:
    const MidiClip* clip = nullptr;
    int cursorStep = 0;
    std::vector<int> previewNotes;
    int previewDurationSteps = 1;
    float previewAlpha = 1.0f;
    int playbackStep = -1;

    std::array<bool, 12> inScale { { true, false, true, false, true, true, false, true, false, true, false, true } }; // C major

    int loopStartStep = 0;
    int loopEndStep = 0;
    bool loopRegionEnabled = false;

    std::vector<int> selectedPitches;
    std::vector<int> selectedNoteStarts;

    int rangeSelectionStart = 0;
    int rangeSelectionEnd = 0;

    std::vector<LiveRecordingPreviewNote> liveRecordingNotes;

    // Pans via scrollPitchView(); default centres the same C3-C6 range the
    // view used to be hard-limited to.
    int lowestVisiblePitch = 48;

    // Horizontal scroll position, in steps -- sticky: only moves when the
    // followed step (edit cursor, or the playhead while playing) would
    // otherwise land outside [firstVisibleStep, firstVisibleStep +
    // visibleStepsCount), via followStepIfOffscreen(). Deliberately NOT
    // recomputed to re-center every frame -- that made the view jump on
    // every single cursor advance even while the cursor was still
    // comfortably on-screen. Mirrors lowestVisiblePitch/
    // centerPitchView()'s same "only scroll when off-screen" pattern on the
    // vertical axis.
    int firstVisibleStep = 0;
    void followStepIfOffscreen();

    // Zoom levels -- row/column counts, not fixed constants anymore.
    int visiblePitchRows = 37; // was highestPitch(84) - lowestPitch(48) + 1
    // 96 at the old 12-steps-per-quarter resolution = 2 bars (4/4) visible
    // by default -- scaled by the same 80x MidiClip::stepsPerQuarterNote
    // itself was raised by (12 -> 960), so the default view still shows the
    // same 2 bars' worth of musical time, not 1/80th of a beat.
    int visibleStepsCount = 96 * 80;

    static constexpr float labelGutterWidth = 40.0f; // left-edge pitch-name axis (includes pitchBarWidth below)
    static constexpr float pitchBarWidth = 6.0f; // slim landscape-gradient strip along the left of the pitch-name axis
    static constexpr float measureLabelHeight = 16.0f; // top strip showing measure numbers
    static constexpr float velocityLaneHeight = 28.0f; // bottom strip showing per-note velocity bars
    // Adjustable (not constexpr) -- see zoomVerticalAutomationLanes()'s
    // declaration: Cmd+Shift+T/G scales these while automation edit mode
    // is active, independently of the note grid's own pitch-row zoom.
    float sustainLaneHeight = 14.0f; // very bottom strip showing sustain pedal ON/OFF -- see paint()

    // Pitch-bend/filter-cutoff lanes, below the sustain lane -- see
    // setAutomationEditMode()'s declaration. Only take up height while
    // automationEditModeActive is true (both collapse to 0 otherwise), so
    // the default piano roll view is unaffected by this feature existing.
    bool automationEditModeActive = false;
    AutomationLane automationEditLane = AutomationLane::Sustain;
    int activeParameterLaneIndex = 0; // see setAutomationEditMode()'s parameterLaneIndex parameter
    float automationLaneHeight = 24.0f; // adjustable -- see sustainLaneHeight's comment above
    int automationPendingValue = -1; // see setAutomationPendingValue()'s declaration
    AutomationCurveType automationPendingCurveType = AutomationCurveType::Curve; // see setAutomationPendingCurveType()'s declaration
    float automationPendingCurveAmount = 0.0f; // see setAutomationPendingCurveAmount()'s declaration
    float parameterPendingValue = -1.0f; // see setParameterAutomationPendingValue()'s declaration
    std::map<int, float> parameterPreviewValuesByLaneIndex; // see setParameterAutomationPreviewValues()'s declaration
    AutomationCurveType parameterPendingCurveType = AutomationCurveType::Curve; // see setParameterAutomationPendingCurveType()'s declaration
    float parameterPendingCurveAmount = 0.0f; // see setParameterAutomationPendingCurveAmount()'s declaration
    std::vector<int> selectedAutomationSteps; // see setSelectedAutomationSteps()'s declaration
};
