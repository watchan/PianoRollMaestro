#pragma once
#include <JuceHeader.h>
#include <vector>

// One note in a step. A step with multiple StepNotes is a chord.
struct StepNote
{
    int pitch = 60;         // MIDI note number 0-127
    float velocity = 0.8f;  // 0.0-1.0

    // -1 = unset -- use the owning Step's tie-chain length instead (the
    // default for hand-entered notes and old files). >0 = this note's own
    // actual sounding length in base steps, independent of the chord's
    // other notes and independent of the Step's own tie chain (which still
    // exists purely as an editing/visual envelope sized to whichever note
    // in the chord is longest) -- see MainEditorComponent::
    // handleMidiNoteChange()'s wall-clock note-on/note-off timing, which
    // populates this per note so a real-time-recorded chord's individually
    // released notes keep their own actual lengths instead of all being
    // forced to one shared duration.
    int durationSteps = -1;

    bool operator==(const StepNote& other) const
    {
        return pitch == other.pitch && velocity == other.velocity && durationSteps == other.durationSteps;
    }
};

// One slot in a MidiClip's step grid.
struct Step
{
    std::vector<StepNote> notes;   // empty = rest, >1 entry = chord
    int lengthInSteps = 1;
    bool tiedFromPrevious = false; // true = continuation of the previous step's note

    // -1 = not quantized. Otherwise the raw step index this note-start was
    // actually played at before MainEditorComponent::quantizeSelectedNotes()
    // snapped it to a grid -- unquantizeSelectedNotes() reads this to move
    // it back. Only meaningful on a genuine note-start step (non-empty
    // notes, not tiedFromPrevious).
    int quantizedFromStep = -1;

    // Used by undo/redo to detect whether a command actually changed
    // anything -- see MainEditorComponent::StepEditGuard.
    bool operator==(const Step& other) const
    {
        return notes == other.notes && lengthInSteps == other.lengthInSteps && tiedFromPrevious == other.tiedFromPrevious
            && quantizedFromStep == other.quantizedFromStep;
    }
};

// A real MIDI sustain pedal (CC64) press/release, captured at the exact
// step it happened during Real-time REC -- see MainEditorComponent::
// recordSustainPedalEvent(). Recorded and played back as genuine CC64
// automation (PlaybackEngine::scheduleUpTo()/renderNextBlock()) instead of
// approximated via note length, so the loaded synth/plugin's OWN
// sustain-pedal behavior (including any pedal-specific timbre/resonance
// layer it has) reproduces during playback exactly as it already sounds
// during live preview.
struct SustainPedalEvent
{
    int stepIndex = 0;
    bool pedalDown = false;

    bool operator==(const SustainPedalEvent& other) const
    {
        return stepIndex == other.stepIndex && pedalDown == other.pedalDown;
    }
};

// A single breakpoint in a continuous-value automation lane (pitch bend or
// filter cutoff/CC74) -- unlike SustainPedalEvent (a binary on/off
// transition), each of these carries its own value, and
// PlaybackEngine::scheduleUpTo() linearly interpolates between consecutive
// points to reconstruct a smooth ramp during playback, rather than jumping
// instantly from one value to the next. Captured either from real MIDI
// hardware during Real-time REC (a pitch wheel/mod wheel physically moved)
// or drawn by hand, point by point, in MainEditorComponent's keyboard-only
// automation edit mode (Cmd+Ctrl+A) -- see AutomationLane's declaration.
// The shape of the segment ARRIVING AT a given AutomationPoint from the
// PREVIOUS one in the same lane (a point's curveType/curveAmount describe
// the segment leading INTO it, not the one leaving it -- the first point
// in a lane has nothing arriving at it, so its own curveType/curveAmount
// are unused). Cmd+Ctrl+V toggles between the two at the cursor's point
// if one already sits there, otherwise it toggles the PENDING curve type
// (MainEditorComponent::pitchBendPendingCurveType/filterCutoffPendingCurveType)
// that the next Ctrl+V/Cmd+Ctrl+I placement will use -- deliberately
// curve-on-arrival rather than curve-on-departure, so the ghost preview
// (see StepGridComponent::automationPendingValue) can show the actual
// shape of the segment about to be committed, letting the position (t/g)
// and shape (Cmd+Ctrl+V / Cmd+Ctrl+Z/X) of a not-yet-placed point be
// decided together while looking at it, rather than having to place the
// point first and only then go back to shape the segment that led to it.
//
// Replaced the original 4-way discrete Linear/EaseIn/EaseOut/Step enum
// with a binary type (Curve/Step) plus a continuous signed strength
// (AutomationPoint::curveAmount) after hands-on use showed the discrete
// steps were both hard to tell apart visually and too coarse to dial in
// a specific feel -- the same "tension knob" a segment gets in Ableton
// Live/FL Studio/Cubase/Logic's own automation editors. Step
// is kept as its own discrete type rather than folded into the
// continuous range because it's a qualitatively different, non-curve
// behavior (a flat hold followed by an instant jump) that no power-curve
// exponent can represent.
enum class AutomationCurveType
{
    Curve, // shaped by curveAmount below -- 0 = linear, otherwise eased
    Step // holds the PREVIOUS point's value right up until this one, then jumps
};

