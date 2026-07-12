#pragma once
#include <JuceHeader.h>
#include <map>
#include "AudioMidiSettingsWindow.h"
#include "ChordEstimateBarComponent.h"
#include "HumInputListener.h"
#include "InstrumentPanelWindow.h"
#include "MicLevelMeterComponent.h"
#include "MidiInputRouter.h"
#include "PlaybackEngine.h"
#include "PluginEditorWindow.h"
#include "PluginHost.h"
#include "ProjectModel.h"
#include "SessionGridComponent.h"
#include "ShortcutHelpBarComponent.h"
#include "StepGridComponent.h"
#include "TrackListComponent.h"
#include "TransportBarComponent.h"

class MainEditorComponent : public juce::AudioAppComponent,
                             private juce::ChangeListener,
                             private juce::Timer
{
public:
    MainEditorComponent();
    ~MainEditorComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;
    void releaseResources() override;

private:
    // Persists a device manager's audio/MIDI device selection (which
    // physical in/out devices, channels, sample rate) to disk so a manual
    // fix made in AudioMidiSettingsWindow survives the next launch --
    // without this, every relaunch re-runs JUCE's automatic default-device
    // selection from scratch. Saved on every change via ChangeListener (not
    // just on clean shutdown) since testing/development often kills the
    // process rather than quitting it. Two separate files, one per manager
    // (see micDeviceManager below for why there are two managers at all).
    static juce::File getOutputAudioSettingsFile();
    static juce::File getMicAudioSettingsFile();
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    // Polls PlaybackEngine's playback position while playing and pushes it
    // to stepGrid as a playhead locator (see StepGridComponent::setPlaybackStep)
    // -- there's no push-based notification from the audio thread for this,
    // so a ~30Hz poll is the simplest way to get a live-updating playhead.
    void timerCallback() override;

    void refreshMidiDeviceList();
    void midiDeviceSelected();
    // Hum is inherently monophonic (one pitch at a time from the detector),
    // so each new pitch REPLACES pendingChord with a single note.
    void handleHumNoteChange(int noteNumber, float velocity, bool isOn);
    // MIDI keyboard can play chords: heldMidiNotes tracks exactly what's
    // physically down (add/remove per note-on/off) for live-audio purposes,
    // while pendingChord captures the full chord as it's built up on
    // note-ons and is left untouched on note-offs -- so releasing the keys
    // one at a time (as chords normally are) doesn't erode it back down to
    // whatever's still held. A note-on while nothing else is down starts a
    // fresh chord capture, replacing whatever was pending before (same
    // "stays until overwritten, then replaced on a new gesture" behavior
    // hum's pendingChord already has, just for a set of notes instead of one).
    void handleMidiNoteChange(int noteNumber, float velocity, bool isOn);
    void liveNote(int noteNumber, float velocity, bool isOn);
    void togglePlayback();
    void openInstrumentPanel();
    void openAudioMidiSettings();
    // Show/hide the current track's plugin editor window (Cmd+P). Each
    // track keeps its own window + a remembered "should this be visible"
    // flag (pluginEditorWindowsByTrack/pluginEditorDesiredVisible below),
    // so switching tracks auto-hides whichever window was showing and
    // auto-shows the new track's (if it has one and was left visible) --
    // see updatePluginEditorWindowVisibility(), called from switchTrack().
    // No-op if the current track has no plugin loaded.
    void togglePluginEditor();
    // Applies pluginEditorDesiredVisible against cursorTrackIndex to every
    // window in pluginEditorWindowsByTrack: the current track's window (if
    // any) shows or hides according to its remembered flag, every other
    // track's window is forced hidden. Called on every track switch and
    // whenever a window's desired-visible flag changes. allowStealFocus
    // controls whether a newly-shown window is also brought to the front/
    // made key -- true for a deliberate "show me the plugin" action (Cmd+P,
    // just loaded an instrument), false when it's a side effect of merely
    // switching tracks, where stealing focus from the main editor would
    // silently break every keyboard shortcut until you click back.
    void updatePluginEditorWindowVisibility(bool allowStealFocus = false);
    // A track's plugin instance was just replaced or removed -- any
    // existing PluginEditorWindow for it holds a reference to the now-
    // destroyed juce::AudioPluginInstance and must be torn down, not just
    // hidden. Called before setTrackInstrument() everywhere it's used.
    void invalidatePluginEditorWindow(int trackIndex);

    // Editing commands, all reachable with hands on the keyboard home row.
    void ensureStepExists(int trackIndex, int stepIndex);
    void moveCursor(int deltaSteps);
    // Underlying navigation: jumps to the previous/next note's start if the
    // cursor is currently on/within a note (fast browsing of existing
    // content), otherwise falls back to moveCursor(direction) for precise
    // placement on a rest. direction is -1 or +1.
    void moveCursorByNoteOrStep(int direction);
    // Plain 'f': places a pending hum pitch (if any) like Shift+F used to,
    // otherwise moveCursorByNoteOrStep(1) -- Ableton-Live-style step input.
    void handleForwardKey();
    // Plain 'd': deletes the whole note under the cursor if there is one,
    // otherwise moveCursorByNoteOrStep(-1).
    void handleBackwardKey();
    // Shared by deleteAndRetreat() and handleBackwardKey(): clears every
    // step belonging to the note starting at ownerIndex (its own step plus
    // all tied-continuation steps).
    void deleteWholeNoteAt(int ownerIndex);
    void switchTrack(int deltaTracks);
    // Plain Space: doesn't write or clear anything -- just moves the cursor
    // forward by the current duration preset. Pure navigation, no undo entry.
    void advanceByDuration();
    void deleteAndRetreat();
    void clearCurrentStep();
    void tieCurrentStep();
    // Nudges the pitch of the note at the cursor by deltaSemitones (+1 =
    // '3', -1 = 'e', unmodified -- distinct from Cmd+3/Cmd+E's track
    // switching). Works from ANY step within the note's duration, including
    // tied continuation steps -- walks back to find the step that actually
    // owns the note data first. Auditions the new pitch with a brief
    // noteOn/noteOff so the change is audible immediately. No-op on a rest.
    void adjustNotePitch(int deltaSemitones);
    // Plays a brief noteOn/noteOff for whatever note (if any) is under the
    // cursor right now -- called after every cursor move so navigating with
    // d/f (or auto-advance from other commands) audibly "scrubs" through
    // existing content, and after adjustNotePitch() to confirm the new pitch.
    void auditionNoteAtCursor();
    void shiftOctave(int deltaOctaves);
    void addTrack();
    // Project-wide, not tied to any track/step -- not undoable via
    // Cmd+Z (that history is scoped to note edits, see StepEditGuard).
    void adjustTempo(double deltaBpm);

    // Loop/cycle region for playback (project-wide, not per-track -- same
    // reasoning as adjustTempo, not undo-tracked). 'c' ("Cycle", Ableton/
    // Cubase's name for this) toggles looping on and off; Shift+C/Cmd+C drop
    // the start/end marker at the edit cursor's current position -- 'C' was
    // picked over the more obvious 'L' because L sits on the right-hand side
    // of the keyboard, breaking the left-hand-only rule every other shortcut
    // in this app follows. PlaybackEngine reads project.loopStartStep/
    // loopEndStep/loopEnabled directly and wraps once playback reaches the
    // end while looping is on.
    void toggleLoopEnabled();
    void setLoopStartHere();
    void setLoopEndHere();

    // Session View: the app's starting view (see currentViewMode's default)
    // and the only way into Piano Roll -- a grid of independently-
    // launchable clip slots per track; entering Piano Roll (via 's' or 't')
    // always opens a specific slot (creating a fresh one if it was empty),
    // so there's no "floating" editing buffer that isn't visible anywhere
    // in this grid. Rows = tracks (reuses cursorTrackIndex, shared with the
    // piano roll -- 3/e move it while in Session View, same as Cmd+G/Cmd+B
    // in either view); columns = clip slots, sessionCursorSlotIndex is the
    // session-view-only column cursor (moved by d/f). Only 3/e/d/f/g/t/z/x
    // change meaning between the two views (see keyPressed()'s switch(c))
    // -- everything else (Space/Tab, every Cmd/Shift/Option shortcut) stays
    // identical.
    void toggleViewMode();
    void moveSessionCursor(int deltaSlots);
    // Launches sceneClips[sessionCursorSlotIndex] on the current track --
    // no-op if that slot doesn't exist yet (nothing captured there).
    void launchSlotAtCursor();
    // Silences the current track (playingSlotIndex = -2), independent of
    // every other track's playback.
    void stopCurrentTrackSlot();
    // Copies the current track's editing buffer (clip) into
    // sceneClips[sessionCursorSlotIndex], growing sceneClips to fit if the
    // cursor is past its current end, and live-links `clip` to that slot
    // (see Track::editingSlotIndex) so further edits keep syncing to it.
    void captureClipToSlotAtCursor();
    // Copies sceneClips[sessionCursorSlotIndex] into the current track's
    // editing buffer (clip), live-links `clip` to that slot the same way
    // captureClipToSlotAtCursor() does, and switches to the piano-roll view
    // to show it. If that slot doesn't exist yet, creates a fresh empty
    // clip there first (growing sceneClips to fit) rather than doing
    // nothing -- 't' on an empty slot always gets you into the editor with
    // something to write into.
    void loadSlotAtCursorToEditor();

    // Flips Track::includeInChordEstimate on the current track (Cmd+A) --
    // lets e.g. a drum/percussion track be excluded from ChordEstimator's
    // pooled analysis (its "pitches" would just add noise to the chord
    // guess), with any number of tracks independently in/excluded at once.
    // Not undo-tracked (same reasoning as the loop region/tempo -- it's a
    // project-wide-ish authoring choice, not note content).
    void toggleChordEstimateForCurrentTrack();

    // Undo/redo (Cmd+Z / Cmd+Shift+Z), scoped to note/step edits on the
    // current track only -- not track-list or instrument changes, which
    // aren't wrapped in a StepEditGuard. RAII: construct a StepEditGuard as
    // the first line of any command that edits project.tracks[cursorTrackIndex]
    // .clip.steps; its destructor compares before/after and pushes one
    // undoManager entry if anything actually changed (so pure-navigation
    // outcomes, e.g. handleForwardKey() when there's nothing to place,
    // correctly produce no undo entry). Declared here, defined as a nested
    // class in the .cpp so it can reach private members without a friend
    // declaration -- nested classes have that access automatically.
    class StepEditGuard;
    void applyStepEdit(int trackIndex, const std::vector<Step>& steps, int cursorStep);
    void performUndo();
    void performRedo();

    // Step-grid VIEW controls -- pan/zoom, doesn't touch note data. The
    // grid's pitch window used to be hard-limited to a fixed 48-84 (C3-C6)
    // range with no way to scroll past it. Cmd+Option+3/E pan the visible
    // pitch range up/down (moved off plain 3/e once those became the hum
    // semitone nudge -- see nudgeHumPitch()). Horizontal (Cmd+F/Cmd+D) and
    // vertical (Cmd+3/Cmd+E) zoom are independent, not a combined control.
    void scrollStepGridPitch(int deltaSemitones);
    void zoomStepGridHorizontal(float factor);
    void zoomStepGridVertical(float factor);

    // Piano-roll scale tint (purely visual, doesn't restrict entry) --
    // default C major. Cmd+M cycles Major -> Natural Minor -> off.
    void cycleScale();

    // Hum input: 'v' toggles the mic listener on/off (press once to enter
    // hum-listening mode, again to leave it) so it's never a three-finger
    // hold-and-press gesture. Shift+Z/Shift+X cycle a persistent note-
    // duration preset. Committing is handleForwardKey()'s job (plain 'f'
    // places pendingChord -- the last note(s) heard from EITHER the hum
    // monitor or the MIDI keyboard, not necessarily still sounding, see
    // pendingChord below -- into the step grid at that duration and
    // advances the cursor by it). Can be pressed repeatedly after input
    // stops to commit the same note(s) again.
    void toggleHumInput();
    void cycleHumDuration(int delta);
    void commitPendingNote();
    // Plain 3/e: see humSemitoneNudge's declaration below for why this is
    // scoped to hum only.
    void nudgeHumPitch(int deltaSemitones);
    // Applies octaveShiftOctaves + (hum-only) humSemitoneNudge to a raw
    // pendingChord pitch, clamped 0-127 -- shared by commitPendingNote()
    // and updatePendingNoteDisplays() so the preview always matches exactly
    // what a commit would actually write.
    int shiftedPendingPitch(int rawPitch) const;

    // Persistence -- the only commands allowed to touch a mouse dialog.
    void saveProject();
    void saveProjectAs();
    void openProject();
    void newProject();
    void writeProjectToFile(const juce::File& file);
    void syncProjectInstrumentState();
    void restoreInstrumentsFromProject();

    void refreshChildViews();

    // Pops any trailing run of genuinely-empty rest steps (no notes, not a
    // tied continuation of an earlier note) off the back of every track's
    // clip.steps. ensureStepExists() only grows the vector far enough to
    // WRITE something; clearing/deleting past the current end doesn't need
    // to pad the vector out to get there (an out-of-range index is already
    // an implicit rest everywhere else in this file). Without this,
    // navigating far ahead and then clearing/deleting there left a long,
    // otherwise-invisible tail of allocated-but-empty steps behind --
    // inflating both the saved file and the chord-estimate overview's bar
    // count with nothing to show for it. Called from refreshChildViews()
    // so it runs continuously, including retroactively on a project that
    // already has such bloat. Not undo-tracked -- there's no note content
    // to lose, only unused slots being freed.
    void trimTrailingEmptySteps();

    // Updates the pending-note status text and the step-grid preview
    // outlines from pendingChord -- called from refreshChildViews() AND
    // directly from handleHumNoteChange()/handleMidiNoteChange() so the
    // preview updates live as a new pitch arrives, not just whenever
    // refreshChildViews() happens to run for some unrelated reason.
    void updatePendingNoteDisplays();

    // Pushes currentScaleType/scaleRootPitchClass to stepGrid as a 12-entry
    // in-scale mask. Called from refreshChildViews() and cycleScale().
    void updateStepGridScale();

    // Re-runs ChordEstimator over the whole project and pushes the result
    // to chordEstimateBar. Called from refreshChildViews() -- cheap enough
    // (a handful of tracks/bars) to just always recompute rather than
    // tracking dirtiness.
    void updateChordEstimates();

    MidiInputRouter midiInputRouter;
    HumInputListener humInputListener;

    // The hum-input mic runs on its own, completely independent
    // AudioDeviceManager -- NOT the inherited AudioAppComponent::deviceManager
    // used for output+MIDI. Requesting both input and output channels on a
    // single manager (a normal duplex stream) produced a persistent audible
    // artifact on this machine regardless of which specific devices were
    // selected (confirmed by testing every input/output device combination
    // available); opening output and mic input as two fully separate,
    // single-direction streams avoids it. This does mean the mic can be a
    // different physical device than the main output, or even the same
    // physical interface opened twice (once per direction) -- both work.
    juce::AudioDeviceManager micDeviceManager;
    MicLevelMeterComponent micLevelMeter;

    juce::ComboBox midiDeviceBox;
    juce::Array<juce::MidiDeviceInfo> availableMidiDevices;

    juce::TextButton playButton{ "Play" };
    juce::TextButton instrumentButton{ "Instrument" };
    juce::TextButton audioSettingsButton{ "Audio/MIDI" };
    PlaybackEngine playbackEngine;
    PluginHost pluginHost;

    std::unique_ptr<InstrumentPanelWindow> instrumentPanelWindow;
    // Per-track plugin editor windows, created lazily (only once a track's
    // "show editor" is actually requested) and kept alive-but-hidden when
    // you switch away, so returning to a track shows the same window state
    // (scroll position, open menus) rather than a freshly recreated editor.
    // pluginEditorDesiredVisible remembers whether each track's window
    // SHOULD be visible, independent of its momentary OS visibility (which
    // gets forced false while a different track is current) -- see
    // updatePluginEditorWindowVisibility().
    std::map<int, std::unique_ptr<PluginEditorWindow>> pluginEditorWindowsByTrack;
    std::map<int, bool> pluginEditorDesiredVisible;
    std::unique_ptr<AudioMidiSettingsWindow> audioMidiSettingsWindow;

    TransportBarComponent transportBar;
    TrackListComponent trackList;
    // Estimated chord-progression readout, docked above stepGrid and
    // x-aligned to whatever range it's currently showing -- see
    // updateChordEstimates() (recomputed in refreshChildViews()) and
    // timerCallback() (keeps it visually in sync as stepGrid pans/zooms,
    // since those don't otherwise go through refreshChildViews()).
    ChordEstimateBarComponent chordEstimateBar;
    StepGridComponent stepGrid;
    // Session View's grid, laid out in the same central area stepGrid
    // occupies -- only one of the two is visible at a time, swapped by
    // resized()/toggleViewMode() based on currentViewMode.
    SessionGridComponent sessionGrid;
    ShortcutHelpBarComponent shortcutHelpBar;

    Project project;
    juce::File currentProjectFile;
    std::unique_ptr<juce::FileChooser> fileChooser;

    // Note-edit undo/redo history -- see StepEditGuard. Cleared whenever
    // `project` itself is replaced wholesale (newProject()/openProject()),
    // since old entries reference step data by track index into the OLD
    // project and make no sense once it's gone.
    juce::UndoManager undoManager;

    int cursorTrackIndex = 0;
    int cursorStepIndex = 0;
    int octaveShiftOctaves = 0;

    // Session View -- see toggleViewMode()'s declaration above. Starts here
    // (not PianoRoll): the piano roll always edits a specific, slot-linked
    // clip now (see Track::editingSlotIndex), so the natural entry point is
    // "browse the (empty) slot grid, open one to start writing" rather than
    // dropping straight into an editing buffer that isn't visible anywhere
    // in Session View until you remember to link/capture it.
    enum class ViewMode { PianoRoll, Session };
    ViewMode currentViewMode = ViewMode::Session;
    int sessionCursorSlotIndex = 0;

    // Tracks what liveNote(isOn=true) actually turned on (track + shifted
    // pitch) per raw note number, so the matching note-off targets the same
    // pitch/track even if octaveShiftOctaves or cursorTrackIndex changed
    // while the note was still sounding -- see liveNote() for why this
    // matters. Keyed by the raw (unshifted) note number from the source
    // (hum or real MIDI keyboard).
    struct ActiveLiveNote { int trackIndex; int shiftedPitch; };
    std::map<int, ActiveLiveNote> activeLiveNotes;

    // Piano-roll scale tint state. Off = no tint (all rows treated as
    // "in scale" so nothing is drawn differently).
    enum class ScaleType { Major, NaturalMinor, Off };
    ScaleType currentScaleType = ScaleType::Major;
    int scaleRootPitchClass = 0; // 0 = C

    // Empty = nothing heard yet. Otherwise the last note(s) detected from
    // EITHER source: a single entry from hum (monophonic), or however many
    // are currently/were-last held on the MIDI keyboard (a chord).
    // Deliberately NOT cleared just because nothing's sounding right now
    // (only when a genuinely new pitch/chord arrives), so plain 'f'
    // (handleForwardKey) can commit it on its own schedule instead of
    // needing to land inside the exact instant something is still sounding.
    std::vector<StepNote> pendingChord;

    // Currently physically held MIDI keys -- only used to know when a
    // fresh chord capture should start (see handleMidiNoteChange()); NOT
    // mirrored live into pendingChord, since that would erode a chord back
    // down to whatever's still held as each key is released.
    std::vector<StepNote> heldMidiNotes;

    // Which live-input source pendingChord currently holds -- lets
    // humSemitoneNudge (plain 3/e) apply only to hum-detected pitches, not
    // MIDI ones (a MIDI key is already the exact pitch played; there's
    // nothing to correct). Set by handleHumNoteChange()/handleMidiNoteChange()
    // whenever they (re)populate pendingChord.
    enum class PendingChordSource { None, Hum, Midi };
    PendingChordSource pendingChordSource = PendingChordSource::None;

    // Plain 3/e: nudges hum-detected pitches by a semitone, up/down --
    // correction for the YIN pitch detector occasionally landing a
    // semitone off. Applied as a transform at display/commit time (like
    // octaveShiftOctaves), not baked into pendingChord's raw pitch.
    int humSemitoneNudge = 0;

    // Duration presets in base grid steps (clip resolution is 12 steps per
    // quarter note -- see MidiClip::stepsPerQuarterNote -- specifically so
    // an eighth-note triplet is representable as an exact integer step
    // count). Ordered by actual musical duration (not step count) so
    // cycleHumDuration's finer/coarser direction stays musically monotonic:
    // 3=16th, 4=eighth-triplet, 6=8th, 12=quarter.
    static constexpr int humDurationPresets[4] = { 3, 4, 6, 12 };
    int humDurationPresetIndex = 2; // default to 1/8 (index 2 in this array)

    // Hard-mutes output for ~300ms then fades in over ~150ms whenever
    // prepareToPlay() runs, to mask any click/pop most audio APIs produce on
    // the very first callback(s) after a stream opens. All *Remaining/*Total
    // values are set from the real sample rate in prepareToPlay().
    int64_t deviceSwitchMuteSamplesRemaining = 0;
    int64_t startupFadeInSamplesRemaining = 0;
    int64_t startupFadeInTotalSamples = 1;

    // Set from the real device sample rate in prepareToPlay(); used by
    // timerCallback() to convert PlaybackEngine's sample-based position into
    // a step index for the playhead locator.
    double playbackSampleRate = 44100.0;
};
