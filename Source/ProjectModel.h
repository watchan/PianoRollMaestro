#pragma once
#include <JuceHeader.h>
#include <vector>

// One note in a step. A step with multiple StepNotes is a chord.
struct StepNote
{
    int pitch = 60;         // MIDI note number 0-127
    float velocity = 0.8f;  // 0.0-1.0

    bool operator==(const StepNote& other) const { return pitch == other.pitch && velocity == other.velocity; }
};

// One slot in a MidiClip's step grid.
struct Step
{
    std::vector<StepNote> notes;   // empty = rest, >1 entry = chord
    int lengthInSteps = 1;
    bool tiedFromPrevious = false; // true = continuation of the previous step's note

    // Used by undo/redo to detect whether a command actually changed
    // anything -- see MainEditorComponent::StepEditGuard.
    bool operator==(const Step& other) const
    {
        return notes == other.notes && lengthInSteps == other.lengthInSteps && tiedFromPrevious == other.tiedFromPrevious;
    }
};

// An ordered sequence of steps at a fixed step resolution.
class MidiClip
{
public:
    // 12 (not a plain power-of-2 like 4) so both 16th notes (3 steps) AND
    // eighth-note triplets (4 steps -- a triplet doesn't divide evenly into
    // a 4-per-quarter grid at all) are exact integer step counts.
    int stepsPerQuarterNote = 12;
    std::vector<Step> steps;

    // 0 = unset (auto -- the clip ends right after its last note;
    // MainEditorComponent::trimTrailingEmptySteps() keeps trimming trailing
    // rests down to that point, same as before this field existed). >0 =
    // explicitly "the clip is this many steps long," even if that runs past
    // the last note -- trailing rests up to this point are preserved
    // instead of trimmed, and playback/looping (Session View slots) run out
    // to here instead of snapping tight to the last note. Set via 'b' (Set
    // Clip End) at the edit cursor -- MainEditorComponent::setClipEndHere().
    int explicitLengthInSteps = 0;

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
    // same stepsPerQuarterNote in practice). loopEndStep <= loopStartStep
    // means no usable region has been set yet; PlaybackEngine only wraps
    // when loopEnabled is also true. See MainEditorComponent::
    // setLoopStartHere()/setLoopEndHere()/toggleLoopEnabled().
    int loopStartStep = 0;
    int loopEndStep = 0;
    bool loopEnabled = false;

    // Audible click on every quarter-note beat during playback (accented on
    // the downbeat of each 4/4 bar) -- project-wide like the loop region,
    // toggled with 'w'. See PlaybackEngine::renderMetronomeClicks().
    bool metronomeEnabled = false;

    juce::ValueTree toValueTree() const;
    void loadFromValueTree(const juce::ValueTree& tree);

    static constexpr const char* fileExtension = ".pianoroll";
};