// The exponent-scaling factor shared by every "shape a segment from
// curveAmount" computation (PlaybackEngine::interpolateAutomationValue()
// and StepGridComponent's addShapedSegment lambda) -- kept in one place
// so the two can never drift apart the way the old discrete EaseIn/EaseOut
// power (always exactly 2) risked if either site's math were tweaked
// independently. amount=+1 (max Ease In) reaches exponent 1+K; amount=-1
// (max Ease Out) mirrors it. Chosen to comfortably exceed the old fixed
// EaseIn/EaseOut's exponent of 2 by the time amount reaches its extremes,
// so the new continuous range strictly a superset of the old discrete one.
constexpr double automationCurveAmountExponentScale = 3.0;

struct AutomationPoint
{
    int stepIndex = 0;
    // Pitch bend: 0-16383, 8192 = center/no bend -- the same range
    // juce::MidiMessage::pitchWheel()'s position argument expects, so no
    // conversion is needed at either recording or playback time. Filter
    // cutoff: 0-127 (CC74), matching juce::MidiMessage::controllerEvent().
    int value = 0;
    AutomationCurveType curveType = AutomationCurveType::Curve;
    // -1.0 (strongest Ease Out) .. 0.0 (Linear) .. +1.0 (strongest Ease
    // In) -- only meaningful when curveType == Curve; ignored (but still
    // stored/persisted harmlessly) when curveType == Step. See
    // automationCurveAmountExponentScale's declaration for the exact
    // shaping formula.
    float curveAmount = 0.0f;

    bool operator==(const AutomationPoint& other) const
    {
        return stepIndex == other.stepIndex && value == other.value
            && curveType == other.curveType && curveAmount == other.curveAmount;
    }
};

// One breakpoint in a ParameterAutomationLane -- same shape as
// AutomationPoint, just a float value (0.0-1.0, JUCE's own normalized
// AudioProcessorParameter range) instead of AutomationPoint's int, since a
// plugin parameter's real resolution would otherwise get needlessly
// quantized down to whatever int range was picked.
struct ParameterAutomationPoint
{
    int stepIndex = 0;
    float value = 0.0f;
    AutomationCurveType curveType = AutomationCurveType::Curve;
    float curveAmount = 0.0f;

    bool operator==(const ParameterAutomationPoint& other) const
    {
        return stepIndex == other.stepIndex && value == other.value
            && curveType == other.curveType && curveAmount == other.curveAmount;
    }
};

// One host-automated plugin parameter -- see MidiClip::parameterLanes'
// declaration. parameterID is JUCE's own AudioProcessorParameter::
// getParameterID() (stable across a plugin's parameter list being
// reordered, unlike a raw index) -- resolved back to a live
// AudioProcessorParameter* by MainEditorComponent/PlaybackEngine
// whenever the track's current plugin is queried; if the currently
// loaded plugin doesn't have a matching parameter (different plugin,
// or an old file), the lane's points are simply not applied, but stay
// harmlessly stored rather than being discarded.
struct ParameterAutomationLane
{
    juce::String parameterID;
    juce::String parameterName; // display fallback if the live plugin can't be queried
    std::vector<ParameterAutomationPoint> points;

