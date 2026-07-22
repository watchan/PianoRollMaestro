#pragma once
#include <JuceHeader.h>
#include <map>
#include <set>
#include "AudioMidiSettingsWindow.h"
#include "ChordEstimateBarComponent.h"
#include "InstrumentPanelWindow.h"
#include "KeyboardOverlayWindow.h"
#include "KeyEstimator.h"
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
                             private juce::Timer,
                             private juce::AudioProcessorListener
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
    // Persists the device manager's audio/MIDI device selection (which
    // physical in/out devices, channels, sample rate) to disk so a manual
    // fix made in AudioMidiSettingsWindow survives the next launch --
    // without this, every relaunch re-runs JUCE's automatic default-device
    // selection from scratch. Saved on every change via ChangeListener (not
    // just on clean shutdown) since testing/development often kills the
    // process rather than quitting it.
    static juce::File getOutputAudioSettingsFile();
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    // Polls PlaybackEngine's playback position while playing and pushes it
    // to stepGrid as a playhead locator (see StepGridComponent::setPlaybackStep)
    // -- there's no push-based notification from the audio thread for this,
    // so a ~30Hz poll is the simplest way to get a live-updating playhead.
    void timerCallback() override;

    void refreshMidiDeviceList();
    void midiDeviceSelected();
    // Same "~/Library/Application Support/PianoRollMaestro/" convention as
    // getOutputAudioSettingsFile() -- MIDI input isn't part of
    // deviceManager's own state (MidiInputRouter opens a juce::MidiInput
    // directly), so it needs its own tiny settings file rather than riding
    // along in AudioDeviceState.xml. Just the chosen device's identifier
    // string, written by midiDeviceSelected() on every manual change and
    // read once at startup by restoreSavedMidiInputDevice() -- a device
    // identifier that no longer matches anything in availableMidiDevices
    // (unplugged since last launch) is silently ignored, same as JUCE's own
    // default-device fallback behavior elsewhere in this app.
    static juce::File getMidiInputDeviceSettingsFile();
    void restoreSavedMidiInputDevice();

    // Substitute MIDI keyboard: a mapped key (see virtualKeyboardKeyMap()
    // in VirtualKeyboardMaps.h) plays a note for as long as it's held, no
    // modifier required -- always active, same as a real MIDI keyboard.
    // Enter (toggleDrumGridMode()) switches these same keys over to the
    // 4x4 drum-pad grid (virtualDrumKeyMap()) instead; the two are mutually
    // exclusive. keyPressed() only fires once per physical press and has
    // no matching "key up" callback, so this is polled from timerCallback()
    // (~30Hz) via juce::KeyPress::isKeyCurrentlyDown() instead, diffing
    // against heldVirtualKeyboardKeys/heldVirtualDrumKeys to find
    // press/release transitions and feeding them through
    // midiInputRouter.injectNote() -- the same shared entry point real
    // MIDI input uses, so chords and live preview all just work without
    // any separate handling.
    void pollVirtualKeyboardInput();
    // Enter -- switches pollVirtualKeyboardInput() between the melodic
    // keyboard (virtualKeyboardKeyMap(), default) and the 4x4 drum grid
    // (virtualDrumKeyMap()) for the same physical keys. Neither map needs
    // a held modifier anymore, so this toggle is what disambiguates them
    // instead ("代わりに１６Pad 4x4の方は/で切り替え" -- moved from '/' to
    // Enter once '/' turned out to double as the melodic map's last key
    // AND the drum grid's last pad, see drumGridModeActive's declaration).
    void toggleDrumGridMode();
    // Ctrl+Z / Ctrl+X, semitone steps -- shifts virtualKeyboardTransposeSemitones,
    // applied only to the melodic map (not the drum grid, which is meant to
    // hit fixed instrument pitches).
    void adjustVirtualKeyboardTranspose(int deltaSemitones);
    // Ctrl+Shift+Z / Ctrl+Shift+X -- both keys are unmapped in
    // virtualDrumKeyMap() (see its declaration), so this doesn't collide
    // with any actual drum pad. Adjusts virtualKeyboardVelocity, the fixed
    // velocity every PC-keyboard note source (the melodic keyboard AND the
    // drum grid) uses, since neither can report a real physical
    // press-force the way a MIDI keyboard's velocity byte does. Also
    // nudges the stored velocity of whichever note(s) are currently
    // selected in the piano roll and plays them back so the change is
    // audible immediately -- same targeting as adjustNotePitch()'s T/G.
    void adjustVirtualKeyboardVelocity(float delta);
    // MIDI keyboard can play chords: heldMidiNotes tracks exactly what's
    // physically down (add/remove per note-on/off) for live-audio purposes,
    // while pendingChord captures the full chord as it's built up on
    // note-ons and is left untouched on note-offs -- so releasing the keys
    // one at a time (as chords normally are) doesn't erode it back down to
    // whatever's still held. A note-on while nothing else is down starts a
    // fresh chord capture, replacing whatever was pending before -- stays
    // until overwritten, then replaced on the next gesture.
    void handleMidiNoteChange(int noteNumber, float velocity, bool isOn);
    // Real-time REC onset capture, wrapped with "anticipation tolerance" at
    // a loop's wrap point: a note struck just BEFORE the loop wraps back to
    // its start (anticipating the wrap) is captured at the loop's start
    // step instead of its own literal near-the-tail raw position, so
    // recording can continue seamlessly across the loop boundary
    // ("繰り返しの後ろに作られたノートは、頭に戻った拍に置く。こうする
    // ことで繰り返しの中でRecできる"). Only called while
    // playbackEngine.isPlaying() (the count-in case is handled separately
    // via getCountInTargetStep()).
    int realtimeOnsetStep() const;
    void liveNote(int noteNumber, float velocity, bool isOn);
    void togglePlayback();
    // Shift+Space -- not a toggle like togglePlayback(): always (re)starts
    // playback with every track's cursor beginning at the edit cursor's
    // current step (PlaybackEngine::start(cursorStepIndex)), stopping first
    // if something was already playing. Session View slots still loop from
    // their own start regardless -- this only affects where the transport
    // itself begins counting from.
    void playFromLocator();
    void openInstrumentPanel();
    void openAudioMidiSettings();
    // Cmd+K: shows/hides the live keyboard-shortcut cheat-sheet window
    // (KeyboardOverlayWindow) -- created lazily on first toggle, then just
    // setVisible() flipped after that (unlike the plugin editor windows,
    // there's only ever one of these, and it never needs recreating).
    void toggleKeyboardOverlay();
    // Toggles the single global pluginEditorDesiredVisible switch (Cmd+P)
    // and applies it to the current track's window. Switching tracks while
    // it's on shows whichever track you land on's own window (if it has
    // one); switching while it's off shows nothing, and it stays off across
    // track switches until toggled back on -- there's no per-track memory.
    // No-op if the current track has no plugin loaded.
    void togglePluginEditor();
    // Applies pluginEditorDesiredVisible to every window in
    // pluginEditorWindowsByTrack: the current track's window (if any) shows
    // only if the global switch is on, every other track's window is
    // forced hidden. Called on every track switch and whenever the switch
    // changes. Never lets a shown window take keyboard focus -- activating
    // it would silently break every keyboard shortcut in the main editor
    // until you click back ("プラグインウィンドウをアクティブにしない。
    // アクティブにすると手の操作が止まってしまうため").
    void updatePluginEditorWindowVisibility();
    // A track's plugin instance was just replaced or removed -- any
    // existing PluginEditorWindow for it holds a reference to the now-
    // destroyed juce::AudioPluginInstance and must be torn down, not just
    // hidden. Called before setTrackInstrument() everywhere it's used.
    void invalidatePluginEditorWindow(int trackIndex);

    // Host-style "touch to automate" for plugin parameters -- see
    // MidiClip::parameterLanes' declaration. This component registers
    // itself as an AudioProcessorListener on every track's live plugin
    // (see registerParameterAutomationListener()/
    // unregisterParameterAutomationListener(), called alongside every
    // setTrackInstrument()) and uses the standard JUCE gesture-begin/end
    // pair to tell a genuine physical knob-turn in the plugin's own GUI
    // apart from a value change the plugin made itself (preset load,
    // internal LFO, or -- importantly -- OUR OWN playback applying
    // already-recorded automation, which must never re-record itself).
    void registerParameterAutomationListener(int trackIndex);
    void unregisterParameterAutomationListener(int trackIndex);
    // AudioProcessorListener overrides. All four early-out unless called
    // on the message thread -- a real user gesture always arrives there
    // (JUCE dispatches GUI events on the message thread), whereas
    // PlaybackEngine applying existing automation calls
    // setValueNotifyingHost() from the audio thread (see
    // PlaybackEngine::renderNextBlock()) and must never be mistaken for a
    // user touching something, both for correctness (it would just
    // re-record what was already there) and for thread-safety (this
    // class's own state -- touchedParameters, MidiClip -- is only ever
    // touched from the message thread otherwise).
    void audioProcessorParameterChanged(juce::AudioProcessor* processor, int parameterIndex, float newValue) override;
    void audioProcessorChanged(juce::AudioProcessor* processor, const juce::AudioProcessorListener::ChangeDetails& details) override;
    void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor* processor, int parameterIndex) override;
    void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor* processor, int parameterIndex) override;
    // -1 if processor isn't any track's current plugin (e.g. it was just
    // swapped out from under an in-flight callback).
    int findTrackIndexForProcessor(juce::AudioProcessor* processor);
    // Shared by writeAutomationPoint()'s sibling -- writes/replaces the
    // point at realtimeOnsetStep()-equivalent for trackIndex (NOT
    // necessarily cursorTrackIndex -- the touched plugin can belong to
    // any track), auto-creating clip.parameterLanes' entry for
    // parameterID the first time it's touched. Throttled the same way
    // recordAutomationPoint() is (automationRecordMinStepGap) since a
    // real knob drag fires many changes per second. This is Realtime-REC-
    // style capture -- audioProcessorParameterChanged() only calls this
    // while the transport is actually playing; see
    // previewTouchedParameterValue()'s sibling for the stopped/Manual case.
    void recordParameterAutomationPoint(int trackIndex, juce::AudioProcessorParameter& parameter, float value);
    // Manual-mode automation authoring: called instead of
    // recordParameterAutomationPoint() whenever a touch gesture's value
    // changes while the transport is STOPPED -- there's no playhead step to
    // write a point at yet, so this just follows the knob live into
    // parameterPendingValue (auto-creating/selecting that parameter's lane
    // and switching cursorTrackIndex/automationEditLane to match, same
    // "touch picks the parameter" role Touch mode already plays while
    // playing) so StepGridComponent's ghost marker tracks the knob at the
    // current cursor position. Nothing is actually written until an
    // explicit Cmd+Ctrl+I, exactly like the keyboard-driven pending value
    // Pitch Bend/Filter Cutoff already use ("Touchでオートメーションを書く
    // とき、MANUALでも書けるようにしたい。レーン上のAutomationの点を
    // パラメータの変化に合わせて上下させて、Commitすると点が撃たれる
    // ようにする").
    void previewTouchedParameterValue(int trackIndex, juce::AudioProcessorParameter& parameter, float value);
    // Cmd+Ctrl+W.
    void toggleAutomationTouchMode();

    // Editing commands, all reachable with hands on the keyboard home row.
    void ensureStepExists(int trackIndex, int stepIndex);
    void moveCursor(int deltaSteps);
    // Underlying note-aware navigation: jumps to the previous/next note's
    // start if the cursor is currently on/within a note (fast browsing of
    // existing content). direction is -1 or +1. fallbackToStep controls
    // what happens when there's no such note to jump to (a genuine rest,
    // or already at a note's own boundary with nothing further that way):
    // true falls back to moveCursor(direction) for precise placement
    // (extendNoteSelection()'s Shift+D/F use); false does nothing instead
    // -- plain d/f's pure note-to-note navigation, which is deliberately
    // NOT duration-preset aware ("Dfはノート単位で動く、B Vが指定した
    // 音価単位で動くにする" -- duration-preset movement is 'c'/'v' now,
    // "vbはcvに移動する").
    void moveCursorByNoteOrStep(int direction, bool fallbackToStep);
    // Plain 'f': pure note-to-note navigation, never commits -- committing
    // lives on Ctrl+V instead (see commitPendingNoteManually()). Duration-
    // preset-only movement is 'v' (advanceByDuration()).
    void handleForwardKey();
    // Plain 'd': pure note-to-note navigation, backward -- see
    // handleForwardKey(). No delete side effect; use Cmd+X/'a' to actually
    // delete a note. Duration-preset-only movement is 'c'
    // (retreatByDuration()).
    void handleBackwardKey();
    // Ctrl+V: commits a pending chord (if any) at the cursor -- the manual
    // "confirm" action Manual/Auto describe (see RecMode's declaration).
    // No-op with nothing pending, in Browse mode (recMode == Off), or in
    // Realtime mode while stopped (that's deliberately preview-only --
    // see RecMode::Realtime's declaration). Briefly lived on Cmd+F by
    // mistake, moved to Ctrl+F ("コミット間違えた。Cmd FじゃなくてCtrl F
    // にしたい"), then to Ctrl+V ("CommitをCtrl Vにする").
    void commitPendingNoteManually();
    // Shared by deleteAndRetreat() and clearCurrentStep(): clears every
    // step belonging to the note starting at ownerIndex (its own step plus
    // all tied-continuation steps).
    void deleteWholeNoteAt(int ownerIndex);
    // Shared by quantizeSelectedNotes()/unquantizeSelectedNotes(): moves
    // the note (root + tied-continuation chain) starting at fromIndex to
    // toIndex. If a genuine note already occupies toIndex, the moved
    // note's pitches are merged into it as a chord (same convention
    // commitPendingNoteAt() uses for a collision); otherwise the whole
    // chain is relocated intact. The vacated fromIndex range is cleared to
    // rests. No-op if fromIndex isn't currently a genuine note-start.
    void moveNoteTo(int fromIndex, int toIndex);
    // Called right after moveNoteTo(fromIndex, toIndex) by
    // quantizeSelectedNotes()/unquantizeSelectedNotes(): repoints whichever
    // of multiSelectedNoteStarts/cursorStepIndex was tracking fromIndex onto
    // toIndex instead. Without this, the selection/cursor were left
    // pointing at the now-empty rest the note just vacated -- the cyan
    // outline or narrowed selection looked like it had silently cleared
    // even though nothing was ever actually deselected ("選択して
    // クオンタイズをかけた後選択が外れてしまう"). cursorWasOnFromIndex must
    // be computed by the caller BEFORE calling moveNoteTo() -- by the time
    // this runs, fromIndex's old span is already vacated, so
    // findOwningNoteStepIndex() can no longer be used here to tell whether
    // the cursor used to be inside it.
    void updateSelectionAfterNoteMove(int fromIndex, int toIndex, bool cursorWasOnFromIndex);
    void switchTrack(int deltaTracks);
    // Plain 'v': doesn't write or clear anything -- just moves the cursor
    // forward by the current duration preset. Pure navigation, no undo
    // entry. Backward twin is retreatByDuration() ('b').
    void advanceByDuration();
    void retreatByDuration();
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
    // Cmd+5 (raiseLowest=true) / Cmd+R (raiseLowest=false): voicing-spread
    // command -- moves only the ONE extreme (lowest or highest) pitch of
    // effectiveSelectedPitches() by an octave, leaving every other selected
    // note in place. No-op if fewer than 2 notes are currently selected
    // (moving a single note's own extreme is already Shift+T/G's whole-note
    // octave shift) or if the shift would leave the MIDI range (0-127).
    void adjustSelectionVoicingEdge(bool raiseLowest);
    // Piano Roll only ('3'/'e' unmodified, or Cmd+Shift+W/Cmd+Shift+E(R) to
    // extend): narrows/moves the single-note selection within the chord at
    // the cursor. delta -1 = up ('3'), +1 = down ('e'). First press from a
    // fresh/stale state always lands on the highest pitch, regardless of
    // delta's direction; subsequent presses step by delta with circular
    // wraparound. extend=true adds the new focus pitch to
    // noteSelectionPitches instead of replacing it, building a multi-select.
    // See noteSelectionAnchorStep's declaration for how this interacts with
    // cursor movement, and effectiveSelectedPitches() for how adjustNotePitch()/
    // clearCurrentStep() consume the result.
    void navigateNoteSelection(int delta, bool extend);
    // The pitches an edit RIGHT NOW would affect: noteSelectionPitches if
    // noteSelectionAnchorStep still matches cursorStepIndex (intersected
    // against the chord's current pitches, defensively, in case it changed
    // underneath), otherwise every pitch in the chord at the cursor (the
    // "just arrived on a chord -> everything's selected" ground state --
    // this fallback is what makes narrowing entirely optional: nothing
    // calls this after a stale/absent selection, it just naturally reports
    // "the whole chord" until navigateNoteSelection() is used). Empty on a
    // rest. Used by adjustNotePitch()/clearCurrentStep()/auditionNoteAtCursor()
    // so their default behavior (nothing narrowed) is byte-for-byte
    // identical to before this feature existed.
    std::vector<int> effectiveSelectedPitches() const;
    // Shift+D(direction=-1)/Shift+F(direction=+1) -- extends the
    // time-axis multi-note selection used by quantize (see
    // multiSelectedNoteStarts's declaration). Reuses moveCursorByNoteOrStep()
    // for the actual movement, then adds whatever note the cursor landed on
    // (if any) to the selection set.
    void extendNoteSelection(int direction);
    // Cmd+A -- populates multiSelectedNoteStarts with every note in the
    // current track's clip, so a single following action (delete, pitch
    // shift, quantize, velocity, ...) applies to the whole clip at once
    // ("Cmd Aで全てのノートを選択"). Piano Roll only.
    void selectAllNotesInCurrentTrack();
    // The notes (by owning step index) a quantize action ('1'/'2'/'3'/'5')
    // would affect right now: multiSelectedNoteStarts if non-empty,
    // otherwise just the note under the cursor (if any) -- same
    // "narrow first, otherwise whatever's under the cursor" fallback
    // effectiveSelectedPitches() already uses for pitch selection, just on
    // the time axis instead.
    std::vector<int> effectiveSelectedNoteStarts() const;
    // '1'/'2'/'3' -- snaps effectiveSelectedNoteStarts() to the nearest
    // multiple of gridSteps (or its triplet width, gridSteps*2/3, if
    // quantizeTripletMode is on), actually relocating each note's data
    // within clip.steps. Non-destructive: the note's ORIGINAL step index is
    // remembered in Step::quantizedFromStep so unquantizeSelectedNotes()
    // ('5') can restore it later. Doesn't necessarily move all the way to
    // the grid line -- see quantizeAmountPercent's declaration. Shares its
    // move-with-merge logic with unquantizeSelectedNotes() via the private
    // moveNoteTo() helper. A thin StepEditGuard wrapper around
    // quantizeSelectedNotesImpl() -- see its declaration for why the two
    // are split.
    void quantizeSelectedNotes(int gridSteps);
    // The actual snapping logic, WITHOUT its own StepEditGuard -- split out
    // so autoQuantizeOnRecordEnabled's real-time-REC integration
    // (handleMidiNoteChange()) can call it directly from inside a
    // StepEditGuard it already has open, instead of nesting a second one
    // (StepEditGuard isn't reentrant-safe: nesting would push two separate
    // undo transactions for what should be one atomic "record + auto-
    // quantize" action, corrupting the undo history). quantizeSelectedNotes()
    // itself is just `{ StepEditGuard g(*this); quantizeSelectedNotesImpl(gridSteps); }`.
    void quantizeSelectedNotesImpl(int gridSteps);
    // '5' -- for each of effectiveSelectedNoteStarts(), if it has a stored
    // quantizedFromStep, moves it back there and clears the field.
    // No-op on notes that were never quantized.
    void unquantizeSelectedNotes();
    // Option+D ('direction' -1) / Option+F ('direction' +1) -- nudges
    // every note in effectiveSelectedNoteStarts() one base step left/right
    // (raw relocation, NOT quantize -- doesn't touch quantizedFromStep),
    // reusing moveNoteTo()'s own tie-chain/collision-merge handling
    // ("Option D, Fはノートや、おーとめーしょんのポイントを左右に移動
    // する"). Landing exactly on another note's own head merges pitches
    // into it, same convention moveNoteTo() already uses elsewhere.
    void nudgeSelectedNotes(int direction);
    void toggleQuantizeTripletMode(); // '4'
    // Cmd+U -- cycles quantizeAmountPercent through 25/50/75/100 and back to
    // 25, wrapping. Takes effect the next time '1'/'2'/'3' is pressed (doesn't
    // retroactively touch notes already quantized at a different amount).
    void cycleQuantizeAmount();
    // Cmd+Shift+U -- toggles autoQuantizeOnRecordEnabled (see its
    // declaration).
    void toggleAutoQuantizeOnRecord();

    // '1'/'2'/'4' -- selects noteRepeatGridSteps and turns
    // noteRepeatEnabled on, UNLESS gridSteps already matches the current
    // rate AND it's already on, in which case this turns it off instead
    // (so the same three keys both pick a rate and act as their own
    // off-switch, no separate key needed -- "note repeat機能をつけたい。
    // 1/4, 1/8, 1/16, トリプレットON/OFF").
    void setNoteRepeatRate(int gridSteps);
    void toggleNoteRepeatTripletMode(); // '5'
    // Called every timerCallback() tick -- while noteRepeatEnabled and at
    // least one note is currently held (heldMidiNotes), re-fires every
    // held pitch (a synthetic note-off then note-on, through the exact
    // same handleMidiNoteChange() path a real repeated key-press would
    // take) once per noteRepeatGridSteps/noteRepeatTripletMode interval at
    // the current tempo. Reuses the existing commit/live-preview pipeline
    // entirely instead of a parallel one: the note-off naturally auto-
    // commits/re-triggers audio exactly like tapping the key yourself
    // would, and the immediately-following note-on starts the next
    // interval's gesture, producing a rolling repeated pattern in whatever
    // REC mode is currently active (or just an audible retrigger in
    // Browse/Manual, same as any other held-note preview).
    void updateNoteRepeat();
    // Cmd+C -- captures effectiveSelectedNoteStarts() into noteClipboard,
    // each note's own step index turned into an offset relative to the
    // EARLIEST copied note, so pasteNotesAtCursor() can lay the whole group
    // back down with its internal timing intact no matter where the cursor
    // is when it's pasted. Read-only -- doesn't touch clip.steps or the
    // current selection.
    void copySelectedNotes();
    // Cmd+V -- writes every noteClipboard entry at cursorStepIndex +
    // its offsetSteps. A target that already holds a note gets the pasted
    // pitches merged into it as a chord (same collision convention
    // commitPendingNoteAt()/moveNoteTo() use); an empty target gets the
    // whole note (root + tied-continuation chain) written fresh. No-op if
    // nothing's been copied yet.
    void pasteNotesAtCursor();
    // Plays a brief noteOn/noteOff for whatever note (if any) is under the
    // cursor right now -- called after every cursor move so navigating with
    // d/f (or auto-advance from other commands) audibly "scrubs" through
    // existing content, and after adjustNotePitch() to confirm the new
    // pitch. Always suppressed during playback -- the transport's own
    // audio already covers what's playing, so a scrub/edit preview note
    // would just clash with it.
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

    // Metronome click during playback (project-wide, not undo-tracked, same
    // reasoning as the loop region above). 'w' toggles it -- one of the
    // only plain left-hand letters still free by the time this was added.
    // PlaybackEngine reads project.metronomeEnabled directly and renders
    // the click itself (see PlaybackEngine::renderMetronomeClicks()).
    void toggleMetronome();

    // Whether starting Real-time REC gives a 4-beat count-in first, or
    // starts immediately -- project-wide, same treatment as
    // metronomeEnabled just above. Shift+W (same letter as plain 'w'
    // Metronome, since both are playback pre-roll/click settings).
    void toggleCountIn();

    // Marks the current track's clip as ending at the edit cursor (plain
    // 'b', "Bound") -- see MidiClip::explicitLengthInSteps's declaration
    // for what this changes (trimming, playback/loop length, the piano-
    // roll's boundary marker). Piano-roll only (Session View has no step
    // cursor). Not undo-tracked, same reasoning as the loop markers below.
    void setClipEndHere();

    // Range duplication in the piano roll -- select a span of steps
    // (Shift+R/Cmd+R drop the start/end marker at the cursor, same
    // start/end-marker pattern the loop region above already uses) and
    // 'r' inserts a copy of it immediately after itself, pushing
    // everything from there on later in time (NOT an overwrite -- the
    // clip gets longer). rangeSelectionEnd <= rangeSelectionStart means
    // "no range marked," same "unset" convention loopEndStep/loopStartStep
    // use. Not undo-tracked for the markers themselves (UI state, matches
    // the loop markers' precedent) but duplicateSelectedRange() IS
    // undo-tracked (StepEditGuard) since it actually writes note data.
    void setRangeSelectionStart();
    void setRangeSelectionEnd();
    void duplicateSelectedRange();

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
    // Session View only ('a'): clears sceneClips[sessionCursorSlotIndex]
    // back to an empty MidiClip. No-op if that slot doesn't exist yet.
    // Stops the track first if it was currently playing that slot, and
    // unlinks editingSlotIndex if it pointed there -- otherwise the next
    // edit's live-link sync (refreshChildViews()) would just copy the
    // piano-roll buffer straight back into the slot, undoing the delete.
    void deleteClipAtCursor();
    // Session View only ('b'): copies sceneClips[sessionCursorSlotIndex]
    // into the NEXT slot (sessionCursorSlotIndex + 1, growing sceneClips to
    // fit, overwriting whatever was already there), then moves the cursor
    // onto the new copy. No-op if the cursor's current slot doesn't exist.
    void duplicateClipAtCursor();

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
    // sustainPedalEvents/pitchBendPoints/filterCutoffPoints joined steps
    // here so StepEditGuard's undo/redo tracking covers automation edits
    // too, not just note edits ("Undo, Redoにオートメーションも含めて") --
    // scheduleUpTo() reads all four from the audio thread, so they need
    // the exact same stop-before-reassign protection steps already had.
    void applyStepEdit(int trackIndex, const std::vector<Step>& steps,
                        const std::vector<SustainPedalEvent>& sustainPedalEvents,
                        const std::vector<AutomationPoint>& pitchBendPoints,
                        const std::vector<AutomationPoint>& filterCutoffPoints,
                        int cursorStep);
    void performUndo();
    void performRedo();

    // Step-grid VIEW controls -- pan/zoom, doesn't touch note data. The
    // grid's pitch window used to be hard-limited to a fixed 48-84 (C3-C6)
    // range with no way to scroll past it. Cmd+Option+3/E pan the visible
    // pitch range up/down (moved off plain 3/e once those became the
    // individual-note selection navigation -- see navigateNoteSelection()).
    // Horizontal (Cmd+F/Cmd+D) and
    // vertical (Cmd+3/Cmd+E) zoom are independent, not a combined control.
    void scrollStepGridPitch(int deltaSemitones);
    void zoomStepGridHorizontal(float factor);
    void zoomStepGridVertical(float factor);

    // Piano-roll scale tint (purely visual, doesn't restrict entry) --
    // key auto-estimated from the actual notes (KeyEstimator). Cmd+M
    // toggles Auto -> Off -> Auto.
    void cycleScale();

    // Committing: Shift+Z/Shift+X cycle a persistent note-duration preset
    // (commitDurationPresets/commitDurationPresetIndex below). Manual commit
    // is commitPendingNoteManually()'s job (Ctrl+V writes whatever's in
    // pendingChord -- the last note(s) heard from the MIDI/PC keyboard, not
    // necessarily still sounding, see pendingChord below -- into the step
    // grid at that duration and advances the cursor by it), but only while
    // recMode allows it -- see recMode's declaration below for the 4
    // user-visible modes this produces. Can be pressed repeatedly after
    // input stops to commit the same note(s) again. 'r' cycles recMode.
    void cycleRecMode();
    void cycleCommitDuration(int delta);
    // Pure write: merges pendingChord into the note already at targetStep,
    // or writes a fresh note + tied-continuation chain starting there.
    // Writes exactly `notes` (a caller-supplied subset, not necessarily the
    // whole pendingChord -- Real-time REC's auto-commit groups pendingChord
    // by each note's own onset step and calls this once per group, see
    // realtimeNoteOnsetSteps's declaration). Each note keeps its OWN
    // individually-measured StepNote::durationSteps (how long IT was
    // actually held, see handleMidiNoteChange()) for playback; the tie
    // chain itself is sized to whichever note in `notes` is longest, or
    // fallbackDurationSteps for a note that never got measured (e.g. a
    // manual 'f' commit while still held). Doesn't touch cursorStepIndex --
    // callers that want the cursor to follow the commit (manual 'f',
    // Step-REC auto-commit) do that themselves via commitPendingNote()
    // below; Real-time REC calls this directly so the edit cursor stays put
    // while the playhead keeps moving.
    void commitPendingNoteAt(int targetStep, int fallbackDurationSteps, const std::vector<StepNote>& notes);
    // A note with its own measured durationSteps should keep sounding for
    // exactly that long (see StepNote::durationSteps), independent of how
    // far this commit's own tie-chain envelope happens to extend -- the
    // envelope frequently gets truncated early by a completely unrelated,
    // DIFFERENT-pitch note that simply happened to get written into a
    // later step first (an everyday occurrence in real-time/legato
    // playing, not a real conflict at all). The one thing it genuinely
    // must not do is run into a FUTURE note-on for the exact SAME (already
    // shifted) pitch, which would double-trigger that MIDI note number
    // mid-ring. Scans forward from targetStep (called before this commit's
    // own tie-chain fill loop writes anything there, so it only ever sees
    // pre-existing, unrelated notes) and clamps down to fit if a same-
    // pitch collision is found within the requested span; -1/0 (unset)
    // passes through unchanged. PlaybackEngine trusts a positive
    // durationSteps exactly as stored -- this is the only place that
    // clamps it now, deliberately on the message thread rather than a
    // forward scan on the audio thread inside scheduleUpTo() (see its
    // matching comment).
    int clampDurationForPitchConflict(int targetStep, int shiftedPitch, int durationSteps) const;
    // Thin wrapper: commitPendingNoteAt(cursorStepIndex, <current duration
    // preset>, pendingChord) then advances the cursor by that same preset.
    void commitPendingNote();
    // Applies octaveShiftOctaves to a raw pendingChord pitch, clamped
    // 0-127 -- shared by commitPendingNote()/commitPendingNoteAt() and
    // updatePendingNoteDisplays() so the preview always matches exactly
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
    // directly from handleMidiNoteChange() so the
    // preview updates live as a new pitch arrives, not just whenever
    // refreshChildViews() happens to run for some unrelated reason.
    void updatePendingNoteDisplays();

    // If currentScaleType == Auto, re-runs KeyEstimator and syncs
    // scaleRootPitchClass/scaleIsMinor to its guess first. Then pushes the
    // resulting scale to stepGrid as a 12-entry in-scale mask and to
    // transportBar's KEY badge. Called from refreshChildViews() and
    // cycleScale().
    void updateStepGridScale();

    // Re-runs ChordEstimator over the whole project and pushes the result
    // to chordEstimateBar. Called from refreshChildViews() -- cheap enough
    // (a handful of tracks/bars) to just always recompute rather than
    // tracking dirtiness.
    void updateChordEstimates();

    MidiInputRouter midiInputRouter;

    juce::ComboBox midiDeviceBox;
    juce::Array<juce::MidiDeviceInfo> availableMidiDevices;

    // Enter toggles this -- false (default) = the melodic keyboard
    // (virtualKeyboardKeyMap()) is what pollVirtualKeyboardInput() reads;
    // true = the 4x4 drum grid (virtualDrumKeyMap()) instead. The two note-
    // performance inputs share physical keys (see virtualDrumKeyMap()'s
    // declaration), so exactly one is ever active at a time.
    bool drumGridModeActive = false;

    // Keys currently down for the virtual keyboard / drum grid, as of the
    // last pollVirtualKeyboardInput() poll -- diffed against each new poll
    // to find press/release transitions. Each entry remembers the pitch
    // that key's note-on actually used, so note-off targets the same pitch
    // even if virtualKeyboardTransposeSemitones changes mid-hold. One of
    // the two is always empty -- see drumGridModeActive above.
    std::vector<std::pair<char, int>> heldVirtualKeyboardKeys;
    std::vector<std::pair<char, int>> heldVirtualDrumKeys;

    bool wasSustainKeyDown = false; // rising-edge detection, see pollVirtualKeyboardInput()

    // Two independent SOURCES of sustain -- a real MIDI keyboard's own
    // physical pedal (CC64, arrives as a controller message via
    // MidiInputRouter::onLiveControllerMessage) and the PC-keyboard's
    // Ctrl+S (polled in pollVirtualKeyboardInput()) -- both funnel into the
    // same recordSustainPedalEvent() below and, for live audio, both send a
    // genuine CC64 juce::MidiMessage to PlaybackEngine::liveMidiMessage()
    // (the PC-keyboard side is a synthetic-but-real message, since it has
    // no actual MIDI hardware to have generated one).
    //
    // Recorded as genuine CC64 automation (MidiClip::sustainPedalEvents),
    // NOT approximated by lengthening the note -- an earlier version did
    // exactly that (grew StepNote::durationSteps to cover however long the
    // pedal held a note over), which meant playback never actually resent
    // CC64 to the loaded synth/plugin at all: it just played an
    // artificially longer note. That loses any pedal-specific behavior the
    // instrument itself has (release resonance samples, layered pedal-down
    // timbre, multiple notes tied together by one press, etc.) -- live
    // preview sounded authentic because the real pedal message reaches the
    // synth directly while playing, but the RECORDED result never
    // reproduced it the same way ("サスティンをCCとして記録せずに、ただの
    // 長いノートとして記録している？だとしたら仕様が全然違う"). Now
    // PlaybackEngine::scheduleUpTo() resends the actual recorded CC64
    // events during playback, so it sounds the same both times.
    bool midiSustainPedalDown = false;
    void recordSustainPedalEvent(bool pedalDown);
    // Sends the actual deferred playbackEngine.liveNoteOff() for every
    // pitch liveNote() held back -- see sustainedLiveNotePitches'
    // declaration. Called once sustain is confirmed released, from
    // whichever source (real pedal debounce-confirm, PC-keyboard Ctrl+S
    // release) just set midiSustainPedalDown back to false.
    void flushSustainedLiveNotes();
    // Immediately confirms a still-pending sustain crossing (see
    // pendingSustainCrossingMs's declaration) instead of waiting out the
    // rest of sustainSettleMs -- call this BEFORE stopping
    // playback/Real-time REC (togglePlayback(), etc.). Without it, lifting
    // the pedal and then stopping within the debounce window (a completely
    // normal way to end a take -- release the pedal, then hit stop) left
    // recordSustainPedalEvent()'s own isPlaying() gate silently dropping
    // that final release the instant the debounce timer got around to
    // confirming it, moments after playback had already stopped ("Recの
    // 時４小説目の最後のSustainは記録されなかった"). No-op if nothing is
    // actually pending.
    void forceConfirmPendingSustainRelease();

    // A real analog/half-damper pedal (confirmed via MIDI Monitor on a
    // Roland A-88MK2) doesn't send a clean single CC64=127-then-eventually-
    // 0 transition -- it sends jittery bursts around a 0 reading, sometimes
    // clean single dips, sometimes several rapid flips in a row (e.g. a
    // confirmed "...0[t], 127[t+190ms], 0[t+200ms], 127[t+210ms]..." run,
    // or two CC64 values arriving at the literal SAME millisecond -- both
    // physically impossible for an actual foot, i.e. unambiguous electrical
    // noise). Several elapsed-time-based debounce/hysteresis schemes were
    // tried here previously (60ms, then 200ms release + 40ms "rise must
    // persist to cancel a pending release") and each was eventually proven
    // wrong by real data, because elapsed time alone cannot separate
    // hardware noise from a genuine QUICK re-pedal -- standard legato
    // piano pedaling is lift-then-immediate-repress, and a real one was
    // measured on this hardware at just 50ms from dip to rise
    // (06:17:55.232 -> 55.282, preceded by a smooth monotonic decline
    // 50->26->25->24->22->18->13->8->3->0, i.e. clearly a real foot, not a
    // glitch). Any "does the rise persist long enough to cancel the
    // release" rule is structurally unable to keep this: a real repedal's
    // whole POINT is that the new press holds, so it always looks
    // identical to "the dip must have been noise" no matter how the
    // window is sized ("サスティンが全然意図通りに入らない" -- confirmed:
    // every deliberate repedal in a whole take was being silently merged
    // into one unbroken sustain-on block).
    //
    // Replaced with a single settle-window debounce instead: any raw
    // threshold-crossing (0 <-> nonzero, tracked in lastRawSustainDown,
    // separate from the currently-APPLIED midiSustainPedalDown) arms/
    // re-arms pendingSustainCrossingMs. Once sustainSettleMs pass with no
    // FURTHER crossing, resolvePendingSustainCrossing() applies whatever
    // the raw signal is doing AT THAT MOMENT as the new state -- a single
    // clean crossing resolves after one quiet window; several rapid flips
    // (a burst) just keep re-arming the same pending window and collapse
    // into ONE resolution once they finally go quiet, using the LAST
    // (settled) raw reading rather than firing a separate event per flip.
    // This intentionally does NOT try to distinguish "burst = ignore,
    // revert to whatever state existed before it" from "burst = genuine
    // transition with some contact bounce on re-engagement" -- confirmed
    // real data has clusters of BOTH kinds, sitting on the same hardware,
    // often the same physical pedal-down event (see the comment above),
    // and there is no reliable signal in CC64 alone to tell them apart in
    // general. Trusting the settled value is the same "just look at what
    // the signal is actually saying" bias the threshold fix below already
    // used successfully.
    //
    // sustainSettleMs (25ms) sits between the shortest confirmed noise gap
    // seen on this hardware (~10ms between flips within one burst) and the
    // shortest confirmed genuine repedal gap (50ms) -- resolved either the
    // moment a NEW crossing arrives late enough to prove the window has
    // elapsed (checked synchronously against real elapsed time in
    // onLiveControllerMessage, not tied to the UI timer's tick rate), or
    // by timerCallback()'s periodic poll for the tail case where nothing
    // else ever arrives (an isolated final release/press with nothing
    // following).
    //
    // The >0 (not >=64) threshold below is unchanged from an earlier fix
    // ("0とそれ以外でON/OFFを判定したら？") -- ordinary half-pedal wobble
    // on this hardware (20, 33, 42, 53, 70, 72, 74, 75, 78...) never
    // touches a literal 0, so it never even registers as a crossing here
    // in the first place; only real dips-to-zero (genuine releases or
    // actual glitches) reach this settle logic at all. The PC-keyboard's
    // Ctrl+S doesn't need any of this (a clean digital keystate, not
    // jittery analog hardware) -- it sets midiSustainPedalDown directly.
    bool lastRawSustainDown = false;
    double pendingSustainCrossingMs = -1.0;
    static constexpr double sustainSettleMs = 25.0;
    // Applies lastRawSustainDown as the new midiSustainPedalDown if it
    // differs from the currently-applied state (a no-op if the raw signal
    // settled back to where it already was, e.g. a burst that resolved
    // with no net change) -- forwards the real CC64 to the synth, records
    // the automation event, and flushes deferred notes on a release. Called
    // from onLiveControllerMessage/timerCallback once pendingSustainCrossingMs
    // has been quiet for sustainSettleMs, and from
    // forceConfirmPendingSustainRelease() to resolve early before playback
    // stops.
    void resolvePendingSustainCrossing();

    // liveNote()'s own audio-level sustain hold-over -- deliberately
    // separate from midiSustainPedalDown's genuine CC64 recording/
    // forwarding above. Recorded automation and live audio USED to both
    // rely purely on the loaded synth/plugin's own internal CC64 handling
    // (this app only ever forwards the raw message) -- but at least one
    // real instrument (confirmed: a Kontakt-hosted patch via Komplete
    // Kontrol) turned out to release ANY currently-sounding note the
    // instant it saw CC64 cross below 64, including ones whose key was
    // still being genuinely, physically held down and had never actually
    // received a note-off at all -- not just the ones it was legitimately
    // sustaining over ("Sustain OFFの時NoteもOFFされて聞こえる"). That's
    // not standard sustain-pedal behavior for any well-behaved instrument,
    // but there's no portable way to fix a specific plugin's own DSP/
    // scripting from here.
    //
    // Implemented at the application level instead, independent of
    // whatever the loaded instrument does with the raw CC64 it also still
    // receives: liveNote()'s note-off branch defers sending
    // playbackEngine.liveNoteOff() for as long as midiSustainPedalDown is
    // true, so from the SYNTH's own perspective that note simply never
    // got a note-off yet -- there is nothing for even a trigger-happy
    // pedal-release handler to prematurely release. Deliberately scoped to
    // ONLY this audio-side call, not handleMidiNoteChange()'s own
    // heldMidiNotes/pendingChord/commit bookkeeping above it (which keeps
    // processing every note-off immediately, exactly as before) -- an
    // earlier version of this whole feature deferred at THAT level
    // instead, which blocked Real-time REC's entire gesture/auto-commit
    // pipeline for as long as the pedal stayed down ("弾いている時、Rec
    // しているときはサスティンが効いているが、実際にはRecされない"); this
    // redesign can't reintroduce that bug because recording is completely
    // unaffected by it.
    //
    // ADDENDUM: the above only covers notes that get a real note-off WHILE
    // the pedal is down. It turns out the SAME buggy instrument also reacts
    // to the raw CC64=0 message itself, independent of any note-off at
    // all -- a note that is STILL being genuinely held (key never
    // released) was still getting cut the instant CC64=0 reached the
    // synth ("Note ONが続いている中で、SUSTAINがOFFになると音が途切れる。
    // NoteONである限り音は継続すべき"). A note that's still down should be
    // completely unaffected by pedal state, by definition. Deferring the
    // forward here at the app level (an earlier attempt, since removed)
    // only ever covered THIS live-preview forward -- resolvePendingSustainCrossing()
    // also records the transition, and PlaybackEngine::scheduleUpTo()
    // independently RESENDS whatever recordSustainPedalEvent() just wrote
    // to clip.sustainPedalEvents during playback, moments later and
    // completely bypassing anything deferred only here. Fixed instead in
    // PlaybackEngine itself (see TrackAudioState::activeNotePitches), the
    // one place both the live and scheduled/recorded paths actually
    // converge before reaching the plugin -- CC64 is now always forwarded
    // from here immediately (never deferred), and PlaybackEngine is solely
    // responsible for immediately re-triggering any note the plugin
    // incorrectly kills on a real CC64=0.
    std::vector<int> sustainedLiveNotePitches;

    // Keyboard-only automation editing (Cmd+Ctrl+A toggles), covering
    // sustain plus three continuous lane KINDS -- see
    // MidiClip::AutomationPoint's declaration. Piano Roll only (no meaning
    // in Session View, same as the note-editing keys). Cycled with
    // Cmd+Ctrl+L, which also cycles through however many
    // MidiClip::parameterLanes currently exist (Parameter, one "slot"
    // covering all of them -- see automationEditParameterLaneIndex's
    // declaration for which specific one).
    //
    // A Parameter lane is only ever CREATED by physically touching a
    // plugin's own knob (Cmd+Ctrl+W's Touch mode) -- but once it exists,
    // it's edited exactly the same way as Pitch Bend/Filter Cutoff (Cmd+Ctrl+
    // Z/X adjust value, Cmd+Ctrl+I inserts a point, Cmd+Ctrl+V toggles curve
    // type, Cmd+C/V copy/paste, etc.), NOT read-only. Touch input's actual
    // job is narrower than "recording" in the traditional DAW sense -- it's
    // the most intuitive way to pick WHICH of a plugin's however-many
    // parameters you even want a lane for (moving the real knob IS the
    // parameter picker), after which the lane is just an automation lane
    // like any other and the keyboard does the rest ("できたレーンは全て
    // 同じ扱いにする。Touch入力をきっかけにレーンができたとしても、PC
    // キーボードから調整や新たな点の追加はする。むしろ、Touch入力をするの
    // は、どのプラグインのどのパラメータを操作したいのかを取り込むため").
    enum class AutomationLane { Sustain, PitchBend, FilterCutoff, Parameter };
    bool automationEditModeActive = false;
    AutomationLane automationEditLane = AutomationLane::Sustain;
    // Which entry of project.tracks[cursorTrackIndex].clip.parameterLanes
    // is current, only meaningful while automationEditLane == Parameter.
    // Cmd+Ctrl+L steps through 0, 1, 2, ... in order, then wraps back to
    // Sustain once it runs past the last one.
    int automationEditParameterLaneIndex = 0;
    // The value a manually-drawn point would be placed at right now --
    // t/g (fine) and Shift+T/G (coarse) adjust whichever of these belongs
    // to automationEditLane; Cmd+Ctrl+I commits a point at the cursor with
    // it. Tracked per-lane (not one shared value) so switching lanes and
    // back doesn't lose your place. 8192 = pitch bend center/no bend; 64
    // is just a reasonable starting point for filter cutoff, not a
    // meaningful default in the way 8192 is.
    int pitchBendPendingValue = 8192;
    int filterCutoffPendingValue = 64;
    // The curve type (see AutomationCurveType's declaration -- curve-on-
    // arrival) that Cmd+Ctrl+I would give the NEXT point placed in this
    // lane. Cmd+Ctrl+V toggles this (Curve/Step) when no real point sits
    // at the cursor (it toggles that point's own curveType instead when
    // one does) -- see cycleAutomationCurveTypeAtCursor()'s declaration.
    // Also drives the ghost preview's incoming-segment shape
    // (StepGridComponent::setAutomationPendingCurveType()) so the shape
    // AND position of a not-yet-placed point can be judged together
    // before committing ("終点を打つ前に...カーブの形と終点の位置を
    // 決める方がユーザフレンドリーだと思う").
    AutomationCurveType pitchBendPendingCurveType = AutomationCurveType::Curve;
    AutomationCurveType filterCutoffPendingCurveType = AutomationCurveType::Curve;
    // The continuous curveAmount (-1..+1, see AutomationPoint's
    // declaration) the NEXT point placed in this lane would get, while
    // curveType == Curve. Cmd+Ctrl+Z/X (fine) and Cmd+Ctrl+Shift+Z/X
    // (coarse) adjust whichever of these belongs to automationEditLane --
    // repurposed off what used to be an "alternate binding" duplicate of
    // t/g's own value-adjust (the shortcut help text called it exactly
    // that), since a discrete 4-way Linear/EaseIn/EaseOut/Step cycle
    // turned out both hard to tell apart visually and too coarse to dial
    // in a specific feel ("どちらかというとEaseIn,EaseOutの傾斜を調整
    // できる必要がありそう"). See adjustAutomationPendingCurveAmount()'s
    // declaration.
    float pitchBendPendingCurveAmount = 0.0f;
    float filterCutoffPendingCurveAmount = 0.0f;
    // Parameter lanes' own pending value/curve, mirroring
    // pitchBendPendingValue/filterCutoffPendingValue and their curve
    // siblings above -- but ONE shared set covering every Parameter lane
    // (not one per lane) rather than a full per-lane set, since landing the
    // cursor on any real point in ANY Parameter lane immediately overwrites
    // these with that point's own value/curve anyway (see moveCursor()'s
    // pickup logic) -- the same self-healing behavior PitchBend/
    // FilterCutoff already rely on, so per-lane storage would only ever
    // matter for the split second before the cursor's first move. Range is
    // 0.0-1.0 (JUCE's own normalized AudioProcessorParameter range, see
    // ParameterAutomationPoint's declaration), unlike PitchBend/FilterCutoff's
    // int ranges.
    float parameterPendingValue = 0.5f;
    AutomationCurveType parameterPendingCurveType = AutomationCurveType::Curve;
    float parameterPendingCurveAmount = 0.0f;

    void toggleAutomationEditMode();          // Cmd+Ctrl+A
    void cycleAutomationLane();               // Cmd+Ctrl+L
    // direction: -1 ('b') or +1 ('g'). coarse: Shift held.
    void adjustAutomationPendingValue(int direction, bool coarse);
    void toggleSustainEventAtCursor();        // Cmd+Ctrl+S
    void insertAutomationPointAtCursor();     // Cmd+Ctrl+I
    void deleteAutomationPointAtCursor();     // Cmd+Ctrl+D
    // Option+D ('direction' -1) / Option+F ('direction' +1) -- nudges
    // every point/event in effectiveSelectedAutomationSteps() one base
    // step left/right (all three lanes, including Sustain) -- see
    // nudgeSelectedNotes()'s sibling declaration
    // ("Option D, Fはノートや、おーとめーしょんのポイントを左右に移動
    // する"). Landing exactly on another point's step overwrites it, same
    // "later take wins" convention writeAutomationPoint() already uses.
    void nudgeSelectedAutomationPoints(int direction);
    // Toggles the curve TYPE (Curve/Step, see AutomationCurveType's
    // declaration -- curve-on-arrival) of whatever real point sits
    // exactly at the cursor (or every point in a Shift+D/F multi-
    // selection), if one exists there. If none does, toggles the PENDING
    // curve type instead (pitchBendPendingCurveType/
    // filterCutoffPendingCurveType/parameterPendingCurveType) that the
    // next Ctrl+V/Cmd+Ctrl+I placement will use -- PitchBend/FilterCutoff/
    // Parameter all support this equally (a Parameter lane's own points
    // carry the same curveType/curveAmount fields, see
    // ParameterAutomationPoint's declaration -- Touch input only decides
    // WHICH plugin parameter a lane belongs to, not how it's edited
    // afterward), no-op only on the Sustain lane (a binary on/off event
    // list has no curve between points to shape). The continuous strength
    // half of a Curve point's shape is separate -- see
    // adjustAutomationPendingCurveAmount().
    void cycleAutomationCurveTypeAtCursor();  // Cmd+Ctrl+V
    // direction: -1 (Cmd+Ctrl+Z) or +1 (Cmd+Ctrl+X). coarse: Shift held
    // (Cmd+Ctrl+Shift+Z/X). Adjusts curveAmount -- see pitchBendPendingCurveAmount's
    // declaration -- with the same "moves an existing point at the cursor
    // right away, otherwise just the pending amount" live-update pattern
    // adjustAutomationPendingValue() already uses for the point's value,
    // and the same effectiveSelectedAutomationSteps() bulk-shift behavior
    // for a Shift+D/F multi-selection. No-op on the Sustain lane (same
    // reasoning as cycleAutomationCurveTypeAtCursor()) or on a point whose
    // curveType is Step (curveAmount is unused there).
    void adjustAutomationPendingCurveAmount(int direction, bool coarse);
    // Parameter lane sibling of writeAutomationPoint() -- same "later take
    // replaces" write, just float-valued (0.0-1.0) to match
    // ParameterAutomationPoint. Used by insertAutomationPointAtCursor()'s
    // Parameter branch; deliberately NOT shared with
    // recordParameterAutomationPoint() (Touch capture)'s own inline write,
    // to avoid touching that already-hands-on-tested path.
    void writeParameterAutomationPoint(std::vector<ParameterAutomationPoint>& points, int stepIndex, float value,
                                        AutomationCurveType curveType, float curveAmount);
    // direction: -1 ('d') or +1 ('f'). Jumps straight to the nearest
    // existing point/event in automationEditLane on that side of the
    // cursor (Sustain: sustainPedalEvents; PitchBend/FilterCutoff: their
    // own points vector) instead of the usual note/step navigation --
    // automation edit mode's own equivalent of d/f's normal "move to the
    // next note" ("オートメーションレーンに言ったら、d fはオートメーションの
    // ポイントを移動する"). Falls back to plain single-step movement when
    // the lane has no points at all yet (nothing to jump between), and
    // simply holds still past the first/last point in that direction
    // (no wraparound).
    // clearSelection: true for plain d/f ("Shift extends, plain move
    // collapses to one" -- see multiSelectedAutomationSteps's own
    // declaration). extendAutomationSelection() (Shift+D/F) passes false
    // so hopping to the next point doesn't wipe out what's already
    // selected -- calling this WITH the default from there was a bug: it
    // silently capped the selection at one point no matter how many times
    // Shift+D/F was pressed ("オートメーションのポイントのShift押しながら
    // 複数選択").
    void moveCursorToAdjacentAutomationPoint(int direction, bool clearSelection = true);
    // The stepIndex of every point/event currently in automationEditLane
    // (Sustain: sustainPedalEvents; PitchBend/FilterCutoff: their own
    // points vector), ascending -- shared by moveCursorToAdjacentAutomationPoint()
    // and deleteAutomationPointAtCursor()'s post-delete fallback so both
    // read the exact same notion of "what's actually in this lane".
    std::vector<int> automationStepIndicesForCurrentLane() const;

    // Automation edit mode's equivalents of extendNoteSelection()/
    // effectiveSelectedNoteStarts()/copySelectedNotes()/pasteNotesAtCursor()
    // -- same design, same keys (Shift+D/F, Cmd+C/V), just operating on
    // multiSelectedAutomationSteps/automationClipboard instead
    // ("複数選択+一括操作...揃えられるところは揃えたい"). direction: -1
    // (Shift+D) or +1 (Shift+F).
    void extendAutomationSelection(int direction);
    // What a following bulk action (delete/'a', value adjust/t,g, curve
    // cycle/Cmd+Ctrl+V, copy/Cmd+C) would affect right now:
    // multiSelectedAutomationSteps if non-empty, else just the point/event
    // exactly at the cursor if one exists there, else empty.
    std::vector<int> effectiveSelectedAutomationSteps() const;
    void copySelectedAutomationPoints();      // Cmd+C
    void pasteAutomationPointsAtCursor();     // Cmd+V
    // Shift+A's twin -- jumps to the clip's own effective end instead of
    // back to bar 1. Not automation-specific (plain moveCursor(), same as
    // Shift+A) but added alongside this feature since neither mode had a
    // "jump to end" before now ("先頭/末尾へジャンプ...揃えられるところは
    // 揃えたい" -- notes didn't have this either, so both gain it together
    // rather than automation ending up with an ability notes still lack).
    void jumpToClipEnd();                     // Cmd+Shift+A

    // Shared by both the manual editor above (unconditional -- drawing by
    // hand isn't gated on REC state) and real MIDI hardware capture during
    // Real-time REC (gated, see its own definition) -- writes one
    // AutomationPoint into the given lane's vector at the given step,
    // replacing (not accumulating alongside) any point a previous take
    // already left at that exact step, same reasoning as
    // recordSustainPedalEvent()'s own re-recording guard.
    void writeAutomationPoint(std::vector<AutomationPoint>& points, int stepIndex, int value,
                               AutomationCurveType curveType = AutomationCurveType::Curve, float curveAmount = 0.0f);
    // Real MIDI pitch wheel / mod wheel (CC74) hardware capture during
    // Real-time REC -- see recordSustainPedalEvent()'s declaration for why
    // this only writes while recMode==Realtime && isPlaying(). No
    // debounce needed (unlike CC64's binary threshold-crossing jitter, a
    // continuous value has nothing to debounce) but IS throttled so a real
    // wheel's flood of MIDI messages doesn't produce a point at every
    // single one -- only once at least automationRecordMinStepGap steps
    // have passed since the last recorded point for that lane.
    void recordAutomationPoint(AutomationLane lane, int value);
    int lastPitchBendRecordStep = -1;
    int lastFilterCutoffRecordStep = -1;
    static constexpr int automationRecordMinStepGap = 8;

    // Cmd+Ctrl+W -- see toggleAutomationTouchMode()'s declaration and the
    // transport bar's own AUTO: READ/TOUCH badge for what this actually
    // does. Read-only playback of parameterLanes (already-recorded plugin-
    // parameter automation) happens regardless of this flag; Touch only
    // additionally arms NEW recording via a physical knob-turn.
    bool automationTouchModeEnabled = false;
    // (trackIndex, parameterIndex) pairs with an open gesture right now --
    // see audioProcessorParameterChangeGestureBegin/End()'s declarations.
    std::set<std::pair<int, int>> touchedParameters;
    // Per (trackIndex, parameterID) throttle, same reasoning/constant as
    // lastPitchBendRecordStep/lastFilterCutoffRecordStep above.
    std::map<std::pair<int, juce::String>, int> lastParameterRecordStep;
    // Manual-mode (stopped) live-preview values -- see
    // previewTouchedParameterValue()'s declaration. Keyed by (trackIndex,
    // index into that track's clip.parameterLanes). A single physical
    // touch can drive several plugin parameters at once (a macro control,
    // linked/morphed parameters) -- audioProcessorParameterChanged() fires
    // once per parameter, and this holds a live entry for every one of
    // them simultaneously (not just whichever lane Cmd+Ctrl+L currently
    // has selected -- parameterPendingValue above still tracks that one
    // alone, for keyboard-driven Cmd+Ctrl+Z/X editing), so all their
    // points move/get shown together and Cmd+Ctrl+I commits every one of
    // them at once ("プラグインから同時に複数のパラメータが変更される
    // 場合は、変更される全ての点を動かす。レーンの外にあるパラメータも
    // 動かす"). Cleared at the start of a genuinely fresh, unrelated touch
    // session (see audioProcessorParameterChangeGestureBegin()) so an old
    // abandoned preview never silently rides along into a much-later
    // Cmd+Ctrl+I; gestures that begin while another is already open (the
    // multi-parameter case) join the same batch instead. ALSO cleared
    // right after a successful Cmd+Ctrl+I commit (see
    // insertAutomationPointAtCursor()) -- unlike pitchBendPendingValue/
    // filterCutoffPendingValue, which deliberately DO stick around after a
    // commit so the same single value can be placed again elsewhere, an
    // entire multi-parameter BATCH silently re-committing itself at every
    // later cursor position/Cmd+Ctrl+I was surprising rather than useful
    // ("一括して打てるけど、その後もcvでセットで動いてしまうのはなぜ？").
    // Any parameter still being actively touched re-populates its own
    // entry on its very next change regardless, so an in-progress gesture
    // is never affected by this clearing.
    std::map<std::pair<int, int>, float> touchPreviewValues;

    // Ctrl+Z / Ctrl+X, in semitones -- see adjustVirtualKeyboardTranspose().
    int virtualKeyboardTransposeSemitones = 0;

    // Ctrl+Shift+Z / Ctrl+Shift+X, 0.0-1.0 -- see adjustVirtualKeyboardVelocity().
    // Default 0.6 (shown as "VEL: 60%" in the transport bar).
    float virtualKeyboardVelocity = 0.6f;

    juce::TextButton playButton{ "Play" };
    juce::TextButton instrumentButton{ "Instrument" };
    juce::TextButton audioSettingsButton{ "Audio/MIDI" };
    PlaybackEngine playbackEngine;
    PluginHost pluginHost;

    std::unique_ptr<InstrumentPanelWindow> instrumentPanelWindow;
    std::unique_ptr<KeyboardOverlayWindow> keyboardOverlayWindow;
    // Per-track plugin editor windows, created lazily (only once a track's
    // "show editor" is actually requested) and kept alive-but-hidden when
    // you switch away, so returning to a track shows the same window state
    // (scroll position, open menus) rather than a freshly recreated editor.
    // pluginEditorDesiredVisible is a single GLOBAL on/off switch (NOT
    // remembered per track) -- once toggled off, no track's window shows
    // again until explicitly toggled back on, even after switching tracks
    // ("表示か非表示かを全体で決める。一度非表示にしたら他トラックでも
    // 表示しない"). Only the CURRENT track's window is ever actually shown
    // even while this is on (see updatePluginEditorWindowVisibility()) --
    // this just decides whether that one window is allowed to show at all.
    std::map<int, std::unique_ptr<PluginEditorWindow>> pluginEditorWindowsByTrack;
    bool pluginEditorDesiredVisible = false;
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

    // project.tracks.reserve()'d to this many slots at every point the
    // vector gets (re)constructed (constructor, newProject(), openProject())
    // so addTrack() can push_back() without ever reallocating -- see
    // addTrack()'s comment for why that matters while playing. Nowhere near
    // a real ceiling on track count (just the point where a real project
    // would need more than this many tracks and the vector would need to
    // grow/reallocate again, which addTrack() falls back to stop()ing
    // playback for, same as before this reserve existed).
    static constexpr size_t reservedTrackCapacity = 64;

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

    // Individual-note selection within the chord at the cursor --
    // see navigateNoteSelection()/effectiveSelectedPitches(). Not undo-
    // tracked (UI focus state, not note data). noteSelectionAnchorStep is
    // the cursorStepIndex this selection was computed for; a mismatch means
    // "stale," and effectiveSelectedPitches() falls back to "every pitch in
    // the chord at the (possibly new) cursor" without any explicit reset
    // needed on cursor-movement code paths.
    int noteSelectionAnchorStep = -1;
    int noteSelectionFocusPitch = -1; // -1 = not narrowed yet (whole-chord state)
    std::vector<int> noteSelectionPitches;

    // Shift+D/Shift+F (extendNoteSelection()) accumulates note-start step
    // indices into this as you move from note to note, for quantize
    // ('1'/'2'/'3'/'5') to act on more than one note at once -- see
    // effectiveSelectedNoteStarts(). Cleared by any PLAIN d/f move
    // (handleForwardKey()/handleBackwardKey()) -- "Shift extends, plain
    // move collapses to a single note," the standard selection-UI
    // convention. Not undo-tracked (UI focus state, not note data).
    std::vector<int> multiSelectedNoteStarts;

    // Cmd+C (copySelectedNotes()) / Cmd+V (pasteNotesAtCursor()). Each
    // entry is one copied note: offsetSteps is its distance from the
    // EARLIEST note in the copied group's own original step index (so a
    // multi-note copy keeps its internal spacing when pasted elsewhere),
    // rootStep is that note's own step data (quantizedFromStep reset to -1
    // -- a pasted note is a fresh note, not a continuation of the
    // original's quantize history), and tieContinuation is any tied
    // continuation steps that followed it (empty for an untied note).
    struct CopiedNote
    {
        int offsetSteps = 0;
        Step rootStep;
        std::vector<Step> tieContinuation;
    };
    std::vector<CopiedNote> noteClipboard;

    // Automation edit mode's twin of multiSelectedNoteStarts/noteClipboard
    // above -- see extendAutomationSelection()/copySelectedAutomationPoints()'s
    // declarations. One shared shape for all three lanes: value doubles as
    // the Sustain lane's pedalDown (0/nonzero), curveType is unused there.
    std::vector<int> multiSelectedAutomationSteps;
    struct CopiedAutomationPoint
    {
        int offsetSteps = 0;
        int value = 0;
        // Only populated/read when copied from a Parameter lane (float
        // 0.0-1.0, see ParameterAutomationPoint's declaration) -- a
        // separate field rather than repurposing `value` above so Sustain/
        // PitchBend/FilterCutoff's existing int-valued copy/paste is
        // untouched.
        float floatValue = 0.0f;
        AutomationCurveType curveType = AutomationCurveType::Curve;
        float curveAmount = 0.0f;
    };
    std::vector<CopiedAutomationPoint> automationClipboard;

    // '4' -- when true, quantizeSelectedNotes() snaps to the TRIPLET width
    // of whatever grid ('1'/'2'/'3') was pressed (gridSteps * 2/3, exact
    // since the base grid is 12 steps/quarter -- see quantizeSelectedNotes()).
    bool quantizeTripletMode = false;

    // Cmd+U (cycleQuantizeAmount()) -- how far quantizeSelectedNotes() pulls
    // a note toward the grid line, as a percentage of the full distance from
    // its raw (as-played) position to the fully-snapped position. 100 =
    // snaps exactly onto the grid (the only behavior this app had before
    // this feature). Lower values keep some of the original human timing
    // ("クオンタイズ量の調整もできるようにしたい。25%, 50%, 75%, 100%の
    // 4段階"). Only ever one of {25, 50, 75, 100}.
    int quantizeAmountPercent = 100;

    // The gridSteps a manual '1'/'2'/'3' quantize was last invoked with
    // (BEFORE quantizeTripletMode's own adjustment, so toggling triplet
    // mode later still applies to whatever grid this remembers) -- reused
    // by autoQuantizeOnRecordEnabled below so auto-quantize snaps to
    // whatever grid was last manually chosen, without needing its own
    // separate grid-selection UI. Defaults to a sixteenth note
    // (stepsPerQuarterNote / 4, using the fixed 960-steps-per-quarter
    // convention every duration preset in this file already assumes).
    int lastQuantizeGridSteps = 240;

    // Cmd+Shift+U -- when true, every note a Real-time REC gesture commits
    // is immediately run through quantizeSelectedNotesImpl(lastQuantizeGridSteps)
    // (same amount%/triplet settings as a manual quantize), so raw human
    // timing snaps to the grid automatically instead of needing a separate
    // '1'/'2'/'3' press after every take ("Recording時に自動でクオンタイズ
    // かけたいON/OFFしたい"). Off by default -- real-time REC's whole
    // appeal is capturing human timing in the first place, this is an
    // opt-in convenience for players who'd rather have it snapped
    // immediately. Manual/Step-auto REC are unaffected (their notes are
    // usually already grid-aligned by construction).
    bool autoQuantizeOnRecordEnabled = false;

    // '1'/'2'/'4' select this and turn noteRepeatEnabled on -- see
    // setNoteRepeatRate()'s declaration. Same 960-steps-per-quarter grid
    // convention as commitDurationPresets/lastQuantizeGridSteps: 960 =
    // quarter, 480 = eighth, 240 = sixteenth.
    int noteRepeatGridSteps = 960;
    // '5' toggles this -- triplet width for whatever noteRepeatGridSteps
    // currently is, same gridSteps*2/3 convention quantizeTripletMode uses.
    bool noteRepeatTripletMode = false;
    bool noteRepeatEnabled = false;
    // 0.0 = no repeat gesture currently timed (reset whenever heldMidiNotes
    // goes empty, or noteRepeatEnabled turns off) -- updateNoteRepeat()
    // sets this to "now" on the tick a hold starts (so the FIRST tick after
    // pressing just starts the clock rather than firing immediately), then
    // advances it by exactly one interval every time it actually fires
    // (scheduled relative to when the last one was DUE, not to "now", so
    // ~30Hz timer jitter can't accumulate into audible drift over a long
    // hold). Only actually used while playbackEngine.isPlaying() is false
    // -- see lastNoteRepeatStepBucket below for the playing case, which is
    // grid-locked instead of wall-clock-scheduled ("NoteRepeatはテンポと
    // 同期する").
    double noteRepeatNextTriggerMs = 0.0;

    // -1 = no repeat gesture currently tracked (same reset points as
    // noteRepeatNextTriggerMs above). While playbackEngine.isPlaying(),
    // updateNoteRepeat() re-fires whenever getTrackPlaybackStep() crosses
    // into a new multiple of the current interval (currentStep /
    // intervalSteps) instead of scheduling by wall-clock time from
    // whenever the key happened to be pressed -- locks every repeat to the
    // transport's own sample-accurate step grid, so it stays exactly in
    // sync with the song's actual tempo/beat position (and any live tempo
    // change) rather than an independently-phased, merely tempo-RATED
    // timer.
    int lastNoteRepeatStepBucket = -1;

    // Cmd+Ctrl+5 / Cmd+Ctrl+R -- see setRangeSelectionStart()/setRangeSelectionEnd()/
    // duplicateSelectedRange()'s declarations. rangeSelectionEnd <=
    // rangeSelectionStart means unset, same convention as the loop markers.
    int rangeSelectionStart = 0;
    int rangeSelectionEnd = 0;

    // Raw key code of whatever was last dispatched through keyPressed()'s
    // trigger() lambda -- read by KeyboardOverlayComponent (via a getter
    // passed into toggleKeyboardOverlay()) to highlight that key. 0 = none
    // yet.
    int lastPressedKeyCode = 0;

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
    // matters. Keyed by the raw (unshifted) note number from the MIDI/PC
    // keyboard.
    struct ActiveLiveNote { int trackIndex; int shiftedPitch; };
    std::map<int, ActiveLiveNote> activeLiveNotes;

    // Piano-roll scale tint + chord-degree-name key context. Cmd+M
    // (cycleScale()) toggles Auto/Off -- Off means no tint (all rows
    // treated as "in scale" so nothing is drawn differently) and no degree
    // names; Auto means scaleRootPitchClass/scaleIsMinor are kept synced
    // to KeyEstimator's whole-piece estimate (see updateStepGridScale()),
    // rather than a fixed, manually-chosen key -- there's no longer a
    // manual major/minor override ("キーの推定もして").
    enum class ScaleType { Auto, Off };
    ScaleType currentScaleType = ScaleType::Auto;
    int scaleRootPitchClass = 0; // 0 = C -- auto-updated, see above
    bool scaleIsMinor = false; // auto-updated, see above

    // Empty = nothing heard yet. Otherwise the last note(s) held on the
    // MIDI/PC keyboard (a chord). Deliberately NOT cleared just because
    // nothing's sounding right now (only when a genuinely new chord
    // arrives), so plain 'f' (handleForwardKey) can commit it on its own
    // schedule instead of needing to land inside the exact instant
    // something is still sounding.
    std::vector<StepNote> pendingChord;

    // Wall-clock timestamp (ms, juce::Time::getMillisecondCounterHiRes())
    // of when pendingChord last became idle -- every key released
    // (heldMidiNotes empty) and nothing changed since. 0 = not currently
    // idle (a gesture is in progress, or pendingChord is empty). Set in
    // handleMidiNoteChange(); read by timerCallback(), which auto-clears
    // pendingChord ~pendingChordTimeoutMs after this, fading the preview's
    // color out over the final ~pendingChordFadeMs (StepGridComponent::
    // setPreviewAlpha()) so a forgotten chord can't later be written in by
    // an unrelated commit action without the user ever noticing it was
    // still sitting there ("MIDI入力、PC入力で保持したNoteは不意な操作で
    // 変な音が入らないように3秒程度で消える...0.2秒程度のアニメーションで
    // じんわりとNoteを示す色が消えて"). Fade duration bumped from the
    // original 200ms -- reported as fading out a bit too fast
    // ("あにめーしょんがちょっと早すぎる、もう少しだけゆっくり").
    double pendingChordIdleSinceMs = 0.0;
    // Shortened from the original 3000ms -- "もう少し入力Noteの維持時間を短く".
    static constexpr double pendingChordTimeoutMs = 2000.0;
    static constexpr double pendingChordFadeMs = 400.0;

    // Currently physically held MIDI keys -- only used to know when a
    // fresh chord capture should start (see handleMidiNoteChange()); NOT
    // mirrored live into pendingChord, since that would erode a chord back
    // down to whatever's still held as each key is released.
    std::vector<StepNote> heldMidiNotes;

    // Per-pitch note-on wall-clock timestamp (juce::Time::getMillisecondCounterHiRes(),
    // in ms) for whatever's currently held. handleMidiNoteChange() sets this
    // on every note-on (including a retrigger, so it always measures "how
    // long THIS press lasted") and, on note-off, uses it to compute that
    // pitch's own StepNote::durationSteps on the matching pendingChord entry
    // before erasing the timestamp -- but ONLY while realtimeRecordStep >= 0
    // (an in-progress Real-time REC gesture). Manual/Step-auto REC always
    // commit the whole chord at the current commit-duration preset
    // uniformly, so this timestamp is tracked (and always erased on note-
    // off) but its measured duration goes unused outside Real-time REC --
    // briefly made unconditional to fix two different pitches struck
    // together but released at different times sharing one flat duration
    // in Real-time REC ("ピッチの違うノート同士でなぜかDurationが
    // 引っ張られてしまう"), but that also made Step REC's own notes start
    // carrying real key-hold timing instead of the duration preset
    // ("ステップRecの時のDurationがなぜかリアルタイムになっている。指定
    // した音価にしたい") -- Step REC has no real timing to preserve in the
    // first place, so this only ever needed to apply during Real-time REC.
    std::map<int, double> heldNoteOnTimestamps;

    // 'r' cycles Off -> Manual -> Auto -> Realtime -> Off:
    //   Off      -- Browse: pure navigation/selection/pitch-nudge/delete,
    //               'f'/Ctrl+V never commit at all.
    //   Manual   -- Step REC (confirm): Ctrl+V commits pendingChord when
    //               you press it, nothing commits on its own.
    //   Auto     -- Step REC (auto): commits the instant a gesture
    //               completes (all keys released), regardless of whether
    //               the transport happens to be playing -- Ctrl+V still
    //               also works as a manual override.
    //   Realtime -- a dedicated mode for real-time recording, deliberately
    //               NOT derived from Auto+isPlaying() anymore: while
    //               STOPPED it behaves exactly like Off (pure preview, no
    //               commits at all, not even via Ctrl+V) so you can check
    //               sounds before recording without accidentally writing
    //               Step input; starting playback (Space) is the one
    //               button that begins actual real-time capture, which
    //               then commits to the step each gesture STARTED at (not
    //               wherever the edit cursor happens to be) -- see
    //               realtimeRecordStep below ("Realtime Recはやっぱり別
    //               モードで切り出したい。音を確認しながら、さて、録音、
    //               という形でボタンひとつで始まるようにしたい。Rec前に
    //               触るとStep入力されるのは良くない").
    enum class RecMode { Off, Manual, Auto, Realtime };
    RecMode recMode = RecMode::Off;

    // Realtime mode only: the track's playback step captured the instant
    // the current gesture STARTED (heldMidiNotes going empty -> non-empty)
    // while recMode == Realtime && playbackEngine.isPlaying(). -1 = no
    // real-time gesture in progress (a gesture that started while stopped,
    // or with recMode != Realtime, never sets this -- it falls through to
    // Step-REC/Manual/preview-only instead, per recMode's declaration).
    // Consumed and reset to -1 the moment the gesture completes.
    int realtimeRecordStep = -1;

    // Real-time REC only: each pitch's own playhead step position at the
    // moment ITS note-on happened, captured every note-on during the
    // current gesture (not just when the gesture started). Committing at
    // gesture-end groups pendingChord by this instead of writing the whole
    // chord uniformly at realtimeRecordStep, so a note played partway
    // through a still-held chord lands at the step it was actually played
    // at, instead of being pulled back to align with the first note
    // ("後から入れたNoteの頭が、最初のNoteに合ってしまう...途中から入力
    // したものは途中のタイミングがNoteオンになるべき"). Cleared alongside
    // pendingChord at the start of each new gesture. A pitch missing from
    // here (shouldn't normally happen) falls back to realtimeRecordStep.
    std::map<int, int> realtimeNoteOnsetSteps;

    // Duration presets in base grid steps (clip resolution is 960 steps per
    // quarter note -- see MidiClip::stepsPerQuarterNote -- specifically so
    // an eighth-note triplet is representable as an exact integer step
    // count). Ordered by actual musical duration (not step count) so
    // cycleCommitDuration's finer/coarser direction stays musically
    // monotonic: 240=16th, 320=eighth-triplet, 480=8th, 960=quarter.
    static constexpr int commitDurationPresets[4] = { 240, 320, 480, 960 };
    int commitDurationPresetIndex = 2; // default to 1/8 (index 2 in this array)

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
