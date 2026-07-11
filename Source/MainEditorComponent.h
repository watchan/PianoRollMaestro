#pragma once
#include <JuceHeader.h>
#include <map>
#include "AudioMidiSettingsWindow.h"
#include "HumInputListener.h"
#include "InstrumentPanelWindow.h"
#include "MicLevelMeterComponent.h"
#include "MidiInputRouter.h"
#include "PlaybackEngine.h"
#include "PluginEditorWindow.h"
#include "PluginHost.h"
#include "ProjectModel.h"
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
    void stepChordCaptured(const std::vector<StepNote>& notes);
    void liveNote(int noteNumber, float velocity, bool isOn);
    void togglePlayback();
    void openInstrumentPanel();
    void openAudioMidiSettings();

    // Editing commands, all reachable with hands on the keyboard home row.
    void ensureStepExists(int trackIndex, int stepIndex);
    void moveCursor(int deltaSteps);
    // d/f's actual handler: jumps to the previous/next note's start if the
    // cursor is currently on/within a note (fast browsing of existing
    // content), otherwise falls back to moveCursor(direction) for precise
    // placement on a rest. direction is -1 or +1.
    void moveCursorByNoteOrStep(int direction);
    void switchTrack(int deltaTracks);
    void insertRestAndAdvance();
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
    void toggleInputMode();
    void addTrack();

    // Step-grid VIEW controls -- pan/zoom, doesn't touch note data. The
    // grid's pitch window used to be hard-limited to a fixed 48-84 (C3-C6)
    // range with no way to scroll past it. '1'/'2' pan the visible pitch
    // range down/up; Cmd+Z/Cmd+X zoom out/in on both axes together.
    void scrollStepGridPitch(int deltaSemitones);
    void zoomStepGrid(float factor);

    // Hum input: 'v' toggles the mic listener on/off (press once to enter
    // hum-listening mode, again to leave it) so it's never a three-finger
    // hold-and-press gesture. Shift+Z/Shift+X (paired with the octave-shift
    // Z/X) cycle a persistent note-duration preset; Shift+F (paired with the
    // step-right F) commits the last note heard from the hum monitor -- not
    // necessarily still sounding, see currentHumNote below -- into the step
    // grid at that duration and advances the cursor by it. Can be pressed
    // repeatedly after humming stops to commit the same note again.
    void toggleHumInput();
    void cycleHumDuration(int delta);
    void commitHumNote();

    // Persistence -- the only commands allowed to touch a mouse dialog.
    void saveProject();
    void saveProjectAs();
    void openProject();
    void newProject();
    void writeProjectToFile(const juce::File& file);
    void syncProjectInstrumentState();
    void restoreInstrumentsFromProject();

    void refreshChildViews();

    // Updates the HUM status text and the step-grid preview outline from
    // currentHumNote -- called from refreshChildViews() AND directly from
    // humInputListener.onNoteChange so the preview updates live as pitch is
    // detected, not just whenever refreshChildViews() happens to run for
    // some unrelated reason.
    void updateHumDisplays();

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
    std::unique_ptr<PluginEditorWindow> pluginEditorWindow;
    std::unique_ptr<AudioMidiSettingsWindow> audioMidiSettingsWindow;

    TransportBarComponent transportBar;
    TrackListComponent trackList;
    StepGridComponent stepGrid;
    ShortcutHelpBarComponent shortcutHelpBar;

    Project project;
    juce::File currentProjectFile;
    std::unique_ptr<juce::FileChooser> fileChooser;

    int cursorTrackIndex = 0;
    int cursorStepIndex = 0;
    int octaveShiftOctaves = 0;

    // Tracks what liveNote(isOn=true) actually turned on (track + shifted
    // pitch) per raw note number, so the matching note-off targets the same
    // pitch/track even if octaveShiftOctaves or cursorTrackIndex changed
    // while the note was still sounding -- see liveNote() for why this
    // matters. Keyed by the raw (unshifted) note number from the source
    // (hum or real MIDI keyboard).
    struct ActiveLiveNote { int trackIndex; int shiftedPitch; };
    std::map<int, ActiveLiveNote> activeLiveNotes;

    // -1 = nothing heard yet this hum-listening session. Otherwise the last
    // note detected -- deliberately NOT cleared just because humming stopped
    // (only when a new pitch arrives, or hum listening is toggled off), so
    // Shift+F can commit it on its own schedule instead of needing to land
    // inside the exact instant a note is still sounding.
    int currentHumNote = -1;

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