    bool operator==(const ParameterAutomationLane& other) const
    {
        return parameterID == other.parameterID && parameterName == other.parameterName && points == other.points;
    }
};

// An ordered sequence of steps at a fixed step resolution.
class MidiClip
{
public:
    // 960 -- the same ticks-per-quarter-note resolution standard MIDI files
    // and most professional DAWs use, not a plain power-of-2 like 1024, so
    // both 16th notes AND eighth-note triplets (which don't divide evenly
    // into a power-of-2-per-quarter grid at all) are exact integer step
    // counts: 960 / 4 = 240 (16th), 960 / 2 * 2 / 3 = 320 (8th triplet).
    // Was 12 for a long time (a plain "48th note" grid) -- real-time REC's
    // onset capture is still ultimately bounded by how often
    // PlaybackEngine::renderNextBlock() itself runs (once per audio
    // callback, so on the order of several ms depending on buffer size),
    // not by this value alone, but a coarse 12-per-quarter grid meant raw
    // human timing was ALSO being rounded down to whatever multiple of a
    // ~1/48-note grid line was nearest regardless of how precisely a note
    // actually arrived -- this raises that grid-rounding ceiling by 80x.
    int stepsPerQuarterNote = 960;
    std::vector<Step> steps;

    // Kept sorted by stepIndex ascending (recorded in chronological order,
    // and Real-time REC only ever appends forward in time) -- see
    // SustainPedalEvent's declaration. Empty for hand-entered clips and old
    // files, same as every other optional/new field here.
    std::vector<SustainPedalEvent> sustainPedalEvents;

    // Continuous automation lanes -- see AutomationPoint's declaration.
    // Both kept sorted by stepIndex ascending, same as sustainPedalEvents.
    // Empty means "not automated at all" (the loaded synth/plugin just
    // stays at whatever its own default/last-set value is) rather than
    // "automated to some default value."
    std::vector<AutomationPoint> pitchBendPoints;
    std::vector<AutomationPoint> filterCutoffPoints; // CC74

    // Host-style plugin-parameter automation -- one lane per touched
    // parameter, auto-created the first time it's touched (see
    // MainEditorComponent::audioProcessorParameterChangeGestureBegin()).
    // Distinct from pitchBendPoints/filterCutoffPoints above (which drive
    // the plugin over its MIDI input) -- these instead call the plugin's
    // own AudioProcessorParameter::setValueNotifyingHost() directly, the
    // same mechanism real DAW host automation uses, so it works for
    // whatever parameters a given plugin actually exposes rather than
    // being limited to what it happens to listen for over MIDI CC
    // (touch-to-automate covers this
    // more generally than enumerating individual CC numbers would).
    std::vector<ParameterAutomationLane> parameterLanes;

    // 0 = unset (auto -- the clip ends right after its last note;
    // MainEditorComponent::trimTrailingEmptySteps() keeps trimming trailing
    // rests down to that point, same as before this field existed). >0 =
    // explicitly "the clip is this many steps long," even if that runs past
    // the last note -- trailing rests up to this point are preserved
    // instead of trimmed, and playback/looping (Session View slots) run out
    // to here instead of snapping tight to the last note. Set via 'b' (Set
    // Clip End) at the edit cursor -- MainEditorComponent::setClipEndHere()
    // (pressing 'b' at step 0 clears it back to 0/unset). Defaults to 8
    // bars (4/4 assumed, same as the 's'/'g' bar-jump math) rather than 0
    // so a brand-new clip is immediately playable/loopable -- without this,
    // playback of a totally empty clip stops instantly (nothing to
    // schedule) with no way to start real-time recording into it.
    int explicitLengthInSteps = 4 * stepsPerQuarterNote * 8;

    // The single source of truth for "how long is this clip": returns
    // explicitLengthInSteps if set, otherwise steps.size(). Used by
    // PlaybackEngine::scheduleUpTo()'s loop-wrap/stop logic, StepGridComponent's
    // clip-end boundary marker, and ChordEstimator's analysis range.
    int effectiveLengthInSteps() const;

    double stepDurationSeconds(double bpm) const;

    juce::ValueTree toValueTree() const;
    void loadFromValueTree(const juce::ValueTree& tree);
};

class Track
{
public:
    juce::String name = "Track 1";
    int midiChannel = 1;

    // The clip currently loaded in the piano-roll editor -- every existing
    // note-editing command (StepEditGuard, ensureStepExists, ChordEstimator,
    // trimTrailingEmptySteps, tie/delete, etc.) reads/writes this one
    // unconditionally, so its meaning is unchanged from before Session View.
    MidiClip clip;

    // Session View: independently-launchable clip slots, separate from the
    // editing buffer above -- deliberately not unified with `clip` so none
    // of that existing editing code needed to change. Grows as needed
    // (MainEditorComponent::captureClipToSlotAtCursor() extends it to fit
    // whatever slot index it's asked to write).
    std::vector<MidiClip> sceneClips;

    // Which source PlaybackEngine schedules for this track: -1 = `clip`
    // (default, preserves pre-Session-View behavior for anyone who never
    // touches it), -2 = explicitly stopped/silent, >=0 = sceneClips[index].
    // Written directly by MainEditorComponent (which owns the Project);
    // PlaybackEngine::retriggerTrack() is then called to pick it up.
    int playingSlotIndex = -1;

    // -1 = `clip` is a standalone editing buffer, unlinked from any slot
    // (matches pre-Session-View behavior). >=0 = `clip` is live-linked to
    // sceneClips[editingSlotIndex] -- MainEditorComponent::refreshChildViews()
    // copies every edit straight back to that slot, so a captured/loaded
    // clip can be edited directly in the piano roll without a separate
    // manual "capture" step each time. Set by loadSlotAtCursorToEditor()/
    // captureClipToSlotAtCursor(). Deliberately not persisted to disk --
    // purely in-session editing state, always -1 right after a project loads.
    int editingSlotIndex = -1;

    // Whether this track's notes are pooled into ChordEstimator's analysis
    // (Source/ChordEstimator.cpp). Defaults to false -- a new track opts IN
    // with Cmd+A rather than opting out, so adding e.g. a drum/percussion
    // track doesn't silently start feeding noise into the chord guess.
    // Multiple tracks are independently includable/excludable at once.
    bool includeInChordEstimate = false;

    // Instrument assignment (Milestone 2). Data only -- the live
    // juce::AudioPluginInstance lives in PlaybackEngine, not here.
    // instrumentDescription.name.isEmpty() means "no plugin, use the
    // built-in fallback synth."
    juce::PluginDescription instrumentDescription;
    juce::MemoryBlock instrumentState;

    juce::ValueTree toValueTree() const;
    void loadFromValueTree(const juce::ValueTree& tree);
};

class Project
{
public:
    double tempoBpm = 120.0;
    std::vector<Track> tracks;

    // Global loop/cycle region for playback, in step-grid coordinates (the
    // same units as cursorStepIndex and Track::clip.steps -- one region for
    // the whole project rather than per-track, since all tracks share the
    // same stepsPerQuarterNote in practice). Two loop kinds, both gated on
    // loopEnabled (PlaybackEngine::renderNextBlock()): if loopEndStep >
    // loopStartStep (a marker region has actually been set), that region
    // is what loops -- takes priority whenever it's set. Otherwise, with
    // loopEnabled on but no usable region (loopEndStep <= loopStartStep,
    // the "unset" convention), playback instead loops each track's own
    // clip to its own end (Track::clip.effectiveLengthInSteps()) rather
    // than stopping there. See MainEditorComponent::
    // setLoopStartHere()/setLoopEndHere()/toggleLoopEnabled().
    int loopStartStep = 0;
    int loopEndStep = 0;
    bool loopEnabled = false;

    // Audible click on every quarter-note beat during playback (accented on
    // the downbeat of each 4/4 bar) -- project-wide like the loop region,
    // toggled with 'w'. See PlaybackEngine::renderMetronomeClicks().
    bool metronomeEnabled = false;

    // Whether starting Real-time REC gives a 4-beat audible count-in before
    // it actually starts capturing, or starts immediately -- project-wide
    // like metronomeEnabled, toggled with Shift+W. See
    // MainEditorComponent::togglePlayback()/playFromLocator().
    bool countInEnabled = true;

    juce::ValueTree toValueTree() const;
    void loadFromValueTree(const juce::ValueTree& tree);

    static constexpr const char* fileExtension = ".pianoroll";
};
