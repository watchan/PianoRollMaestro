#include "MainEditorComponent.h"
#include "ChordEstimator.h"
#include "VirtualKeyboardMaps.h"
#include <algorithm>

// Defined further below; forward-declared here so earlier methods (e.g.
// moveCursorByNoteOrStep) can use them too.
static int noteTotalLengthInSteps(const std::vector<Step>& steps, int ownerIndex);
static int findOwningNoteStepIndex(const std::vector<Step>& steps, int stepIndex);

namespace
{
    // Generic two-closure UndoableAction so each undo entry can just say
    // "apply this state" / "apply that state" without a bespoke subclass
    // per editing command.
    class LambdaUndoableAction : public juce::UndoableAction
    {
    public:
        LambdaUndoableAction(std::function<void()> redoIn, std::function<void()> undoIn)
            : redoAction(std::move(redoIn)), undoAction(std::move(undoIn)) {}

        bool perform() override { redoAction(); return true; }
        bool undo() override { undoAction(); return true; }

    private:
        std::function<void()> redoAction, undoAction;
    };

    // Stable parameter identity used to key a MidiClip::ParameterAutomationLane
    // -- shared by recordParameterAutomationPoint() (Realtime capture) and
    // previewTouchedParameterValue() (Manual-mode preview), so a lane
    // touched once in either state is found/reused by the other.
    // HostedAudioProcessorParameter::getParameterID() survives a plugin's
    // own parameter list being reordered, unlike a raw index; falls back to
    // the raw index for a parameter type that doesn't implement it (stable
    // enough within one still-loaded instance, even if not across plugin
    // versions).
    juce::String stableParameterID(juce::AudioProcessorParameter& parameter)
    {
        if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*>(&parameter))
            return hosted->getParameterID();
        return juce::String(parameter.getParameterIndex());
    }
}

// RAII undo/redo scope for note-editing commands -- see the declaration in
// MainEditorComponent.h for the full rationale. Nested class, so it has
// access to MainEditorComponent's private members without a friend
// declaration.
class MainEditorComponent::StepEditGuard
{
public:
    explicit StepEditGuard(MainEditorComponent& ownerIn)
        : owner(ownerIn), trackIndex(ownerIn.cursorTrackIndex),
          before(ownerIn.project.tracks[(size_t) trackIndex].clip.steps),
          sustainBefore(ownerIn.project.tracks[(size_t) trackIndex].clip.sustainPedalEvents),
          pitchBendBefore(ownerIn.project.tracks[(size_t) trackIndex].clip.pitchBendPoints),
          filterCutoffBefore(ownerIn.project.tracks[(size_t) trackIndex].clip.filterCutoffPoints),
          cursorBefore(ownerIn.cursorStepIndex)
    {
    }

    ~StepEditGuard()
    {
        auto& clipNow = owner.project.tracks[(size_t) trackIndex].clip;
        if (clipNow.steps == before && clipNow.sustainPedalEvents == sustainBefore
            && clipNow.pitchBendPoints == pitchBendBefore && clipNow.filterCutoffPoints == filterCutoffBefore)
            return; // pure navigation, nothing to undo

        auto after = clipNow.steps;
        auto sustainAfter = clipNow.sustainPedalEvents;
        auto pitchBendAfter = clipNow.pitchBendPoints;
        auto filterCutoffAfter = clipNow.filterCutoffPoints;
        auto cursorAfter = owner.cursorStepIndex;
        auto* ownerPtr = &owner;
        auto trackIndexCopy = trackIndex;
        auto beforeCopy = before;
        auto sustainBeforeCopy = sustainBefore;
        auto pitchBendBeforeCopy = pitchBendBefore;
        auto filterCutoffBeforeCopy = filterCutoffBefore;
        auto cursorBeforeCopy = cursorBefore;

        // Without this, UndoManager merges every perform() into whatever
        // transaction is already open -- it only auto-starts a fresh one
        // for the very first perform() call ever made (newTransaction
        // defaults to true, but perform() clears it and never sets it back
        // on its own). Left out, undo() would revert the ENTIRE session's
        // edits in one shot instead of one note-edit command at a time.
        owner.undoManager.beginNewTransaction();
        owner.undoManager.perform(new LambdaUndoableAction(
            [ownerPtr, trackIndexCopy, after, sustainAfter, pitchBendAfter, filterCutoffAfter, cursorAfter]
            { ownerPtr->applyStepEdit(trackIndexCopy, after, sustainAfter, pitchBendAfter, filterCutoffAfter, cursorAfter); },
            [ownerPtr, trackIndexCopy, beforeCopy, sustainBeforeCopy, pitchBendBeforeCopy, filterCutoffBeforeCopy, cursorBeforeCopy]
            { ownerPtr->applyStepEdit(trackIndexCopy, beforeCopy, sustainBeforeCopy, pitchBendBeforeCopy, filterCutoffBeforeCopy, cursorBeforeCopy); }));
    }

private:
    MainEditorComponent& owner;
    int trackIndex;
    std::vector<Step> before;
    std::vector<SustainPedalEvent> sustainBefore;
    std::vector<AutomationPoint> pitchBendBefore;
    std::vector<AutomationPoint> filterCutoffBefore;
    int cursorBefore;
};

void MainEditorComponent::applyStepEdit(int trackIndex, const std::vector<Step>& steps,
                                          const std::vector<SustainPedalEvent>& sustainPedalEvents,
                                          const std::vector<AutomationPoint>& pitchBendPoints,
                                          const std::vector<AutomationPoint>& filterCutoffPoints,
                                          int cursorStep)
{
    auto& clip = project.tracks[(size_t) trackIndex].clip;

    // If the target state is already exactly what's live -- the common
    // case: StepEditGuard's destructor snapshots `after` as a copy of
    // project.tracks[trackIndex].clip's tracked fields AFTER the caller
    // already mutated them in place, then immediately hands that same
    // content back here via undoManager.perform()'s initial redo -- there's
    // nothing to actually reassign, so skip the stop()/restart dance below
    // entirely. That dance exists ONLY to guard the audio thread against a
    // genuine vector reallocation; re-assigning IDENTICAL content isn't
    // one, and doing it anyway was needlessly cutting off whatever else was
    // still sounding on every single commit made during playback -- not
    // just this specific edit, but the underlying cause behind that class
    // of bug generally. Genuine undo()/redo() NAVIGATION still
    // takes the dance below normally, since those really do swap in
    // different content than what's currently live.
    if (clip.steps == steps && clip.sustainPedalEvents == sustainPedalEvents
        && clip.pitchBendPoints == pitchBendPoints && clip.filterCutoffPoints == filterCutoffPoints)
    {
        cursorTrackIndex = trackIndex;
        cursorStepIndex = cursorStep;
        refreshChildViews();
        return;
    }

    // Reassigning any of these while the audio thread might be
    // concurrently iterating them (scheduleUpTo(), which reads clip.steps
    // AND clip.sustainPedalEvents/pitchBendPoints/filterCutoffPoints, only
    // runs while playbackEngine.isPlaying()) is a genuine data race --
    // stop() first to avoid it, same reasoning as addTrack()/newProject()/
    // openProject(). But this runs on EVERY commit (StepEditGuard pushes an
    // undo entry -> undoManager.perform() -> here, for every actual edit),
    // so calling stop() unconditionally also sent All-Notes-Off/All-Sound-
    // Off to every track's synth on every single 'f' press -- including
    // whatever's still physically held on the MIDI keyboard's live
    // monitor, cutting it off mid-note. Skipped entirely while not
    // playing: there's no concurrent audio-thread access to guard against
    // then (the overwhelmingly common case -- editing with the transport
    // stopped), so there's nothing to protect and no reason to kill the
    // live monitor.
    //
    // Real-time recording (mode 3, see handleMidiNoteChange()) commits
    // DURING playback, so this now has to resume afterwards instead of
    // just leaving the transport stopped -- captured as a rounded step
    // position (not the exact sample) since PlaybackEngine::start() only
    // understands resuming at a step boundary anyway. This causes a brief
    // all-notes-off/restart click on every commit made while playing --
    // an accepted trade-off of the stop-to-mutate-safely approach, same
    // one Shift+Space's "jump every track to one step" already established.
    auto wasPlaying = playbackEngine.isPlaying();
    auto resumeStep = 0;

    if (wasPlaying)
    {
        auto stepSeconds = clip.stepDurationSeconds(project.tempoBpm);
        if (stepSeconds > 0.0)
        {
            auto positionSeconds = (double) playbackEngine.getPlaybackPositionSamples() / playbackSampleRate;
            resumeStep = juce::jmax(0, (int) std::round(positionSeconds / stepSeconds));
        }
        playbackEngine.stop();
    }

    clip.steps = steps;
    clip.sustainPedalEvents = sustainPedalEvents;
    clip.pitchBendPoints = pitchBendPoints;
    clip.filterCutoffPoints = filterCutoffPoints;
    cursorTrackIndex = trackIndex;
    cursorStepIndex = cursorStep;

    if (wasPlaying)
        playbackEngine.start(resumeStep);

    refreshChildViews();
}

void MainEditorComponent::performUndo()
{
    if (undoManager.canUndo())
        undoManager.undo();
}

void MainEditorComponent::performRedo()
{
    if (undoManager.canRedo())
        undoManager.redo();
}

MainEditorComponent::MainEditorComponent()
{
    project.tracks.reserve(reservedTrackCapacity); // see its declaration -- lets addTrack() push_back() safely during playback
    project.tracks.push_back(Track{});
    playbackEngine.setProject(&project);

    addAndMakeVisible(transportBar);
    addAndMakeVisible(trackList);
    chordEstimateBar.attachToStepGrid(stepGrid);
    addAndMakeVisible(chordEstimateBar);
    addAndMakeVisible(stepGrid);
    addChildComponent(sessionGrid); // starts hidden -- resized()/toggleViewMode() control visibility
    addAndMakeVisible(shortcutHelpBar);

    addAndMakeVisible(midiDeviceBox);
    midiDeviceBox.onChange = [this] { midiDeviceSelected(); };
    refreshMidiDeviceList();
    restoreSavedMidiInputDevice();

    addAndMakeVisible(playButton);
    playButton.onClick = [this] { togglePlayback(); grabKeyboardFocus(); };

    addAndMakeVisible(instrumentButton);
    instrumentButton.onClick = [this] { openInstrumentPanel(); };

    addAndMakeVisible(audioSettingsButton);
    audioSettingsButton.onClick = [this] { openAudioMidiSettings(); };

    // MIDI keyboard (and PC keyboard, via midiInputRouter.injectNote()) is a
    // live monitor that feeds pendingChord -- it can hold a chord, see
    // handleMidiNoteChange(). Manual commit is the explicit action in
    // handleForwardKey() (plain 'f'); recMode ('r') layers an automatic
    // commit on top in its Auto state, see handleMidiNoteChange().
    midiInputRouter.onLiveNote = [this](int noteNumber, float velocity, bool isOn)
    {
        handleMidiNoteChange(noteNumber, velocity, isOn);
    };
    midiInputRouter.onLiveControllerMessage = [this](const juce::MidiMessage& message)
    {
        if (message.isController() && message.getControllerNumber() == 64)
        {
            // Threshold is 0 vs anything else, NOT the MIDI-spec-typical
            // 64 -- see lastRawSustainDown's declaration for the full
            // reasoning (this was proven correct earlier and is unchanged).
            // Deliberately does NOT forward every raw CC64 value to
            // playbackEngine.liveMidiMessage() below the way every OTHER
            // controller message does -- a real pedal's jittery in-between
            // dips were reaching the loaded synth/plugin as genuine, if
            // momentary, CC64=0 "pedal up" messages, and at least one
            // instrument responded by releasing notes that were still
            // being genuinely held (finger still down, never actually
            // note-off'd) rather than only the ones it was sustaining over.
            // Only resolvePendingSustainCrossing() actually forwards to the
            // synth or records, once the settle window confirms the value.
            auto rawDown = message.getControllerValue() > 0;
            auto now = juce::Time::getMillisecondCounterHiRes();
            // A new crossing arriving late enough proves the previous
            // pending crossing's settle window has already elapsed --
            // resolve it (using whatever lastRawSustainDown still is, i.e.
            // BEFORE this new message updates it) before starting the next
            // one, rather than waiting for timerCallback()'s coarser poll.
            if (pendingSustainCrossingMs >= 0.0 && now - pendingSustainCrossingMs >= sustainSettleMs)
                resolvePendingSustainCrossing();
            if (rawDown != lastRawSustainDown)
            {
                lastRawSustainDown = rawDown;
                pendingSustainCrossingMs = now;
            }
        }
        // Real pitch wheel / mod wheel (CC74) hardware capture -- see
        // AutomationLane's declaration. Audio-side live preview is always
        // forwarded regardless of REC state (matches every other branch
        // here); recordAutomationPoint() itself is what gates whether
        // anything actually gets written to the clip.
        else if (message.isPitchWheel())
        {
            playbackEngine.liveMidiMessage(cursorTrackIndex, message);
            recordAutomationPoint(AutomationLane::PitchBend, message.getPitchWheelValue());
        }
        else if (message.isController() && message.getControllerNumber() == 74)
        {
            playbackEngine.liveMidiMessage(cursorTrackIndex, message);
            recordAutomationPoint(AutomationLane::FilterCutoff, message.getControllerValue());
        }
        else
        {
            playbackEngine.liveMidiMessage(cursorTrackIndex, message);
        }
    };

    setWantsKeyboardFocus(true);

    // Wide enough for every transport-bar badge (REC/KEY/LOOP/METRONOME/
    // AUTO-Q/REPEAT/SUSTAIN/NOTE/AUTO) plus its trailing status text
    // (BPM/OCT/VEL/DUR/QUANT/COUNT-IN, plus a pending-note name) to
    // actually fit on screen at once -- 900 stopped being wide enough
    // several badges ago; TransportBarComponent::paint() lays them out
    // left-to-right with juce::Rectangle::removeFromLeft(), which just
    // silently clips to zero width once the row runs out of room rather
    // than erroring, so a too-narrow window doesn't fail loudly, it just
    // makes the newest/rightmost badges invisible.
    setSize(1700, 700);

    // Output+MIDI manager (inherited deviceManager): output-only, no audio
    // input channels requested at all. Restore a previously-saved setup if
    // one exists so a manual device fix survives past this launch instead of
    // JUCE re-running automatic default-device selection from scratch every
    // time.
    std::unique_ptr<juce::XmlElement> savedOutputState;
    if (auto settingsFile = getOutputAudioSettingsFile(); settingsFile.existsAsFile())
        savedOutputState = juce::XmlDocument::parse(settingsFile);

    setAudioChannels(0, 2, savedOutputState.get());
    deviceManager.addChangeListener(this);

    startTimerHz(30);

    refreshChildViews();
}

MainEditorComponent::~MainEditorComponent()
{
    deviceManager.removeChangeListener(this);
    shutdownAudio();
}

juce::File MainEditorComponent::getOutputAudioSettingsFile()
{
    // Same "~/Library/Application Support/PianoRollMaestro/" convention
    // PluginHost.cpp uses for KnownPlugins.xml -- userApplicationDataDirectory
    // resolves to ~/Library itself on macOS, not .../Application Support.
    auto appDataDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                           .getChildFile("Application Support")
                           .getChildFile("PianoRollMaestro");
    appDataDir.createDirectory();
    return appDataDir.getChildFile("AudioDeviceState.xml");
}

void MainEditorComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &deviceManager)
    {
        if (auto xml = deviceManager.createStateXml())
            xml->writeTo(getOutputAudioSettingsFile());
    }
}

void MainEditorComponent::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    playbackEngine.prepare(sampleRate, samplesPerBlockExpected);
    playbackSampleRate = sampleRate;

    deviceSwitchMuteSamplesRemaining = (int64_t) (sampleRate * 0.3); // ~300ms hard mute
    startupFadeInTotalSamples = juce::jmax((int64_t) 1, (int64_t) (sampleRate * 0.15)); // ~150ms ramp after that
    startupFadeInSamplesRemaining = startupFadeInTotalSamples;
}

void MainEditorComponent::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    bufferToFill.clearActiveBufferRegion();

    juce::AudioBuffer<float> subBuffer(bufferToFill.buffer->getArrayOfWritePointers(),
                                        bufferToFill.buffer->getNumChannels(),
                                        bufferToFill.startSample,
                                        bufferToFill.numSamples);

    juce::MidiBuffer scratchMidi;
    playbackEngine.renderNextBlock(subBuffer, scratchMidi, bufferToFill.numSamples);

    for (int i = 0; i < bufferToFill.numSamples; ++i)
    {
        float gain;

        if (deviceSwitchMuteSamplesRemaining > 0)
        {
            gain = 0.0f;
            --deviceSwitchMuteSamplesRemaining;
        }
        else if (startupFadeInSamplesRemaining > 0)
        {
            gain = 1.0f - (float) startupFadeInSamplesRemaining / (float) startupFadeInTotalSamples;
            --startupFadeInSamplesRemaining;
        }
        else
        {
            break; // nothing left to attenuate; rest of the block plays at full gain
        }

        for (int ch = 0; ch < subBuffer.getNumChannels(); ++ch)
            subBuffer.setSample(ch, i, subBuffer.getSample(ch, i) * gain);
    }
}

void MainEditorComponent::releaseResources()
{
}

void MainEditorComponent::timerCallback()
{
    pollVirtualKeyboardInput();
    updateNoteRepeat();

    // Tail case for lastRawSustainDown's settle-window debounce: resolves
    // an isolated final crossing (press or release) when nothing else ever
    // arrives to trigger the synchronous check in onLiveControllerMessage.
    if (pendingSustainCrossingMs >= 0.0
        && juce::Time::getMillisecondCounterHiRes() - pendingSustainCrossingMs >= sustainSettleMs)
        resolvePendingSustainCrossing();

    // stepGrid's pan/zoom can change via calls that don't go through
    // refreshChildViews() (zoomStepGridHorizontal() etc. call straight into
    // stepGrid) -- repainting every tick keeps chordEstimateBar's bar labels
    // aligned to stepGrid's current view without hooking every such call site.
    chordEstimateBar.repaint();

    // The counting-in -> playing transition (PlaybackEngine::startWithCountIn())
    // happens on the audio thread, asynchronously from any UI action -- poll
    // it here so the transport bar notices and updates without needing an
    // explicit push from wherever triggered it.
    transportBar.setCountingIn(playbackEngine.isCountingIn());
    transportBar.setPlaying(playbackEngine.isPlaying());
    // Debounced state (see pendingSustainCrossingMs's declaration) -- pushed
    // every tick, same as playing/countingIn above, so the badge visibly
    // tracks a real pedal live instead of only updating on the next edit --
    // there was otherwise no way to SEE whether a press/jitter was actually
    // being recognized, only to infer it after the fact from a note's length.
    transportBar.setSustainPedalDown(midiSustainPedalDown);

    // Auto-clear a forgotten pendingChord ~pendingChordTimeoutMs after it
    // went idle (see pendingChordIdleSinceMs's declaration), fading the
    // preview's color out over the final pendingChordFadeMs so the clear is
    // visible rather than an abrupt disappearance. Checked every tick
    // regardless of play state -- this has nothing to do with playback.
    if (pendingChordIdleSinceMs > 0.0 && !pendingChord.empty())
    {
        auto idleMs = juce::Time::getMillisecondCounterHiRes() - pendingChordIdleSinceMs;
        if (idleMs >= pendingChordTimeoutMs)
        {
            pendingChord.clear();
            pendingChordIdleSinceMs = 0.0;
            stepGrid.setPreviewAlpha(1.0f);
            updatePendingNoteDisplays();
        }
        else
        {
            auto fadeStartMs = pendingChordTimeoutMs - pendingChordFadeMs;
            auto alpha = idleMs <= fadeStartMs ? 1.0f
                : juce::jlimit(0.0f, 1.0f, (float) ((pendingChordTimeoutMs - idleMs) / pendingChordFadeMs));
            stepGrid.setPreviewAlpha(alpha);
        }
    }

    if (!playbackEngine.isPlaying())
    {
        stepGrid.setPlaybackStep(-1);
        stepGrid.setLiveRecordingPreview({});
        return;
    }

    // Per-track step position, NOT the global sample clock -- a launched
    // Session View clip loops on its own track cursor independently of the
    // transport's single global position (see getTrackPlaybackStep()'s
    // comment), so deriving the playhead from the global sample count made
    // it run straight past a looping clip's own boundary instead of
    // wrapping with it.
    auto trackPlaybackStep = playbackEngine.getTrackPlaybackStep(cursorTrackIndex);
    stepGrid.setPlaybackStep(trackPlaybackStep);

    // Real-time REC: while a chord is actively being held (realtimeRecordStep
    // set, see handleMidiNoteChange()), grow the preview every tick from
    // each pitch's OWN onset. A pitch still actually held (heldMidiNotes)
    // keeps extending live to right now; a pitch that's already been
    // released (but the whole gesture hasn't ended -- some OTHER pitch is
    // still held, see handleMidiNoteChange()'s mid-gesture flush) freezes
    // exactly at its own measured end instead of continuing to visually
    // stretch alongside whatever's still held.
    if (realtimeRecordStep >= 0)
    {
        std::vector<StepGridComponent::LiveRecordingPreviewNote> previewNotes;
        for (auto& n : pendingChord)
        {
            auto onsetIt = realtimeNoteOnsetSteps.find(n.pitch);
            auto onset = onsetIt != realtimeNoteOnsetSteps.end() ? onsetIt->second : realtimeRecordStep;

            auto isHeld = std::any_of(heldMidiNotes.begin(), heldMidiNotes.end(),
                [&](const StepNote& h) { return h.pitch == n.pitch; });

            auto endStep = isHeld ? trackPlaybackStep + 1 : onset + juce::jmax(1, n.durationSteps);
            previewNotes.push_back({ shiftedPendingPitch(n.pitch), onset, endStep });
        }
        stepGrid.setLiveRecordingPreview(previewNotes);
    }
    else
    {
        stepGrid.setLiveRecordingPreview({});
    }
}

void MainEditorComponent::pollVirtualKeyboardInput()
{
    // Enter (toggleDrumGridMode()) picks which map is live -- mutually
    // exclusive even though several physical keys are shared between the
    // two maps (see virtualDrumKeyMap()'s comment), so exactly one is ever
    // polled at a time.
    auto melodicActive = !drumGridModeActive;
    auto drumActive = drumGridModeActive;

    // Ctrl+S ("Sustain") -- 'S' is unmapped in both virtualKeyboardKeyMap()
    // and virtualDrumKeyMap(), so it's free to hold alongside actual note
    // keys without colliding. Melodic-only (sustain doesn't really apply to
    // one-shot drum hits). Still Ctrl-gated (unlike the note maps
    // themselves) since plain 'S' is already used elsewhere. Excludes
    // Cmd -- Cmd+Ctrl+S is horizontal zoom-in (see keyPressed()'s Ctrl
    // block), a discrete press with no hold semantics of its own, so it
    // shouldn't also arm sustain for as long as it's held (same reasoning
    // that originally moved this off Ctrl+F once THAT became a zoom key).
    auto currentMods = juce::ModifierKeys::getCurrentModifiers();
    auto sustainKeyDown = melodicActive && currentMods.isCtrlDown() && !currentMods.isCommandDown()
        && juce::KeyPress::isKeyCurrentlyDown('S');
    // Rising edge only (not held-down-every-tick), same reasoning as the
    // note-press highlight below -- otherwise F would dominate
    // lastPressedKeyCode for as long as it's held, drowning out whatever
    // note keys get pressed while sustaining.
    if (sustainKeyDown && !wasSustainKeyDown)
    {
        lastPressedKeyCode = 'F';
        // A clean digital keystate, unlike a real pedal's analog signal --
        // no debounce needed, applied immediately both for live audio (the
        // synth needs a genuine CC64 to actually hold notes over, same as
        // the real-pedal path) and for recording (recordSustainPedalEvent(),
        // see midiSustainPedalDown's declaration).
        playbackEngine.liveMidiMessage(cursorTrackIndex, juce::MidiMessage::controllerEvent(1, 64, 127));
        midiSustainPedalDown = true;
        recordSustainPedalEvent(true);
    }
    else if (!sustainKeyDown && wasSustainKeyDown)
    {
        playbackEngine.liveMidiMessage(cursorTrackIndex, juce::MidiMessage::controllerEvent(1, 64, 0));
        midiSustainPedalDown = false;
        recordSustainPedalEvent(false);
        flushSustainedLiveNotes();
    }
    wasSustainKeyDown = sustainKeyDown;

    // Shared diff-and-fire logic for one map: figures out which of its keys
    // are currently down (given whether this map is even "active" this
    // poll), fires injectNote() for anything newly pressed/released versus
    // last poll, and remembers each held key's ACTUAL sounded pitch (not
    // just the key) so a note-off always targets the same pitch its note-on
    // used, even if virtualKeyboardTransposeSemitones changes while it's
    // still held (same reasoning as liveNote()'s ActiveLiveNote tracking).
    // Doesn't need to know about sustain at all anymore -- that's now a
    // genuine CC64 message handled entirely above/by the synth itself, not
    // something that defers a note's own note-off.
    auto pollOneMap = [this](const std::map<char, int>& keyMap, bool active, int baseNote,
                              std::vector<std::pair<char, int>>& held)
    {
        std::vector<std::pair<char, int>> currentlyDown;
        if (active)
            for (auto& [ch, offset] : keyMap)
                if (juce::KeyPress::isKeyCurrentlyDown((int) ch))
                    currentlyDown.push_back({ ch, juce::jlimit(0, 127, baseNote + offset) });

        for (auto& [ch, pitch] : currentlyDown)
        {
            if (std::any_of(held.begin(), held.end(), [ch = ch](auto& h) { return h.first == ch; }))
                continue;

            midiInputRouter.injectNote(pitch, virtualKeyboardVelocity, true);
            // Rising edge of an actual note keypress -- keyPressed()'s
            // trigger() never sees these (see this method's own top-of-file
            // comment), so KeyboardOverlayComponent's "last pressed"
            // highlight would otherwise be stuck on whatever editing
            // command was pressed last, never reflecting note keys at all.
            lastPressedKeyCode = (int) ch;
        }

        for (auto& [ch, pitch] : held)
        {
            if (std::any_of(currentlyDown.begin(), currentlyDown.end(), [ch = ch](auto& c) { return c.first == ch; }))
                continue;

            midiInputRouter.injectNote(pitch, 0.0f, false);
        }

        held = std::move(currentlyDown);
    };

    pollOneMap(virtualKeyboardKeyMap(), melodicActive, 60 + virtualKeyboardTransposeSemitones, heldVirtualKeyboardKeys);
    pollOneMap(virtualDrumKeyMap(), drumActive, 48, heldVirtualDrumKeys);
}

void MainEditorComponent::toggleDrumGridMode()
{
    // Switching away from whichever map was active mid-hold would leave
    // its currently-held keys stuck sounding forever (pollOneMap would
    // stop seeing them as "active" and never get a chance to send their
    // note-off) -- force everything off first, the same reasoning
    // liveNote()/stop() etc. already apply elsewhere in this file.
    for (auto& [ch, pitch] : heldVirtualKeyboardKeys)
        midiInputRouter.injectNote(pitch, 0.0f, false);
    heldVirtualKeyboardKeys.clear();
    for (auto& [ch, pitch] : heldVirtualDrumKeys)
        midiInputRouter.injectNote(pitch, 0.0f, false);
    heldVirtualDrumKeys.clear();

    drumGridModeActive = !drumGridModeActive;
    refreshChildViews();
}

void MainEditorComponent::adjustVirtualKeyboardTranspose(int deltaSemitones)
{
    virtualKeyboardTransposeSemitones = juce::jlimit(-48, 48, virtualKeyboardTransposeSemitones + deltaSemitones);
}

void MainEditorComponent::adjustVirtualKeyboardVelocity(float delta)
{
    virtualKeyboardVelocity = juce::jlimit(0.0f, 1.0f, virtualKeyboardVelocity + delta);

    // Also nudge the velocity of whichever note(s) are currently selected
    // in the piano roll (same targeting adjustNotePitch()'s T/G uses --
    // effectiveSelectedNoteStarts()/effectiveSelectedPitches()), and play
    // the result back immediately so the change is audible. No-op beyond
    // the global preset above when there's nothing to target (Session
    // View, or no note under the cursor).
    if (currentViewMode == ViewMode::PianoRoll)
    {
        auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
        auto cursorOwnerIndex = findOwningNoteStepIndex(steps, cursorStepIndex);
        auto targetStarts = effectiveSelectedNoteStarts();

        if (!targetStarts.empty())
        {
            StepEditGuard undoGuard(*this);

            for (auto stepIndex : targetStarts)
            {
                if (stepIndex < 0 || stepIndex >= (int) steps.size())
                    continue;

                // Only the cursor's own chord honors the narrowed within-
                // chord pitch selection (T/G's effectiveSelectedPitches())
                // -- every other note in a multi-note selection has no such
                // narrowing concept, so all of its notes get nudged.
                auto& step = steps[(size_t) stepIndex];
                std::vector<int> selected = (stepIndex == cursorOwnerIndex)
                    ? effectiveSelectedPitches()
                    : std::vector<int>{};
                if (stepIndex != cursorOwnerIndex)
                    for (auto& note : step.notes)
                        selected.push_back(note.pitch);

                for (auto& note : step.notes)
                    if (std::find(selected.begin(), selected.end(), note.pitch) != selected.end())
                        note.velocity = juce::jlimit(0.0f, 1.0f, note.velocity + delta);
            }

            auditionNoteAtCursor();
        }
    }

    refreshChildViews();
}

void MainEditorComponent::cycleRecMode()
{
    recMode = recMode == RecMode::Off       ? RecMode::Manual
            : recMode == RecMode::Manual    ? RecMode::Auto
            : recMode == RecMode::Auto      ? RecMode::Realtime
                                             : RecMode::Off;
    refreshChildViews();
}

void MainEditorComponent::togglePlayback()
{
    if (playbackEngine.isPlaying())
    {
        // Before stopping -- see forceConfirmPendingSustainRelease()'s
        // declaration for why this can't just wait for the debounce timer.
        forceConfirmPendingSustainRelease();
        playbackEngine.stop();

        // Land the edit cursor where playback actually stopped, instead of
        // leaving it wherever it was before Tab started playing -- same
        // step-index math timerCallback() uses for the playhead locator.
        // stop() doesn't reset the playback position, so this still reads
        // the last position reached. Snapped to the nearest multiple of the
        // current commit-duration preset (Shift+Z/Shift+X, commitDurationPresets
        // [commitDurationPresetIndex]) rather than the raw base-step grid, so
        // the cursor always lands on a musically meaningful boundary for
        // whatever note value you're currently working in, not an arbitrary
        // fine-grained step.
        auto& clip = project.tracks[(size_t) cursorTrackIndex].clip;
        auto stepSeconds = clip.stepDurationSeconds(project.tempoBpm);
        if (stepSeconds > 0.0)
        {
            auto positionSeconds = (double) playbackEngine.getPlaybackPositionSamples() / playbackSampleRate;
            auto rawStep = (int) (positionSeconds / stepSeconds);
            auto duration = commitDurationPresets[commitDurationPresetIndex];
            cursorStepIndex = juce::jmax(0, ((rawStep + duration / 2) / duration) * duration);
            noteSelectionAnchorStep = -1; // see moveCursor()'s comment
        }
    }
    else if (playbackEngine.isCountingIn())
    {
        playbackEngine.stop(); // cancels the count-in before real playback ever started -- cursor untouched
    }
    else
    {
        // Real-time REC (recMode Realtime) gets a 4-beat count-in so
        // there's time to get ready before it actually starts capturing --
        // every other mode starts immediately, same as before. This is
        // also THE single button that begins actual recording once
        // Realtime mode is selected -- before this, Realtime mode is pure
        // preview (see RecMode::Realtime's declaration). Shift+W
        // (toggleCountIn()) can turn the count-in off project-wide.
        if (recMode == RecMode::Realtime && project.countInEnabled)
            playbackEngine.startWithCountIn(0, 4);
        else
            playbackEngine.start();
    }

    transportBar.setPlaying(playbackEngine.isPlaying());
    refreshChildViews();
}

void MainEditorComponent::playFromLocator()
{
    if (playbackEngine.isPlaying() || playbackEngine.isCountingIn())
        playbackEngine.stop();

    if (recMode == RecMode::Realtime && project.countInEnabled)
        playbackEngine.startWithCountIn(cursorStepIndex, 4);
    else
        playbackEngine.start(cursorStepIndex);

    transportBar.setPlaying(playbackEngine.isPlaying());
    refreshChildViews();
}

void MainEditorComponent::openInstrumentPanel()
{
    auto trackIndex = cursorTrackIndex;
    auto* currentInstrument = playbackEngine.getTrackInstrument(trackIndex);
    auto currentName = currentInstrument != nullptr ? currentInstrument->getName() : juce::String();

    instrumentPanelWindow = std::make_unique<InstrumentPanelWindow>(
        pluginHost,
        project.tracks[(size_t) trackIndex].name,
        currentName,
        [this, trackIndex](const juce::PluginDescription& description)
        {
            pluginHost.createInstrument(description, 44100.0, 512,
                [this, trackIndex](std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error)
                {
                    if (instance != nullptr)
                    {
                        // The old instance (if any) is about to be destroyed --
                        // any editor window still pointing at it must go first,
                        // and this listener must be off it before it disappears.
                        invalidatePluginEditorWindow(trackIndex);
                        unregisterParameterAutomationListener(trackIndex);
                        playbackEngine.setTrackInstrument(trackIndex, std::move(instance));
                        registerParameterAutomationListener(trackIndex);

                        // Show the freshly loaded plugin's editor right away.
                        if (auto* newInstrument = playbackEngine.getTrackInstrument(trackIndex))
                        {
                            pluginEditorWindowsByTrack[trackIndex] =
                                std::make_unique<PluginEditorWindow>(*newInstrument, newInstrument->getName(), [this] { grabKeyboardFocus(); });
                            pluginEditorDesiredVisible = true;
                            updatePluginEditorWindowVisibility();
                        }

                        refreshChildViews();
                    }
                    else
                    {
                        DBG("Failed to load instrument for track " << trackIndex << ": " << error);
                    }

                    instrumentPanelWindow = nullptr;
                });
        },
        [this, trackIndex]
        {
            if (auto* instrument = playbackEngine.getTrackInstrument(trackIndex))
            {
                auto& window = pluginEditorWindowsByTrack[trackIndex];
                if (window == nullptr)
                    window = std::make_unique<PluginEditorWindow>(*instrument, instrument->getName(), [this] { grabKeyboardFocus(); });
                pluginEditorDesiredVisible = true;
                updatePluginEditorWindowVisibility();
            }
        },
        [this, trackIndex]
        {
            invalidatePluginEditorWindow(trackIndex);
            unregisterParameterAutomationListener(trackIndex);
            playbackEngine.setTrackInstrument(trackIndex, nullptr);
            refreshChildViews();
            instrumentPanelWindow = nullptr;
        });
}

void MainEditorComponent::openAudioMidiSettings()
{
    audioMidiSettingsWindow = std::make_unique<AudioMidiSettingsWindow>(deviceManager);
}

void MainEditorComponent::toggleKeyboardOverlay()
{
    if (keyboardOverlayWindow == nullptr)
    {
        keyboardOverlayWindow = std::make_unique<KeyboardOverlayWindow>(
            [this] { return currentViewMode == ViewMode::Session; },
            [this] { return (int) recMode; },
            [this] { return virtualKeyboardTransposeSemitones; },
            [this]
            {
                // Union of currently-HELD note/drum keys (so a chord
                // highlights every one of its keys at once, for as long as
                // it's held) with the last plain editing-command keypress
                // (a discrete action, not something that's "held" the same
                // way -- see keyPressed()'s trigger()).
                std::vector<int> codes;
                for (auto& [ch, pitch] : heldVirtualKeyboardKeys)
                    codes.push_back((int) ch);
                for (auto& [ch, pitch] : heldVirtualDrumKeys)
                    codes.push_back((int) ch);
                if (lastPressedKeyCode != 0)
                    codes.push_back(lastPressedKeyCode);
                return codes;
            },
            [this] { return drumGridModeActive; },
            [this] { return scaleRootPitchClass; },
            [this] { return currentScaleType != ScaleType::Off; });
        // setVisible(true) inside the constructor above can still make a
        // freshly-created native window key/frontmost at the OS level even
        // without an explicit toFront(true) call -- reclaim focus for the
        // main editor the same way updatePluginEditorWindowVisibility()
        // does for the plugin editor window.
        grabKeyboardFocus();
        return;
    }

    keyboardOverlayWindow->setVisible(!keyboardOverlayWindow->isVisible());
    grabKeyboardFocus();
}

void MainEditorComponent::togglePluginEditor()
{
    auto* instrument = playbackEngine.getTrackInstrument(cursorTrackIndex);
    if (instrument == nullptr)
        return; // no plugin loaded on the current track

    auto& window = pluginEditorWindowsByTrack[cursorTrackIndex];
    if (window == nullptr)
        window = std::make_unique<PluginEditorWindow>(*instrument, instrument->getName(), [this] { grabKeyboardFocus(); });

    pluginEditorDesiredVisible = !pluginEditorDesiredVisible;
    updatePluginEditorWindowVisibility();
}

void MainEditorComponent::updatePluginEditorWindowVisibility()
{
    for (auto& [trackIndex, window] : pluginEditorWindowsByTrack)
    {
        if (window == nullptr)
            continue;

        auto shouldShow = trackIndex == cursorTrackIndex && pluginEditorDesiredVisible;
        window->setVisible(shouldShow);
        // toFront(false): bring it visually forward without taking
        // keyboard focus (see this method's declaration -- a focused
        // plugin window silently breaks every keyboard shortcut in the
        // main editor).
        if (shouldShow)
            window->toFront(false);
    }

    // setVisible(true)/toFront(false) on a previously-hidden native window
    // can still make it key/frontmost at the OS level regardless -- always
    // reclaim keyboard focus for the main editor afterward.
    grabKeyboardFocus();
}

void MainEditorComponent::invalidatePluginEditorWindow(int trackIndex)
{
    pluginEditorWindowsByTrack.erase(trackIndex);
}

void MainEditorComponent::registerParameterAutomationListener(int trackIndex)
{
    if (auto* instrument = playbackEngine.getTrackInstrument(trackIndex))
        instrument->addListener(this);
}

void MainEditorComponent::unregisterParameterAutomationListener(int trackIndex)
{
    if (auto* instrument = playbackEngine.getTrackInstrument(trackIndex))
        instrument->removeListener(this);

    // Any gesture open on this track's (now-departing) plugin is moot --
    // drop it rather than leave a stale entry that could never be closed
    // (its matching gestureEnd, if any, will arrive for a processor
    // pointer that's no longer any track's instrument, and
    // findTrackIndexForProcessor() will just return -1 for it harmlessly).
    for (auto it = touchedParameters.begin(); it != touchedParameters.end(); )
        it = (it->first == trackIndex) ? touchedParameters.erase(it) : std::next(it);
}

int MainEditorComponent::findTrackIndexForProcessor(juce::AudioProcessor* processor)
{
    for (int i = 0; i < (int) project.tracks.size(); ++i)
        if (playbackEngine.getTrackInstrument(i) == processor)
            return i;
    return -1;
}

void MainEditorComponent::audioProcessorParameterChangeGestureBegin(juce::AudioProcessor* processor, int parameterIndex)
{
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
        return;
    // Arms tracking regardless of transport state -- audioProcessorParameterChanged()
    // below decides whether that means Realtime capture (playing,
    // recordParameterAutomationPoint()) or Manual-mode preview (stopped,
    // previewTouchedParameterValue()). Previously gated on
    // playbackEngine.isPlaying() too, which silently dropped every touch
    // made while stopped.
    if (!automationTouchModeEnabled)
        return;
    auto trackIndex = findTrackIndexForProcessor(processor);
    if (trackIndex < 0)
        return;

    // See touchPreviewValues' declaration -- nothing else open anywhere
    // means this is a genuinely fresh, unrelated touch, so drop whatever
    // preview values a previous (already-ended) session left behind.
    // A gesture beginning while another is already open (the common
    // multi-parameter-macro case, several parameters changing from one
    // physical touch) instead joins that existing batch.
    if (touchedParameters.empty())
        touchPreviewValues.clear();

    touchedParameters.insert({ trackIndex, parameterIndex });
}

void MainEditorComponent::audioProcessorParameterChangeGestureEnd(juce::AudioProcessor* processor, int parameterIndex)
{
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
        return;

    // Physically clicking/dragging a plugin's own knob necessarily gives
    // that (separate, native) window real OS keyboard focus for as long
    // as the mouse is down there -- there's no way around that and still
    // let Touch mode actually work, unlike showing the window itself
    // (which deliberately never steals focus, see
    // updatePluginEditorWindowVisibility()'s declaration). Left
    // unaddressed, every PC-keyboard note/shortcut key pressed while that
    // focus is still sitting on the plugin window went to the plugin (or
    // nowhere) instead of this editor, and macOS beeped for each one.
    // Reclaiming focus the instant the
    // gesture ends (mouse released) -- not on every parameter-changed
    // callback mid-drag, which would fight the plugin's own window for
    // focus while the user is still actively dragging -- restores normal
    // PC-keyboard operation immediately after each touch, without ever
    // needing to click back on the main window by hand.
    grabKeyboardFocus();

    auto trackIndex = findTrackIndexForProcessor(processor);
    if (trackIndex < 0)
        return;
    touchedParameters.erase({ trackIndex, parameterIndex });
}

void MainEditorComponent::audioProcessorParameterChanged(juce::AudioProcessor* processor, int parameterIndex, float newValue)
{
    // See this override's declaration -- our own playback applying
    // existing automation calls setValueNotifyingHost() from the audio
    // thread and must never reach the recording logic below.
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
        return;

    auto trackIndex = findTrackIndexForProcessor(processor);
    if (trackIndex < 0)
        return;
    // Gated on ANY gesture being open on this TRACK's plugin, not
    // specifically on this exact parameterIndex -- some plugins only ever
    // send a gesture-begin/end pair for the one control actually grabbed
    // (a macro knob, say) while the OTHER parameters it drives internally
    // just get plain parameterChanged calls with no gesture wrapper of
    // their own. Gating per-exact-index silently dropped every one of
    // those side-effect changes -- gating per-track
    // instead still filters out genuinely unrelated changes (preset load,
    // internal LFO) whenever NOTHING is being touched at all, while
    // catching every parameter that moves alongside whatever specific one
    // opened the gesture.
    auto hasOpenGestureOnTrack = std::any_of(touchedParameters.begin(), touchedParameters.end(),
        [trackIndex](const std::pair<int, int>& touched) { return touched.first == trackIndex; });
    if (!hasOpenGestureOnTrack)
        return;

    auto* parameter = processor->getParameters()[parameterIndex];
    if (parameter == nullptr)
        return;

    if (playbackEngine.isPlaying())
        recordParameterAutomationPoint(trackIndex, *parameter, newValue);
    else
        previewTouchedParameterValue(trackIndex, *parameter, newValue);
}

void MainEditorComponent::audioProcessorChanged(juce::AudioProcessor*, const juce::AudioProcessorListener::ChangeDetails&)
{
    // Fires for structural changes (latency, program count, etc.) --
    // nothing here needs reacting to.
}

void MainEditorComponent::recordParameterAutomationPoint(int trackIndex, juce::AudioProcessorParameter& parameter, float value)
{
    auto parameterID = stableParameterID(parameter);

    auto throttleKey = std::make_pair(trackIndex, parameterID);
    // Same reasoning/threshold as recordAutomationPoint()'s own throttle
    // -- a real knob drag fires a flood of changes, far more than is
    // musically meaningful to store individually. Uses the TOUCHED
    // track's own playback step, not necessarily cursorTrackIndex -- the
    // plugin being automated can belong to any track. Looked up with
    // find() rather than operator[] -- operator[] would default-construct
    // a MISSING entry to 0 (int's default), not -1, so a genuinely
    // first-ever touch landing early in playback (step < 8) could get
    // mistaken for "already recorded recently at step 0" and silently
    // throttled away before the lane below ever got created.
    auto step = playbackEngine.getTrackPlaybackStep(trackIndex);
    if (step < 0)
        return; // that track isn't actually playing right now
    auto throttleIt = lastParameterRecordStep.find(throttleKey);
    if (throttleIt != lastParameterRecordStep.end() && step - throttleIt->second < automationRecordMinStepGap)
        return;
    lastParameterRecordStep[throttleKey] = step;

    auto& lanes = project.tracks[(size_t) trackIndex].clip.parameterLanes;
    auto laneIt = std::find_if(lanes.begin(), lanes.end(),
        [&](const ParameterAutomationLane& lane) { return lane.parameterID == parameterID; });
    auto isNewLane = laneIt == lanes.end();
    if (isNewLane)
    {
        // First time this parameter's ever been touched -- auto-create
        // its lane, named for display (see ParameterAutomationLane's
        // declaration).
        ParameterAutomationLane newLane;
        newLane.parameterID = parameterID;
        newLane.parameterName = parameter.getName(64);
        lanes.push_back(newLane);
        laneIt = std::prev(lanes.end());
    }

    auto& points = laneIt->points;
    // Same "later take replaces" convention writeAutomationPoint() uses.
    points.erase(std::remove_if(points.begin(), points.end(),
        [step](const ParameterAutomationPoint& p) { return p.stepIndex == step; }), points.end());
    points.push_back({ step, value, AutomationCurveType::Curve, 0.0f });
    if (points.size() > 1 && points[points.size() - 2].stepIndex > points.back().stepIndex)
        std::sort(points.begin(), points.end(),
                  [](const ParameterAutomationPoint& a, const ParameterAutomationPoint& b) { return a.stepIndex < b.stepIndex; });

    // Visible confirmation that a touch actually landed -- reuses the
    // shortcut bar's "last action" corner (see its own declaration),
    // since Touch capture isn't a keypress and so never goes through
    // trigger()/setLastAction() any other way. Console log too, so the
    // exact recorded value/step/lane-created-or-not can be checked
    // against what was actually done with the knob.
    shortcutHelpBar.setLastAction("Touch: " + laneIt->parameterName + " = " + juce::String(value, 3)
        + (isNewLane ? " (new lane)" : ""));
    DBG("Parameter automation recorded: track " << trackIndex << " \"" << laneIt->parameterName << "\" ("
        << parameterID << ") step " << step << " value " << value << (isNewLane ? " [new lane]" : " [existing lane]")
        << " -- lane now has " << points.size() << " point(s)");

    refreshChildViews();
}

void MainEditorComponent::previewTouchedParameterValue(int trackIndex, juce::AudioProcessorParameter& parameter, float value)
{
    // Piano Roll only -- no step cursor/automation concept in Session View
    // (same scoping toggleAutomationEditMode() itself already enforces).
    if (currentViewMode != ViewMode::PianoRoll)
        return;

    if (!automationEditModeActive)
        automationEditModeActive = true; // Touch mode being on already means "I'm authoring automation" -- no separate confirmation needed, same as Realtime capture never asking either

    // The touched plugin can belong to any track, not necessarily whichever
    // one's currently selected -- switch to it (matching Touch's
    // established "picks the parameter, and by extension its track" role)
    // since automationEditLane/automationEditParameterLaneIndex below are
    // interpreted relative to project.tracks[cursorTrackIndex].clip
    // everywhere else in this file. Cursor step position is left
    // untouched, same as switchTrack()'s own established behavior.
    cursorTrackIndex = trackIndex;

    auto parameterID = stableParameterID(parameter);
    auto& lanes = project.tracks[(size_t) trackIndex].clip.parameterLanes;
    auto laneIt = std::find_if(lanes.begin(), lanes.end(),
        [&](const ParameterAutomationLane& lane) { return lane.parameterID == parameterID; });
    if (laneIt == lanes.end())
    {
        // First time this parameter's ever been touched, in EITHER Touch
        // sub-mode -- same auto-create as recordParameterAutomationPoint().
        ParameterAutomationLane newLane;
        newLane.parameterID = parameterID;
        newLane.parameterName = parameter.getName(64);
        lanes.push_back(newLane);
        laneIt = std::prev(lanes.end());
    }

    auto laneIndex = (int) std::distance(lanes.begin(), laneIt);

    // Keeps the single "selected lane" pointing at whichever parameter
    // most recently moved -- drives Cmd+Ctrl+L's own position, the lane
    // outline, and keyboard-driven Cmd+Ctrl+Z/X editing, same as before
    // this method supported more than one parameter at once. The actual
    // live value for THIS (and every other simultaneously touched)
    // parameter is tracked in touchPreviewValues below regardless of which
    // one ends up "selected" -- see its declaration.
    automationEditLane = AutomationLane::Parameter;
    automationEditParameterLaneIndex = laneIndex;
    // Deliberately NOT throttled (unlike recordParameterAutomationPoint()'s
    // automationRecordMinStepGap) -- this only ever updates a live preview,
    // never writes a point, so there's no reason to skip any of the knob's
    // actual movement; every value it passes through should be visible.
    parameterPendingValue = value;
    touchPreviewValues[{ trackIndex, laneIndex }] = value;

    shortcutHelpBar.setLastAction("Touch (preview): " + laneIt->parameterName + " = " + juce::String(value, 3));
    refreshChildViews();
}

void MainEditorComponent::toggleAutomationTouchMode()
{
    automationTouchModeEnabled = !automationTouchModeEnabled;
    refreshChildViews();
}

void MainEditorComponent::refreshMidiDeviceList()
{
    availableMidiDevices = juce::MidiInput::getAvailableDevices();

    midiDeviceBox.clear();
    for (int i = 0; i < availableMidiDevices.size(); ++i)
        midiDeviceBox.addItem(availableMidiDevices[i].name, i + 1);
}

void MainEditorComponent::midiDeviceSelected()
{
    auto index = midiDeviceBox.getSelectedItemIndex();
    grabKeyboardFocus();

    if (index < 0 || index >= availableMidiDevices.size())
        return;

    midiInputRouter.setActiveDevice(availableMidiDevices[index].identifier);
    getMidiInputDeviceSettingsFile().replaceWithText(availableMidiDevices[index].identifier);
}

juce::File MainEditorComponent::getMidiInputDeviceSettingsFile()
{
    auto appDataDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                           .getChildFile("Application Support")
                           .getChildFile("PianoRollMaestro");
    appDataDir.createDirectory();
    return appDataDir.getChildFile("MidiInputDevice.txt");
}

void MainEditorComponent::restoreSavedMidiInputDevice()
{
    auto settingsFile = getMidiInputDeviceSettingsFile();
    if (!settingsFile.existsAsFile())
        return;

    auto savedIdentifier = settingsFile.loadFileAsString().trim();
    if (savedIdentifier.isEmpty())
        return;

    for (int i = 0; i < availableMidiDevices.size(); ++i)
    {
        if (availableMidiDevices[i].identifier == savedIdentifier)
        {
            midiDeviceBox.setSelectedItemIndex(i, juce::dontSendNotification);
            midiInputRouter.setActiveDevice(savedIdentifier);
            break;
        }
    }
}

void MainEditorComponent::ensureStepExists(int trackIndex, int stepIndex)
{
    auto& steps = project.tracks[(size_t) trackIndex].clip.steps;
    while ((int) steps.size() <= stepIndex)
        steps.push_back(Step{});
}

int MainEditorComponent::realtimeOnsetStep() const
{
    auto rawStep = playbackEngine.getTrackPlaybackStep(cursorTrackIndex);
    auto& clip = project.tracks[(size_t) cursorTrackIndex].clip;
    // A sixteenth note's worth of tolerance either side of the wrap point --
    // generous enough to forgive normal anticipation timing without eating
    // into the following beat.
    auto tolerance = juce::jmax(1, clip.stepsPerQuarterNote / 4);

    if (project.loopEnabled && project.loopEndStep > project.loopStartStep)
    {
        // Marker-region loop -- wraps to loopStartStep once playback
        // reaches loopEndStep (see project.loopEnabled's declaration). The
        // upper bound covers a note struck just AFTER the wrap conceptually
        // already happened but this track's own cursor hasn't caught up
        // yet -- the wrap itself is only checked once per audio block (see
        // renderNextBlock()'s own comment), so for up to a block's worth of
        // steps after the true boundary, getTrackPlaybackStep() can still
        // report raw positions at/just past loopEndStep instead of having
        // already reset to loopStartStep. A note landing there reads like
        // "beat 5 of a 4-beat loop" -- there's no such beat, it's actually
        // beat 1 of the next lap, so it belongs at loopStartStep too, the
        // same as a note anticipated slightly before the boundary.
        if (rawStep >= project.loopEndStep - tolerance && rawStep < project.loopEndStep + tolerance)
            return project.loopStartStep;
    }
    else if (project.loopEnabled)
    {
        // No usable markers -- this clip loops to its own end instead.
        // Same before-AND-after tolerance window as the marker case above.
        auto wrapPoint = clip.effectiveLengthInSteps();
        if (wrapPoint > 0 && rawStep >= wrapPoint - tolerance && rawStep < wrapPoint + tolerance)
            return 0;
    }

    return rawStep;
}

void MainEditorComponent::handleMidiNoteChange(int noteNumber, float velocity, bool isOn)
{
    if (isOn)
    {
        // A press while nothing else is held starts capturing a brand-new
        // chord; a press while other notes are already down adds to it.
        // pendingChord is deliberately NOT touched on note-off (below) --
        // real chords are essentially never released in perfect unison, so
        // mirroring "currently held" live would erode the chord back down
        // to whatever's still held as each finger lifts, one at a time,
        // instead of keeping the full chord that was actually played.
        if (heldMidiNotes.empty())
        {
            pendingChord.clear();
            realtimeNoteOnsetSteps.clear();
            pendingChordIdleSinceMs = 0.0; // a new gesture is active -- not idle anymore
            stepGrid.setPreviewAlpha(1.0f); // cancel any fade-out already in progress

            // Real-time REC: a fresh gesture starting while recMode ==
            // Realtime and the transport is rolling captures the playhead
            // position RIGHT NOW -- see realtimeRecordStep's declaration --
            // so the note lands where it was actually played, not wherever
            // the (independent) edit cursor happens to sit. Also resets
            // multiSelectedNoteStarts here (a genuinely NEW real-time
            // gesture, as opposed to a mid-gesture individual flush or the
            // gesture's own final commit -- see both of their comments for
            // why neither of those clears it).
            if (recMode == RecMode::Realtime && playbackEngine.isPlaying())
            {
                realtimeRecordStep = realtimeOnsetStep();
                multiSelectedNoteStarts.clear();
            }
            // A note struck DURING the count-in (anticipating beat 1, before
            // isPlaying() ever goes true -- see isCountingIn()'s declaration)
            // is otherwise not real-time-capturable at all.
            // getCountInTargetStep() is exactly the step real playback is
            // about to resume at, so treat an anticipated beat-1 note as if
            // it landed there.
            else if (recMode == RecMode::Realtime && playbackEngine.isCountingIn())
            {
                realtimeRecordStep = playbackEngine.getCountInTargetStep();
                multiSelectedNoteStarts.clear();
            }
        }

        // A pitch already sitting in pendingChord from an EARLIER press
        // within this still-open gesture would normally mean a retrigger
        // (struck, released, struck again while some OTHER note is still
        // held) -- but Real-time REC now commits each note the INSTANT it's
        // released (see the note-off branch below), so by the time a
        // retrigger's note-on reaches here, the earlier occurrence has
        // already been committed and erased from pendingChord on its own.
        // Nothing left to reconcile here.

        // Every note-on during an active Real-time REC gesture (not just
        // the one that started it) captures ITS OWN onset step -- see
        // realtimeNoteOnsetSteps's declaration.
        if (recMode == RecMode::Realtime && playbackEngine.isPlaying())
            realtimeNoteOnsetSteps[noteNumber] = realtimeOnsetStep();
        else if (recMode == RecMode::Realtime && playbackEngine.isCountingIn())
            realtimeNoteOnsetSteps[noteNumber] = playbackEngine.getCountInTargetStep();

        // Retriggering an already-held pitch shouldn't duplicate it.
        for (auto it = heldMidiNotes.begin(); it != heldMidiNotes.end(); ++it)
        {
            if (it->pitch == noteNumber)
            {
                heldMidiNotes.erase(it);
                break;
            }
        }
        heldMidiNotes.push_back({ noteNumber, velocity });
        // Every press restarts this pitch's own held-duration clock, even a
        // retrigger -- see heldNoteOnTimestamps's declaration.
        heldNoteOnTimestamps[noteNumber] = juce::Time::getMillisecondCounterHiRes();

        auto alreadyPending = false;
        for (auto& n : pendingChord)
            if (n.pitch == noteNumber) { alreadyPending = true; break; }
        if (!alreadyPending)
            pendingChord.push_back({ noteNumber, velocity });
    }
    else
    {
        for (auto it = heldMidiNotes.begin(); it != heldMidiNotes.end(); ++it)
        {
            if (it->pitch == noteNumber)
            {
                heldMidiNotes.erase(it);
                break;
            }
        }
        // pendingChord stays as-is -- see the note-on branch above.

        // This pitch's own actual sounding length, independent of the rest
        // of the chord -- see StepNote::durationSteps. Real-time REC only
        // (realtimeRecordStep >= 0 for the whole duration of an in-progress
        // real-time gesture) -- Manual/Step-auto REC always commit the
        // whole chord at the current commit-duration preset uniformly,
        // regardless of how long each key was actually held. This was
        // briefly made unconditional (every REC mode) to fix two different
        // pitches struck together but released at different times sharing
        // one flat duration in Real-time REC -- but that meant Step REC's
        // own notes started carrying real key-hold timing too, silently
        // overriding the duration preset any time a key wasn't held for
        // exactly that long. Step REC's whole paradigm
        // is "the written length is whatever the grid preset says,
        // regardless of gesture" -- there's no real timing to preserve
        // there in the first place, so the fix only ever needed to apply
        // during Real-time REC specifically. The timestamp itself is still
        // always erased below regardless of recMode, so it never lingers.
        auto timestampIt = heldNoteOnTimestamps.find(noteNumber);
        if (timestampIt != heldNoteOnTimestamps.end())
        {
            if (realtimeRecordStep >= 0)
            {
                auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - timestampIt->second;
                auto& clip = project.tracks[(size_t) cursorTrackIndex].clip;
                auto stepSeconds = clip.stepDurationSeconds(project.tempoBpm);
                if (stepSeconds > 0.0)
                {
                    auto elapsedSteps = juce::jmax(1, (int) std::round((elapsedMs / 1000.0) / stepSeconds));
                    for (auto& n : pendingChord)
                        if (n.pitch == noteNumber) { n.durationSteps = elapsedSteps; break; }
                }
            }
            heldNoteOnTimestamps.erase(timestampIt);
        }

        // Real-time REC: commit THIS note the instant it's released,
        // rather than waiting for every other currently-held note to
        // release too. Unlike Manual/Step-auto REC (a single fixed-
        // position chord, which genuinely needs to wait for the whole
        // press to finish before it even knows what to write), a
        // real-time performance's notes each already carry their OWN
        // onset/duration (captured above), so nothing is gained by
        // batching them -- and overlapping/legato playing (especially
        // natural with the sustain pedal held) can otherwise keep at
        // least one note physically down for an entire passage, meaning
        // heldMidiNotes never empties and NOTHING gets written until the
        // whole passage finally ends -- the pedal
        // itself wasn't actually the bug; it just makes overlapping/
        // legato playing far more likely, which is what actually starves
        // the old "wait for full silence" auto-commit below). Notes that
        // share the same onset step still merge back into one chord via
        // commitPendingNoteAt()'s own merge branch, so a genuine
        // simultaneous chord is unaffected by committing each of its
        // notes separately as they're released.
        if (recMode == RecMode::Realtime && realtimeRecordStep >= 0 && playbackEngine.isPlaying())
        {
            for (auto it = pendingChord.begin(); it != pendingChord.end(); ++it)
            {
                if (it->pitch == noteNumber)
                {
                    auto onsetIt = realtimeNoteOnsetSteps.find(noteNumber);
                    auto onset = onsetIt != realtimeNoteOnsetSteps.end() ? onsetIt->second : realtimeRecordStep;

                    StepEditGuard undoGuard(*this);
                    commitPendingNoteAt(onset, commitDurationPresets[(size_t) commitDurationPresetIndex], { *it });
                    multiSelectedNoteStarts.push_back(onset);
                    if (autoQuantizeOnRecordEnabled)
                        quantizeSelectedNotesImpl(lastQuantizeGridSteps);

                    pendingChord.erase(it);
                    realtimeNoteOnsetSteps.erase(noteNumber);
                    refreshChildViews();
                    break;
                }
            }
        }

        // The gesture just completed (every key released) -- starts the
        // pendingChordTimeoutMs countdown (see pendingChordIdleSinceMs's
        // declaration). Set unconditionally, regardless of recMode: even
        // when Auto-commit below consumes pendingChord immediately, this is
        // harmless (timerCallback() only acts while pendingChord is still
        // non-empty).
        if (heldMidiNotes.empty() && !pendingChord.empty())
            pendingChordIdleSinceMs = juce::Time::getMillisecondCounterHiRes();

        // Auto-commit fires here, once, regardless of how many notes were
        // in the chord (each individual note-off checks
        // heldMidiNotes.empty(), so only the LAST release triggers the
        // commit) -- for Auto mode (Step REC auto, any transport state) or
        // Realtime mode while the transport is actually rolling (real-time
        // capture). Realtime mode while STOPPED deliberately does nothing
        // here at all -- pendingChord just sits there as a preview until
        // its own idle timeout clears it, never getting written (see
        // RecMode::Realtime's declaration).
        auto shouldAutoCommit = recMode == RecMode::Auto
                              || (recMode == RecMode::Realtime && playbackEngine.isPlaying());
        if (heldMidiNotes.empty() && shouldAutoCommit && !pendingChord.empty())
        {
            StepEditGuard undoGuard(*this);
            if (realtimeRecordStep >= 0)
            {
                // Each pendingChord note already carries its own durationSteps
                // (set above) -- this fallback only matters for the rare note
                // that somehow never went through a note-off before the
                // gesture was considered complete. Grouped by each note's
                // own onset step (realtimeNoteOnsetSteps) rather than
                // written uniformly at realtimeRecordStep, so a note played
                // partway through a still-held chord lands at the step it
                // was actually played at instead of aligning to the first
                // note.
                std::map<int, std::vector<StepNote>> notesByOnset;
                for (auto& n : pendingChord)
                {
                    auto onsetIt = realtimeNoteOnsetSteps.find(n.pitch);
                    auto onset = onsetIt != realtimeNoteOnsetSteps.end() ? onsetIt->second : realtimeRecordStep;
                    notesByOnset[onset].push_back(n);
                }
                // Real-time REC'd notes are very likely to get quantized
                // next (raw human timing is the whole point of this mode),
                // so leave every note just written selected -- a following
                // '1'/'2'/'3' or Cmd+A-style bulk action needs no separate
                // selection step first. commitPendingNoteAt() always
                // writes/merges exactly at targetStep, so onsetStep is
                // always the resulting note's actual owning step index.
                // NOT cleared here -- the per-note-release commit above may
                // already have pushed onsets from earlier in this same
                // gesture; clearing here would drop those from the final
                // selection. Only a genuinely NEW gesture starting (see
                // heldMidiNotes.empty() above) resets this.
                for (auto& [onsetStep, notes] : notesByOnset)
                {
                    commitPendingNoteAt(onsetStep, commitDurationPresets[(size_t) commitDurationPresetIndex], notes); // edit cursor stays put
                    multiSelectedNoteStarts.push_back(onsetStep);
                }

                // Cmd+Shift+U -- see autoQuantizeOnRecordEnabled's
                // declaration. Calls the impl directly (no extra
                // StepEditGuard): this whole block already has one open
                // (see the top of this if), and nesting a second one would
                // push a spurious extra undo transaction instead of folding
                // the auto-quantize into the same undo step as the commit
                // itself.
                if (autoQuantizeOnRecordEnabled)
                    quantizeSelectedNotesImpl(lastQuantizeGridSteps);

                refreshChildViews();

                realtimeRecordStep = -1;
            }
            else
            {
                commitPendingNote(); // Step REC (auto) -- writes at and advances the edit cursor
            }
        }

        // The gesture is over the instant every physical key is up --
        // regardless of whether shouldAutoCommit fired above (e.g. Realtime
        // mode while stopped, which deliberately writes nothing), there's
        // no "current gesture" left to track. Set unconditionally here
        // rather than only inside the block above, since the per-note-
        // release commit earlier in this function normally drains
        // pendingChord note-by-note, meaning that block frequently has
        // nothing left to do by the time the last key releases.
        if (heldMidiNotes.empty() && recMode == RecMode::Realtime)
            realtimeRecordStep = -1;
    }

    liveNote(noteNumber, velocity, isOn);
    updatePendingNoteDisplays();
}

void MainEditorComponent::liveNote(int noteNumber, float velocity, bool isOn)
{
    // Remember exactly which (track, shifted pitch) a raw note-on actually
    // sounded as, and reuse that same pair for its note-off -- recomputing
    // the shift fresh at note-off time used whatever octaveShiftOctaves/
    // cursorTrackIndex happen to be RIGHT NOW, which is wrong if either
    // changed while the note was still sounding (e.g. pressing z/x to shift
    // octave while a note is still ringing). That sent the note-off
    // to a different pitch than the one actually on, leaving the original
    // note stuck sounding forever.
    if (isOn)
    {
        // A fresh press of an already-sustained pitch just resumes as an
        // ordinary new note-on below (see sustainedLiveNotePitches'
        // declaration) -- nothing left to flush for it later.
        sustainedLiveNotePitches.erase(std::remove(sustainedLiveNotePitches.begin(), sustainedLiveNotePitches.end(), noteNumber),
                                        sustainedLiveNotePitches.end());
        auto shiftedNote = juce::jlimit(0, 127, noteNumber + octaveShiftOctaves * 12);
        activeLiveNotes[noteNumber] = { cursorTrackIndex, shiftedNote };
        playbackEngine.liveNoteOn(cursorTrackIndex, shiftedNote, velocity);
    }
    else
    {
        // See sustainedLiveNotePitches' declaration -- defer the actual
        // note-off while the pedal is down, entirely independent of
        // whatever handleMidiNoteChange() (this function's only caller)
        // already did above for recording purposes.
        if (midiSustainPedalDown)
        {
            if (std::find(sustainedLiveNotePitches.begin(), sustainedLiveNotePitches.end(), noteNumber) == sustainedLiveNotePitches.end())
                sustainedLiveNotePitches.push_back(noteNumber);
            return;
        }

        auto it = activeLiveNotes.find(noteNumber);
        if (it == activeLiveNotes.end())
            return; // nothing actually sounding for this raw pitch

        playbackEngine.liveNoteOff(it->second.trackIndex, it->second.shiftedPitch);
        activeLiveNotes.erase(it);
    }
}

void MainEditorComponent::flushSustainedLiveNotes()
{
    auto pitches = sustainedLiveNotePitches;
    sustainedLiveNotePitches.clear();
    for (auto pitch : pitches)
        liveNote(pitch, 0.0f, false); // midiSustainPedalDown is already false by the time this runs, so this actually releases them
}

void MainEditorComponent::forceConfirmPendingSustainRelease()
{
    if (pendingSustainCrossingMs < 0.0)
        return;
    resolvePendingSustainCrossing();
}

void MainEditorComponent::resolvePendingSustainCrossing()
{
    pendingSustainCrossingMs = -1.0;
    if (lastRawSustainDown == midiSustainPedalDown)
        return; // raw settled back to the already-applied state -- nothing to do

    midiSustainPedalDown = lastRawSustainDown;
    recordSustainPedalEvent(midiSustainPedalDown);
    // Forwarded immediately regardless of what's currently sounding --
    // PlaybackEngine::TrackAudioState::activeNotePitches is what re-
    // triggers anything a buggy instrument incorrectly kills on a real
    // CC64=0, and it does so correctly for BOTH this live forward and any
    // recorded automation resent during playback, which an earlier
    // version of this defer (kept only here) could never see or cover.
    playbackEngine.liveMidiMessage(cursorTrackIndex, juce::MidiMessage::controllerEvent(1, 64, midiSustainPedalDown ? 127 : 0));
    if (!midiSustainPedalDown)
        flushSustainedLiveNotes();
}

void MainEditorComponent::recordSustainPedalEvent(bool pedalDown)
{
    // Real CC64 automation is written any time the transport is actually
    // playing, regardless of recMode -- unlike NOTE capture (which stays
    // gated to Realtime REC specifically, see realtimeRecordStep's
    // declaration), automation hardware capture doesn't have a Browse/
    // Manual/Auto "commit gesture" concept to gate against in the first
    // place, so there was never a real reason to also require
    // recMode==Realtime here; it just happened to inherit that check from
    // being written alongside note capture originally. Relaxed to match
    // the equally permissive plugin-parameter Touch capture (see
    // MainEditorComponent::audioProcessorParameterChangeGestureBegin()).
    // Live audio preview (the playbackEngine.liveMidiMessage()
    // calls in onLiveControllerMessage/pollVirtualKeyboardInput) reflects
    // the pedal regardless of REC state either way; this only controls
    // whether anything gets written into the clip.
    //
    // Deliberately NOT wrapped in a StepEditGuard, even though it now also
    // tracks sustainPedalEvents/pitchBendPoints/filterCutoffPoints (see its
    // declaration -- keyboard-drawn automation edits use it directly). A
    // real pedal/wheel sends a continuous flood of messages while being
    // moved; wrapping every individual call here would open a fresh undo
    // TRANSACTION per message (StepEditGuard's destructor calls
    // beginNewTransaction() unconditionally whenever anything changed),
    // turning one smooth real-time gesture into dozens of separate
    // Cmd+Z steps instead of one. Hardware-recorded automation is
    // accordingly still not individually undoable, same as pendingChord
    // and a few other pieces of REC-time state -- undoing a whole take
    // means re-recording it, same as it already does for notes.
    if (!playbackEngine.isPlaying())
        return;

    auto step = realtimeOnsetStep();
    auto& events = project.tracks[(size_t) cursorTrackIndex].clip.sustainPedalEvents;

    // Same-instant duplicate guard (e.g. a rising edge recorded the instant
    // before a stale debounce candidate somehow also confirms) -- nothing
    // new to record.
    if (!events.empty() && events.back().stepIndex == step && events.back().pedalDown == pedalDown)
        return;

    auto wasEmpty = events.empty();

    // Re-recording a Real-time REC take over a region that already has
    // pedal automation from an EARLIER take otherwise just accumulates
    // both, potentially leaving two conflicting entries at the exact same
    // step (one from each take) with no defined order between them once
    // sorted -- unlike notes, which cleanly overwrite/merge at their own
    // step via commitPendingNoteAt(). The newest take always wins at any
    // step it itself writes to. Runs before the backfill below so it can
    // never erase what the backfill is about to add.
    events.erase(std::remove_if(events.begin(), events.end(),
        [step](const SustainPedalEvent& ev) { return ev.stepIndex == step; }), events.end());

    // The pedal can already be held down before this recording session
    // even starts (pressed first, then playback started) -- if so, the
    // very first transition Real-time REC ever sees is a RELEASE with no
    // earlier PRESS to anchor it. Left alone, the whole span from the
    // clip's start up to that release would read back as "pedal was never
    // down at all" both in playback and the piano roll's sustain lane,
    // silently dropping sustain on exactly the opening notes it was meant
    // to cover.
    // Backfilling an implicit press at step 0 is safe only when this is
    // the very first event ever recorded for this clip (nothing earlier
    // to conflict with) AND this release isn't itself exactly at step 0
    // (which would leave nothing meaningful to backfill and would
    // otherwise collide with the very event being recorded here).
    if (wasEmpty && !pedalDown && step > 0)
        events.push_back({ 0, true });

    events.push_back({ step, pedalDown });

    // Recorded out of order only if the transport looped back mid-gesture
    // (a launched Session View slot, or the global loop region) --
    // PlaybackEngine::scheduleUpTo()'s binary search over this vector
    // requires ascending stepIndex order.
    if (events.size() > 1 && events[events.size() - 2].stepIndex > events.back().stepIndex)
        std::sort(events.begin(), events.end(),
                  [](const SustainPedalEvent& a, const SustainPedalEvent& b) { return a.stepIndex < b.stepIndex; });
}

void MainEditorComponent::writeAutomationPoint(std::vector<AutomationPoint>& points, int stepIndex, int value,
                                                AutomationCurveType curveType, float curveAmount)
{
    // A later take (or a manual hand-drawn edit) re-writing the same step
    // replaces rather than accumulates alongside whatever was there
    // before -- same reasoning as recordSustainPedalEvent()'s own
    // re-recording guard.
    points.erase(std::remove_if(points.begin(), points.end(),
        [stepIndex](const AutomationPoint& p) { return p.stepIndex == stepIndex; }), points.end());

    points.push_back({ stepIndex, value, curveType, curveAmount });

    if (points.size() > 1 && points[points.size() - 2].stepIndex > points.back().stepIndex)
        std::sort(points.begin(), points.end(),
                  [](const AutomationPoint& a, const AutomationPoint& b) { return a.stepIndex < b.stepIndex; });

    refreshChildViews();
}

void MainEditorComponent::recordAutomationPoint(AutomationLane lane, int value)
{
    // Real MIDI hardware capture writes any time the transport is
    // actually playing, regardless of recMode -- see
    // recordSustainPedalEvent()'s declaration for why this was relaxed
    // off the old recMode==Realtime requirement. Live audio preview is
    // unaffected either way (handled directly in onLiveControllerMessage
    // regardless of this gate).
    if (!playbackEngine.isPlaying())
        return;

    auto step = realtimeOnsetStep();
    auto& lastRecordStep = lane == AutomationLane::PitchBend ? lastPitchBendRecordStep : lastFilterCutoffRecordStep;

    // Throttle -- a real wheel sends a flood of MIDI messages while being
    // moved; recording every single one would balloon the point list far
    // beyond what's musically meaningful. See automationRecordMinStepGap's
    // declaration.
    if (lastRecordStep >= 0 && step - lastRecordStep < automationRecordMinStepGap)
        return;
    lastRecordStep = step;

    auto& points = lane == AutomationLane::PitchBend
        ? project.tracks[(size_t) cursorTrackIndex].clip.pitchBendPoints
        : project.tracks[(size_t) cursorTrackIndex].clip.filterCutoffPoints;
    writeAutomationPoint(points, step, value);
}

void MainEditorComponent::toggleAutomationEditMode()
{
    // Piano Roll only -- Session View has no step cursor/automation
    // concept, same scoping as the note-editing keys.
    if (currentViewMode != ViewMode::PianoRoll)
        return;
    automationEditModeActive = !automationEditModeActive;
    refreshChildViews();
}

void MainEditorComponent::cycleAutomationLane()
{
    auto& parameterLanes = project.tracks[(size_t) cursorTrackIndex].clip.parameterLanes;

    if (automationEditLane == AutomationLane::Sustain)
    {
        automationEditLane = AutomationLane::PitchBend;
    }
    else if (automationEditLane == AutomationLane::PitchBend)
    {
        automationEditLane = AutomationLane::FilterCutoff;
    }
    else if (automationEditLane == AutomationLane::FilterCutoff)
    {
        // Steps into however many touch-recorded parameter lanes this
        // track currently has (see AutomationLane::Parameter's
        // declaration) -- skips straight to Sustain if there are none,
        // same as before this lane category existed.
        if (!parameterLanes.empty())
        {
            automationEditLane = AutomationLane::Parameter;
            automationEditParameterLaneIndex = 0;
        }
        else
        {
            automationEditLane = AutomationLane::Sustain;
        }
    }
    else // Parameter
    {
        if (automationEditParameterLaneIndex + 1 < (int) parameterLanes.size())
            ++automationEditParameterLaneIndex;
        else
            automationEditLane = AutomationLane::Sustain;
    }

    refreshChildViews();
}

void MainEditorComponent::adjustAutomationPendingValue(int direction, bool coarse)
{
    if (automationEditLane == AutomationLane::Sustain)
        return; // no continuous PENDING value on a binary on/off event list

    StepEditGuard undoGuard(*this);
    auto targets = effectiveSelectedAutomationSteps();

    if (automationEditLane == AutomationLane::Parameter)
    {
        auto& clip = project.tracks[(size_t) cursorTrackIndex].clip;
        if (automationEditParameterLaneIndex < 0 || automationEditParameterLaneIndex >= (int) clip.parameterLanes.size())
            return;
        auto& points = clip.parameterLanes[(size_t) automationEditParameterLaneIndex].points;
        // 0.0-1.0 normalized range (see ParameterAutomationPoint's
        // declaration) -- fine/coarse step sized relative to that range
        // the same way Filter Cutoff's is relative to its own 0-127 range,
        // so a plugin parameter feels about as fine-grained to dial in by
        // keyboard as the other continuous lanes.
        auto delta = (coarse ? 16.0f : 1.0f) / 127.0f * (float) direction;

        if (targets.size() > 1)
        {
            for (auto& p : points)
                if (std::find(targets.begin(), targets.end(), p.stepIndex) != targets.end())
                    p.value = juce::jlimit(0.0f, 1.0f, p.value + delta);
        }
        else
        {
            parameterPendingValue = juce::jlimit(0.0f, 1.0f, parameterPendingValue + delta);
            for (auto& p : points)
            {
                if (p.stepIndex == cursorStepIndex)
                {
                    p.value = parameterPendingValue;
                    break;
                }
            }
        }

        refreshChildViews();
        return;
    }

    auto minValue = 0;
    auto maxValue = automationEditLane == AutomationLane::PitchBend ? 16383 : 127;
    // Coarse (Shift+T/G) step doubled from its original size at the
    // user's request -- it felt too small to cover the lane's range in a
    // reasonable number of presses. Fine (plain t/g) unchanged.
    auto delta = automationEditLane == AutomationLane::PitchBend
        ? (coarse ? 2048 : 128) * direction
        : (coarse ? 16 : 1) * direction;
    auto& points = automationEditLane == AutomationLane::PitchBend
        ? project.tracks[(size_t) cursorTrackIndex].clip.pitchBendPoints
        : project.tracks[(size_t) cursorTrackIndex].clip.filterCutoffPoints;

    if (targets.size() > 1)
    {
        // Bulk (Shift+D/F multi-selection): shift every selected point by
        // the SAME delta, preserving their relative differences -- like
        // transpose does for a multi-note selection -- rather than
        // collapsing them all to one identical value.
        for (auto& p : points)
            if (std::find(targets.begin(), targets.end(), p.stepIndex) != targets.end())
                p.value = juce::jlimit(minValue, maxValue, p.value + delta);
    }
    else
    {
        // Single point (or none) at the cursor: adjusts the shared "what
        // Ctrl+V/Cmd+Ctrl+I will place next" pending value instead, moving
        // that one existing point to match right away if there is one.
        auto& pendingValue = automationEditLane == AutomationLane::PitchBend ? pitchBendPendingValue : filterCutoffPendingValue;
        pendingValue = juce::jlimit(minValue, maxValue, pendingValue + delta);
        for (auto& p : points)
        {
            if (p.stepIndex == cursorStepIndex)
            {
                p.value = pendingValue;
                break;
            }
        }
    }

    refreshChildViews();
}

void MainEditorComponent::toggleSustainEventAtCursor()
{
    if (automationEditLane != AutomationLane::Sustain)
        return;

    StepEditGuard undoGuard(*this);
    auto& events = project.tracks[(size_t) cursorTrackIndex].clip.sustainPedalEvents;

    // "Currently down at the cursor" is whatever the last event AT OR
    // BEFORE the cursor left it as (or off, if nothing precedes it) --
    // same lookup StepGridComponent's sustain-lane paint() uses. Toggling
    // inserts the opposite state exactly at the cursor.
    auto it = std::upper_bound(events.begin(), events.end(), cursorStepIndex,
        [](int step, const SustainPedalEvent& ev) { return step < ev.stepIndex; });
    auto currentlyDown = it != events.begin() && std::prev(it)->pedalDown;

    events.erase(std::remove_if(events.begin(), events.end(),
        [this](const SustainPedalEvent& ev) { return ev.stepIndex == cursorStepIndex; }), events.end());
    events.push_back({ cursorStepIndex, !currentlyDown });

    if (events.size() > 1 && events[events.size() - 2].stepIndex > events.back().stepIndex)
        std::sort(events.begin(), events.end(),
                  [](const SustainPedalEvent& a, const SustainPedalEvent& b) { return a.stepIndex < b.stepIndex; });

    refreshChildViews();
}

void MainEditorComponent::insertAutomationPointAtCursor()
{
    StepEditGuard undoGuard(*this);
    if (automationEditLane == AutomationLane::PitchBend)
        writeAutomationPoint(project.tracks[(size_t) cursorTrackIndex].clip.pitchBendPoints, cursorStepIndex, pitchBendPendingValue, pitchBendPendingCurveType, pitchBendPendingCurveAmount);
    else if (automationEditLane == AutomationLane::FilterCutoff)
        writeAutomationPoint(project.tracks[(size_t) cursorTrackIndex].clip.filterCutoffPoints, cursorStepIndex, filterCutoffPendingValue, filterCutoffPendingCurveType, filterCutoffPendingCurveAmount);
    else if (automationEditLane == AutomationLane::Parameter)
    {
        if (!touchPreviewValues.empty())
        {
            // Commits EVERY parameter currently holding a live touch-
            // preview value at once, not just the Cmd+Ctrl+L-selected lane
            // -- see touchPreviewValues' declaration (a single physical
            // touch can drive several plugin parameters simultaneously,
            // and all of their points, even ones outside the currently
            // selected lane, should land together).
            for (auto& [key, value] : touchPreviewValues)
            {
                auto [touchedTrackIndex, laneIndex] = key;
                if (touchedTrackIndex < 0 || touchedTrackIndex >= (int) project.tracks.size())
                    continue;
                auto& laneList = project.tracks[(size_t) touchedTrackIndex].clip.parameterLanes;
                if (laneIndex < 0 || laneIndex >= (int) laneList.size())
                    continue;
                writeParameterAutomationPoint(laneList[(size_t) laneIndex].points, cursorStepIndex, value,
                                               parameterPendingCurveType, parameterPendingCurveAmount);
            }

            // Unlike pitchBendPendingValue/filterCutoffPendingValue (which
            // deliberately DO stick around after a commit, so the same
            // single value can be placed again at another position),
            // clearing this batch here stops it from silently tagging
            // along on every later Cmd+Ctrl+I too -- without this, moving
            // the cursor with c/v to a completely different spot and
            // committing there kept re-writing the SAME whole group of
            // parameters at the new spot as well, even when only one of
            // them was actually meant to move there. Any parameter still
            // being actively touched right
            // now re-populates its own entry immediately on its very next
            // audioProcessorParameterChanged() call, so an in-progress
            // gesture isn't affected by this.
            touchPreviewValues.clear();
        }
        else
        {
            // Nothing's ever been touch-previewed this session (e.g. the
            // pending value came purely from keyboard Cmd+Ctrl+Z/X) --
            // commit just the selected lane, same as this always worked
            // before touchPreviewValues existed.
            auto& clip = project.tracks[(size_t) cursorTrackIndex].clip;
            if (automationEditParameterLaneIndex >= 0 && automationEditParameterLaneIndex < (int) clip.parameterLanes.size())
                writeParameterAutomationPoint(clip.parameterLanes[(size_t) automationEditParameterLaneIndex].points,
                                               cursorStepIndex, parameterPendingValue, parameterPendingCurveType, parameterPendingCurveAmount);
        }
    }
}

void MainEditorComponent::writeParameterAutomationPoint(std::vector<ParameterAutomationPoint>& points, int stepIndex, float value,
                                                          AutomationCurveType curveType, float curveAmount)
{
    // Same "later take replaces" convention as writeAutomationPoint().
    points.erase(std::remove_if(points.begin(), points.end(),
        [stepIndex](const ParameterAutomationPoint& p) { return p.stepIndex == stepIndex; }), points.end());

    points.push_back({ stepIndex, value, curveType, curveAmount });

    if (points.size() > 1 && points[points.size() - 2].stepIndex > points.back().stepIndex)
        std::sort(points.begin(), points.end(),
                  [](const ParameterAutomationPoint& a, const ParameterAutomationPoint& b) { return a.stepIndex < b.stepIndex; });

    refreshChildViews();
}

void MainEditorComponent::deleteAutomationPointAtCursor()
{
    StepEditGuard undoGuard(*this);
    auto& clip = project.tracks[(size_t) cursorTrackIndex].clip;
    // Deletes every point/event in effectiveSelectedAutomationSteps() (a
    // Shift+D/F multi-selection if one exists, else just the one at the
    // cursor) -- mirrors clearCurrentStep() acting on
    // effectiveSelectedNoteStarts().
    auto targets = effectiveSelectedAutomationSteps();
    if (automationEditLane == AutomationLane::Sustain)
    {
        auto& events = clip.sustainPedalEvents;
        events.erase(std::remove_if(events.begin(), events.end(),
            [&targets](const SustainPedalEvent& ev) { return std::find(targets.begin(), targets.end(), ev.stepIndex) != targets.end(); }), events.end());
    }
    else if (automationEditLane == AutomationLane::Parameter)
    {
        // Only real keyboard-driven edit Parameter lanes support -- lets
        // an accidentally-touched/stray point get cleaned up without
        // having to re-touch the plugin's knob just to overwrite it.
        if (automationEditParameterLaneIndex >= 0 && automationEditParameterLaneIndex < (int) clip.parameterLanes.size())
        {
            auto& points = clip.parameterLanes[(size_t) automationEditParameterLaneIndex].points;
            points.erase(std::remove_if(points.begin(), points.end(),
                [&targets](const ParameterAutomationPoint& p) { return std::find(targets.begin(), targets.end(), p.stepIndex) != targets.end(); }), points.end());
        }
    }
    else
    {
        auto& points = automationEditLane == AutomationLane::PitchBend ? clip.pitchBendPoints : clip.filterCutoffPoints;
        points.erase(std::remove_if(points.begin(), points.end(),
            [&targets](const AutomationPoint& p) { return std::find(targets.begin(), targets.end(), p.stepIndex) != targets.end(); }), points.end());
    }
    multiSelectedAutomationSteps.clear(); // its points are gone -- nothing left to point at

    // Matches clearCurrentStep()'s own "select nearest note" fallback --
    // see its declaration: jump to the nearest PRECEDING
    // point/event if the cursor now has none of its own lane exactly under
    // it, else the nearest FOLLOWING one, else back to bar 1 if the lane
    // is empty entirely -- rather than leaving the cursor stranded with
    // nothing to act on for whatever comes next.
    auto stepIndices = automationStepIndicesForCurrentLane();
    if (!stepIndices.empty())
    {
        auto it = std::upper_bound(stepIndices.begin(), stepIndices.end(), cursorStepIndex);
        cursorStepIndex = (it != stepIndices.begin()) ? *std::prev(it) : *it;
    }
    else
    {
        cursorStepIndex = 0;
    }

    refreshChildViews();
}

void MainEditorComponent::nudgeSelectedAutomationPoints(int direction)
{
    StepEditGuard undoGuard(*this);
    auto& clip = project.tracks[(size_t) cursorTrackIndex].clip;
    auto targets = effectiveSelectedAutomationSteps();
    // Same collision-safe processing order reasoning as
    // nudgeSelectedNotes() -- moving right must process the rightmost
    // point first so it never lands on a not-yet-moved sibling's still-
    // occupied step (and symmetrically for moving left).
    if (direction > 0)
        std::sort(targets.rbegin(), targets.rend());
    else
        std::sort(targets.begin(), targets.end());

    auto cursorWasAt = cursorStepIndex;

    if (automationEditLane == AutomationLane::Sustain)
    {
        auto& events = clip.sustainPedalEvents;
        for (auto rawStep : targets)
        {
            auto it = std::find_if(events.begin(), events.end(),
                [rawStep](const SustainPedalEvent& e) { return e.stepIndex == rawStep; });
            if (it == events.end())
                continue; // already moved/merged away by an earlier iteration this batch

            auto targetStep = juce::jmax(0, rawStep + direction);
            if (targetStep == rawStep)
                continue;

            auto pedalDown = it->pedalDown;
            events.erase(it);
            // Landing exactly on another event's step overwrites it -- same
            // "later take replaces" convention recordSustainPedalEvent()/
            // toggleSustainEventAtCursor() already use.
            events.erase(std::remove_if(events.begin(), events.end(),
                [targetStep](const SustainPedalEvent& e) { return e.stepIndex == targetStep; }), events.end());
            events.push_back({ targetStep, pedalDown });

            for (auto& sel : multiSelectedAutomationSteps)
                if (sel == rawStep)
                    sel = targetStep;
            if (cursorWasAt == rawStep)
                cursorStepIndex = targetStep;
        }
        std::sort(events.begin(), events.end(),
                  [](const SustainPedalEvent& a, const SustainPedalEvent& b) { return a.stepIndex < b.stepIndex; });
    }
    else if (automationEditLane == AutomationLane::Parameter)
    {
        if (automationEditParameterLaneIndex >= 0 && automationEditParameterLaneIndex < (int) clip.parameterLanes.size())
        {
            auto& points = clip.parameterLanes[(size_t) automationEditParameterLaneIndex].points;
            for (auto rawStep : targets)
            {
                auto it = std::find_if(points.begin(), points.end(),
                    [rawStep](const ParameterAutomationPoint& p) { return p.stepIndex == rawStep; });
                if (it == points.end())
                    continue;

                auto targetStep = juce::jmax(0, rawStep + direction);
                if (targetStep == rawStep)
                    continue;

                auto moved = *it;
                points.erase(it);
                points.erase(std::remove_if(points.begin(), points.end(),
                    [targetStep](const ParameterAutomationPoint& p) { return p.stepIndex == targetStep; }), points.end());
                moved.stepIndex = targetStep;
                points.push_back(moved);

                for (auto& sel : multiSelectedAutomationSteps)
                    if (sel == rawStep)
                        sel = targetStep;
                if (cursorWasAt == rawStep)
                    cursorStepIndex = targetStep;
            }
            std::sort(points.begin(), points.end(),
                      [](const ParameterAutomationPoint& a, const ParameterAutomationPoint& b) { return a.stepIndex < b.stepIndex; });
        }
    }
    else
    {
        auto& points = automationEditLane == AutomationLane::PitchBend ? clip.pitchBendPoints : clip.filterCutoffPoints;
        for (auto rawStep : targets)
        {
            auto it = std::find_if(points.begin(), points.end(),
                [rawStep](const AutomationPoint& p) { return p.stepIndex == rawStep; });
            if (it == points.end())
                continue;

            auto targetStep = juce::jmax(0, rawStep + direction);
            if (targetStep == rawStep)
                continue;

            auto moved = *it;
            points.erase(it);
            // Same "later take replaces" convention as writeAutomationPoint().
            points.erase(std::remove_if(points.begin(), points.end(),
                [targetStep](const AutomationPoint& p) { return p.stepIndex == targetStep; }), points.end());
            moved.stepIndex = targetStep;
            points.push_back(moved);

            for (auto& sel : multiSelectedAutomationSteps)
                if (sel == rawStep)
                    sel = targetStep;
            if (cursorWasAt == rawStep)
                cursorStepIndex = targetStep;
        }
        std::sort(points.begin(), points.end(),
                  [](const AutomationPoint& a, const AutomationPoint& b) { return a.stepIndex < b.stepIndex; });
    }

    refreshChildViews();
}

void MainEditorComponent::cycleAutomationCurveTypeAtCursor()
{
    if (automationEditLane == AutomationLane::Sustain)
        return; // no curve concept for a binary on/off event list

    StepEditGuard undoGuard(*this);
    auto& clip = project.tracks[(size_t) cursorTrackIndex].clip;
    // Toggles every point in effectiveSelectedAutomationSteps() together --
    // a Shift+D/F multi-selection, or just the one at the cursor.
    auto targets = effectiveSelectedAutomationSteps();

    if (automationEditLane == AutomationLane::Parameter)
    {
        if (automationEditParameterLaneIndex < 0 || automationEditParameterLaneIndex >= (int) clip.parameterLanes.size())
            return;
        auto& points = clip.parameterLanes[(size_t) automationEditParameterLaneIndex].points;
        auto toggledAny = false;
        for (auto& p : points)
        {
            if (std::find(targets.begin(), targets.end(), p.stepIndex) == targets.end())
                continue;
            p.curveType = p.curveType == AutomationCurveType::Step ? AutomationCurveType::Curve : AutomationCurveType::Step;
            toggledAny = true;
        }
        if (!toggledAny)
            parameterPendingCurveType = parameterPendingCurveType == AutomationCurveType::Step ? AutomationCurveType::Curve : AutomationCurveType::Step;
        refreshChildViews();
        return;
    }

    auto& points = automationEditLane == AutomationLane::PitchBend ? clip.pitchBendPoints : clip.filterCutoffPoints;
    auto toggledAny = false;
    for (auto& p : points)
    {
        if (std::find(targets.begin(), targets.end(), p.stepIndex) == targets.end())
            continue;
        p.curveType = p.curveType == AutomationCurveType::Step ? AutomationCurveType::Curve : AutomationCurveType::Step;
        toggledAny = true;
    }
    if (toggledAny)
    {
        refreshChildViews();
        return;
    }

    // No real point sits at the cursor -- toggle the PENDING curve type
    // instead (see its declaration). This is what shapes the ghost
    // preview's incoming segment and gets written into the next point
    // Ctrl+V/Cmd+Ctrl+I places.
    auto& pendingCurve = automationEditLane == AutomationLane::PitchBend ? pitchBendPendingCurveType : filterCutoffPendingCurveType;
    pendingCurve = pendingCurve == AutomationCurveType::Step ? AutomationCurveType::Curve : AutomationCurveType::Step;
    refreshChildViews();
}

void MainEditorComponent::adjustAutomationPendingCurveAmount(int direction, bool coarse)
{
    if (automationEditLane == AutomationLane::Sustain)
        return; // no curve concept on this lane

    StepEditGuard undoGuard(*this);
    constexpr float minAmount = -1.0f, maxAmount = 1.0f;
    auto delta = (coarse ? 0.2f : 0.05f) * (float) direction;
    auto& clip = project.tracks[(size_t) cursorTrackIndex].clip;
    auto targets = effectiveSelectedAutomationSteps();

    if (automationEditLane == AutomationLane::Parameter)
    {
        if (automationEditParameterLaneIndex < 0 || automationEditParameterLaneIndex >= (int) clip.parameterLanes.size())
            return;
        auto& points = clip.parameterLanes[(size_t) automationEditParameterLaneIndex].points;

        if (targets.size() > 1)
        {
            for (auto& p : points)
                if (p.curveType == AutomationCurveType::Curve && std::find(targets.begin(), targets.end(), p.stepIndex) != targets.end())
                    p.curveAmount = juce::jlimit(minAmount, maxAmount, p.curveAmount + delta);
        }
        else
        {
            parameterPendingCurveAmount = juce::jlimit(minAmount, maxAmount, parameterPendingCurveAmount + delta);
            for (auto& p : points)
            {
                if (p.stepIndex == cursorStepIndex)
                {
                    if (p.curveType == AutomationCurveType::Curve)
                        p.curveAmount = parameterPendingCurveAmount;
                    break;
                }
            }
        }

        refreshChildViews();
        return;
    }

    auto& points = automationEditLane == AutomationLane::PitchBend ? clip.pitchBendPoints : clip.filterCutoffPoints;

    if (targets.size() > 1)
    {
        // Bulk (Shift+D/F multi-selection): shift every selected point's
        // amount by the SAME delta, same convention adjustAutomationPendingValue()
        // uses for value. Step points are left alone -- their curveAmount
        // is unused, adjusting it would be a silent no-op anyway but
        // skipping it keeps intent clear.
        for (auto& p : points)
            if (p.curveType == AutomationCurveType::Curve && std::find(targets.begin(), targets.end(), p.stepIndex) != targets.end())
                p.curveAmount = juce::jlimit(minAmount, maxAmount, p.curveAmount + delta);
    }
    else
    {
        // Single point (or none) at the cursor: adjusts the shared
        // pending curve amount, moving that one existing point to match
        // right away if there is one and it's a Curve point -- mirrors
        // adjustAutomationPendingValue()'s own live-update behavior.
        auto& pendingAmount = automationEditLane == AutomationLane::PitchBend ? pitchBendPendingCurveAmount : filterCutoffPendingCurveAmount;
        pendingAmount = juce::jlimit(minAmount, maxAmount, pendingAmount + delta);
        for (auto& p : points)
        {
            if (p.stepIndex == cursorStepIndex)
            {
                if (p.curveType == AutomationCurveType::Curve)
                    p.curveAmount = pendingAmount;
                break;
            }
        }
    }

    refreshChildViews();
}

std::vector<int> MainEditorComponent::automationStepIndicesForCurrentLane() const
{
    auto& clip = project.tracks[(size_t) cursorTrackIndex].clip;
    std::vector<int> stepIndices;
    if (automationEditLane == AutomationLane::Sustain)
    {
        stepIndices.reserve(clip.sustainPedalEvents.size());
        for (auto& ev : clip.sustainPedalEvents)
            stepIndices.push_back(ev.stepIndex);
    }
    else if (automationEditLane == AutomationLane::Parameter)
    {
        if (automationEditParameterLaneIndex < 0 || automationEditParameterLaneIndex >= (int) clip.parameterLanes.size())
            return stepIndices; // stale index (e.g. lanes changed underneath) -- empty, same as "no points yet"
        auto& points = clip.parameterLanes[(size_t) automationEditParameterLaneIndex].points;
        stepIndices.reserve(points.size());
        for (auto& p : points)
            stepIndices.push_back(p.stepIndex);
    }
    else
    {
        auto& points = automationEditLane == AutomationLane::PitchBend ? clip.pitchBendPoints : clip.filterCutoffPoints;
        stepIndices.reserve(points.size());
        for (auto& p : points)
            stepIndices.push_back(p.stepIndex);
    }
    return stepIndices;
}

void MainEditorComponent::moveCursorToAdjacentAutomationPoint(int direction, bool clearSelection)
{
    // "Shift extends, plain move collapses to one" -- same convention
    // handleForwardKey()/handleBackwardKey() already use for
    // multiSelectedNoteStarts.
    if (clearSelection)
        multiSelectedAutomationSteps.clear();

    auto stepIndices = automationStepIndicesForCurrentLane();

    if (stepIndices.empty())
    {
        moveCursor(direction); // nothing to jump between yet -- fall back to plain step movement
        return;
    }

    if (direction > 0)
    {
        auto it = std::upper_bound(stepIndices.begin(), stepIndices.end(), cursorStepIndex);
        if (it == stepIndices.end())
            return; // already at/past the last point -- hold still, no wraparound
        moveCursor(*it - cursorStepIndex);
    }
    else
    {
        auto it = std::lower_bound(stepIndices.begin(), stepIndices.end(), cursorStepIndex);
        if (it == stepIndices.begin())
            return; // already at/before the first point -- hold still, no wraparound
        moveCursor(*std::prev(it) - cursorStepIndex);
    }
}

void MainEditorComponent::extendAutomationSelection(int direction)
{
    // On the very first Shift+D/F press of a gesture, seed the selection
    // with the point the cursor was already sitting on (if any) -- same
    // reasoning as extendNoteSelection()'s own seeding step, and looked
    // up BEFORE the move for the same reason (the move can land somewhere
    // that no longer matches the pre-move cursor position).
    if (multiSelectedAutomationSteps.empty())
    {
        auto stepIndicesBeforeMove = automationStepIndicesForCurrentLane();
        if (std::find(stepIndicesBeforeMove.begin(), stepIndicesBeforeMove.end(), cursorStepIndex) != stepIndicesBeforeMove.end())
            multiSelectedAutomationSteps.push_back(cursorStepIndex);
    }

    // clearSelection=false -- this is the Shift+D/F "extend" path, not
    // plain d/f, so the selection just seeded above (or already built up
    // over previous presses) must survive the jump.
    moveCursorToAdjacentAutomationPoint(direction, false);
    auto stepIndices = automationStepIndicesForCurrentLane();
    if (std::find(stepIndices.begin(), stepIndices.end(), cursorStepIndex) == stepIndices.end())
        return; // held still (no point that direction) -- nothing new to add

    if (std::find(multiSelectedAutomationSteps.begin(), multiSelectedAutomationSteps.end(), cursorStepIndex) == multiSelectedAutomationSteps.end())
        multiSelectedAutomationSteps.push_back(cursorStepIndex);
    refreshChildViews();
}

std::vector<int> MainEditorComponent::effectiveSelectedAutomationSteps() const
{
    if (!multiSelectedAutomationSteps.empty())
        return multiSelectedAutomationSteps;
    auto stepIndices = automationStepIndicesForCurrentLane();
    if (std::find(stepIndices.begin(), stepIndices.end(), cursorStepIndex) != stepIndices.end())
        return { cursorStepIndex };
    return {};
}

void MainEditorComponent::copySelectedAutomationPoints()
{
    auto targets = effectiveSelectedAutomationSteps();
    if (targets.empty())
        return;

    auto anchor = *std::min_element(targets.begin(), targets.end());
    automationClipboard.clear();

    auto& clip = project.tracks[(size_t) cursorTrackIndex].clip;
    if (automationEditLane == AutomationLane::Sustain)
    {
        for (auto& ev : clip.sustainPedalEvents)
            if (std::find(targets.begin(), targets.end(), ev.stepIndex) != targets.end())
                automationClipboard.push_back({ ev.stepIndex - anchor, ev.pedalDown ? 1 : 0, 0.0f, AutomationCurveType::Curve, 0.0f });
    }
    else if (automationEditLane == AutomationLane::Parameter)
    {
        if (automationEditParameterLaneIndex < 0 || automationEditParameterLaneIndex >= (int) clip.parameterLanes.size())
            return;
        auto& points = clip.parameterLanes[(size_t) automationEditParameterLaneIndex].points;
        for (auto& p : points)
            if (std::find(targets.begin(), targets.end(), p.stepIndex) != targets.end())
                automationClipboard.push_back({ p.stepIndex - anchor, 0, p.value, p.curveType, p.curveAmount });
    }
    else
    {
        auto& points = automationEditLane == AutomationLane::PitchBend ? clip.pitchBendPoints : clip.filterCutoffPoints;
        for (auto& p : points)
            if (std::find(targets.begin(), targets.end(), p.stepIndex) != targets.end())
                automationClipboard.push_back({ p.stepIndex - anchor, p.value, 0.0f, p.curveType, p.curveAmount });
    }
}

void MainEditorComponent::pasteAutomationPointsAtCursor()
{
    if (automationClipboard.empty())
        return;

    StepEditGuard undoGuard(*this);
    auto& clip = project.tracks[(size_t) cursorTrackIndex].clip;

    if (automationEditLane == AutomationLane::Sustain)
    {
        auto& events = clip.sustainPedalEvents;
        for (auto& copy : automationClipboard)
        {
            auto targetStep = juce::jmax(0, cursorStepIndex + copy.offsetSteps);
            events.erase(std::remove_if(events.begin(), events.end(),
                [targetStep](const SustainPedalEvent& ev) { return ev.stepIndex == targetStep; }), events.end());
            events.push_back({ targetStep, copy.value != 0 });
        }
        std::sort(events.begin(), events.end(), [](const SustainPedalEvent& a, const SustainPedalEvent& b) { return a.stepIndex < b.stepIndex; });
    }
    else if (automationEditLane == AutomationLane::Parameter)
    {
        if (automationEditParameterLaneIndex < 0 || automationEditParameterLaneIndex >= (int) clip.parameterLanes.size())
            return;
        auto& points = clip.parameterLanes[(size_t) automationEditParameterLaneIndex].points;
        for (auto& copy : automationClipboard)
        {
            auto targetStep = juce::jmax(0, cursorStepIndex + copy.offsetSteps);
            points.erase(std::remove_if(points.begin(), points.end(),
                [targetStep](const ParameterAutomationPoint& p) { return p.stepIndex == targetStep; }), points.end());
            points.push_back({ targetStep, copy.floatValue, copy.curveType, copy.curveAmount });
        }
        std::sort(points.begin(), points.end(), [](const ParameterAutomationPoint& a, const ParameterAutomationPoint& b) { return a.stepIndex < b.stepIndex; });
    }
    else
    {
        auto& points = automationEditLane == AutomationLane::PitchBend ? clip.pitchBendPoints : clip.filterCutoffPoints;
        for (auto& copy : automationClipboard)
        {
            auto targetStep = juce::jmax(0, cursorStepIndex + copy.offsetSteps);
            points.erase(std::remove_if(points.begin(), points.end(),
                [targetStep](const AutomationPoint& p) { return p.stepIndex == targetStep; }), points.end());
            points.push_back({ targetStep, copy.value, copy.curveType, copy.curveAmount });
        }
        std::sort(points.begin(), points.end(), [](const AutomationPoint& a, const AutomationPoint& b) { return a.stepIndex < b.stepIndex; });
    }

    refreshChildViews();
}

void MainEditorComponent::jumpToClipEnd()
{
    auto& clip = project.tracks[(size_t) cursorTrackIndex].clip;
    auto lastStep = juce::jmax(0, clip.effectiveLengthInSteps() - 1);
    moveCursor(lastStep - cursorStepIndex);
}

void MainEditorComponent::moveCursor(int deltaSteps)
{
    // The within-chord pitch narrowing (Cmd+T/G, see noteSelectionAnchorStep's
    // declaration) is only ever meant to apply for as long as the cursor
    // stays continuously on the chord it was narrowed at -- effectiveSelectedPitches()
    // was only ever checking "is the anchor STILL EQUAL to cursorStepIndex",
    // which stayed silently true if the cursor later moved away and back to
    // the exact same step index, re-applying a stale narrowing from a
    // completely unrelated earlier visit and making a fresh landing on that
    // chord act as if only the one previously-narrowed note existed.
    // Any actual cursor movement now
    // unconditionally invalidates it, so returning to a chord always starts
    // fresh (whole chord) unless explicitly re-narrowed.
    noteSelectionAnchorStep = -1;
    cursorStepIndex = juce::jmax(0, cursorStepIndex + deltaSteps);

    // Landing exactly on an existing automation point picks up ITS value
    // as the pending one, so the next Cmd+Ctrl+Z/X nudges from what's
    // actually there instead of from whatever was last left over
    // (otherwise the very first adjustment could jump the point to a
    // wildly different value with no visible relationship to where the
    // cursor landed).
    if (automationEditModeActive && automationEditLane == AutomationLane::Parameter)
    {
        auto& clip = project.tracks[(size_t) cursorTrackIndex].clip;
        if (automationEditParameterLaneIndex >= 0 && automationEditParameterLaneIndex < (int) clip.parameterLanes.size())
        {
            auto& points = clip.parameterLanes[(size_t) automationEditParameterLaneIndex].points;
            for (auto& p : points)
            {
                if (p.stepIndex != cursorStepIndex)
                    continue;
                parameterPendingValue = p.value;
                break;
            }
        }
    }
    else if (automationEditModeActive && automationEditLane != AutomationLane::Sustain)
    {
        auto& points = automationEditLane == AutomationLane::PitchBend
            ? project.tracks[(size_t) cursorTrackIndex].clip.pitchBendPoints
            : project.tracks[(size_t) cursorTrackIndex].clip.filterCutoffPoints;
        for (auto& p : points)
        {
            if (p.stepIndex != cursorStepIndex)
                continue;
            if (automationEditLane == AutomationLane::PitchBend)
                pitchBendPendingValue = p.value;
            else
                filterCutoffPendingValue = p.value;
            break;
        }
    }

    refreshChildViews();
    auditionNoteAtCursor(); // hear whatever's under the cursor as it moves, like scrubbing
}

void MainEditorComponent::moveCursorByNoteOrStep(int direction, bool fallbackToStep)
{
    noteSelectionAnchorStep = -1; // see moveCursor()'s comment -- covers this function's own direct cursorStepIndex assignment below too
    auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
    auto ownerIndex = findOwningNoteStepIndex(steps, cursorStepIndex);

    // If the cursor is currently on/within a note, jump to the previous/next
    // note's start instead of moving step by step -- much faster for
    // browsing already-composed content. On a genuine rest (or with no
    // further note that way), fallbackToStep decides what happens next --
    // see this method's declaration.
    if (ownerIndex >= 0)
    {
        int target = -1;

        if (direction > 0)
        {
            for (int i = cursorStepIndex + 1; i < (int) steps.size(); ++i)
                if (!steps[(size_t) i].tiedFromPrevious && !steps[(size_t) i].notes.empty())
                {
                    target = i;
                    break;
                }

            if (target < 0)
            {
                // No further note ahead -- jump straight to this note's own
                // end (its Note OFF point) rather than a plain 1-step nudge.
                auto noteEnd = ownerIndex + noteTotalLengthInSteps(steps, ownerIndex);
                if (noteEnd > cursorStepIndex)
                    target = noteEnd;
            }
        }
        else
        {
            for (int i = cursorStepIndex - 1; i >= 0; --i)
                if (!steps[(size_t) i].tiedFromPrevious && !steps[(size_t) i].notes.empty())
                {
                    target = i;
                    break;
                }

            if (target < 0 && ownerIndex < cursorStepIndex)
                target = ownerIndex; // no earlier note -- jump to this note's own start
        }

        if (target >= 0)
        {
            cursorStepIndex = target;
            refreshChildViews();
            auditionNoteAtCursor();
            return;
        }
        // already exactly at this note's boundary -- fall through below
    }
    else if (!fallbackToStep)
    {
        // Cursor is on a genuine rest, not on/within any note -- plain d/f
        // used to simply no-op here. Instead,
        // jump to the nearest note: search in the pressed key's own
        // direction first, and if that side has nothing, fall back to the
        // opposite direction rather than doing nothing at all.
        auto searchForward = [&steps](int start) -> int
        {
            for (int i = juce::jmax(0, start); i < (int) steps.size(); ++i)
                if (!steps[(size_t) i].tiedFromPrevious && !steps[(size_t) i].notes.empty())
                    return i;
            return -1;
        };
        auto searchBackward = [&steps](int start) -> int
        {
            for (int i = juce::jmin(start, (int) steps.size() - 1); i >= 0; --i)
                if (!steps[(size_t) i].tiedFromPrevious && !steps[(size_t) i].notes.empty())
                    return i;
            return -1;
        };

        auto target = direction > 0 ? searchForward(cursorStepIndex) : searchBackward(cursorStepIndex);
        if (target < 0)
            target = direction > 0 ? searchBackward(cursorStepIndex) : searchForward(cursorStepIndex);

        if (target >= 0)
        {
            cursorStepIndex = target;
            refreshChildViews();
            auditionNoteAtCursor();
        }
        return;
    }

    if (!fallbackToStep)
        return; // pure note-jump mode (plain d/f) -- nothing further to jump to, stay put

    // Flat duration-preset step (rest fallback, extendNoteSelection()'s
    // Shift+D/F use only -- see fallbackToStep). For backward, if that
    // step lands inside an existing note's span instead of precisely on a
    // boundary -- e.g. stepping back by one beat from a rest just past a
    // longer chord -- snap straight to that note's own head instead of
    // stranding the cursor mid-note, so a single D press lands squarely on
    // the chord rather than requiring a second press to actually reach it.
    // Inlines moveCursor()'s own clamp/
    // refresh/audition rather than calling it and re-snapping afterward,
    // so this only ever triggers one audition, not two.
    auto rawTarget = juce::jmax(0, cursorStepIndex + direction * commitDurationPresets[(size_t) commitDurationPresetIndex]);
    auto landedOwnerIndex = direction < 0 ? findOwningNoteStepIndex(steps, rawTarget) : -1;
    noteSelectionAnchorStep = -1;
    cursorStepIndex = landedOwnerIndex >= 0 ? landedOwnerIndex : rawTarget;
    refreshChildViews();
    auditionNoteAtCursor();
}

void MainEditorComponent::handleForwardKey()
{
    // Automation edit mode redefines d/f as point-to-point navigation --
    // see moveCursorToAdjacentAutomationPoint()'s declaration. Checked
    // first and returns early: none of the note-navigation/StepEditGuard
    // machinery below applies while editing automation.
    if (automationEditModeActive)
    {
        moveCursorToAdjacentAutomationPoint(1);
        return;
    }

    StepEditGuard undoGuard(*this);
    multiSelectedNoteStarts.clear();

    // Pure note-to-note navigation, never commits -- committing lives on
    // Ctrl+V (see commitPendingNoteManually()). Duration-preset-only
    // movement is 'c'/'v' instead (retreatByDuration()/advanceByDuration()).
    moveCursorByNoteOrStep(1, false);
}

void MainEditorComponent::commitPendingNoteManually()
{
    // Browse mode never commits. Realtime mode only commits while the
    // transport is actually rolling -- while stopped it's deliberately
    // preview-only, same as Browse, so checking sounds before recording
    // never accidentally writes Step input even via a manual override
    // (see RecMode::Realtime's declaration).
    if (recMode == RecMode::Off || (recMode == RecMode::Realtime && !playbackEngine.isPlaying()) || pendingChord.empty())
        return;

    StepEditGuard undoGuard(*this);
    multiSelectedNoteStarts.clear(); // same as handleForwardKey()'s old unconditional clear
    commitPendingNote();
}

void MainEditorComponent::handleBackwardKey()
{
    // See handleForwardKey()'s matching check.
    if (automationEditModeActive)
    {
        moveCursorToAdjacentAutomationPoint(-1);
        return;
    }

    StepEditGuard undoGuard(*this);
    multiSelectedNoteStarts.clear();

    // Plain pure note-to-note navigation -- see handleForwardKey(). No
    // delete side effect.
    moveCursorByNoteOrStep(-1, false);
}

void MainEditorComponent::switchTrack(int deltaTracks)
{
    auto numTracks = (int) project.tracks.size();
    cursorTrackIndex = juce::jlimit(0, numTracks - 1, cursorTrackIndex + deltaTracks);
    updatePluginEditorWindowVisibility();
    refreshChildViews();
}

void MainEditorComponent::addTrack()
{
    Track newTrack;
    newTrack.name = "Track " + juce::String(project.tracks.size() + 1);

    // push_back() can reallocate project.tracks's storage, which
    // PlaybackEngine reads from the audio thread via a raw pointer with no
    // synchronization of its own -- unsafe while playing unless we know
    // it WON'T reallocate. project.tracks is kept reserve()'d well beyond
    // any realistic track count (see reservedTrackCapacity's declaration)
    // specifically so this stays a plain push_back(), no locking needed at
    // all. A CriticalSection shared with the
    // audio thread was tried first instead, but even briefly locking OUT
    // the audio thread on every single block turned out to itself cause
    // audible stutter when a track was actually added during playback --
    // reserving ahead of time
    // avoids the need for any lock in the first place. Only in the
    // essentially-never-hit case where reservedTrackCapacity has actually
    // been exhausted does this fall back to the plain stop-first behavior
    // this always had before the reserve existed.
    if (project.tracks.size() >= project.tracks.capacity())
        playbackEngine.stop();

    project.tracks.push_back(newTrack);

    // Pre-builds the new track's TrackAudioState (a Synthesiser + voices +
    // a MidiMessageCollector) here on the message thread right now, instead
    // of leaving renderNextBlock()'s own ensureTrackAudioStates() call to
    // build it on the audio thread the next time it runs -- that
    // allocation happening ON the audio thread was a separate stutter
    // source that persisted even after project.tracks itself stopped being
    // a hazard above. See
    // PlaybackEngine::prepareTrackAudioStates()'s declaration.
    playbackEngine.prepareTrackAudioStates();

    // Switching the view to the new (empty) track and resetting the edit
    // cursor to step 0 is exactly what you want right after adding a track
    // while stopped -- you're about to start writing into it. But while
    // playing, this jump looks/feels like playback itself just restarted
    // from the beginning: the piano roll follows the playhead of whatever
    // track is currently being viewed, and the new track's own cursor
    // legitimately starts at step 0 of a totally blank grid, even though
    // every OTHER track (and the transport's actual sample position) just
    // keeps playing on completely undisturbed.
    // So only auto-switch while stopped; while playing,
    // leave the current view alone and just add the track quietly in the
    // background -- switch to it manually (`3`/`e` or Cmd+Ctrl+P/N)
    // whenever you're ready to start writing into it.
    if (!playbackEngine.isPlaying())
    {
        cursorTrackIndex = (int) project.tracks.size() - 1;
        cursorStepIndex = 0;
    }
    refreshChildViews();
}

void MainEditorComponent::advanceByDuration()
{
    // Pure navigation -- doesn't touch step content at all, so whatever's
    // already at the cursor (a note or a rest) is left exactly as it was.
    // Advances by the current duration preset (Shift+Z/X), same amount
    // commitPendingNote()/tieCurrentStep() use, so the locator lands at the
    // next beat position consistently regardless of what's under it.
    moveCursor(commitDurationPresets[(size_t) commitDurationPresetIndex]);
}

void MainEditorComponent::retreatByDuration()
{
    // Backward twin of advanceByDuration() -- see its comment. Plain 'v'
    // (both views), pairing with 'b' (Piano Roll only; Session View's 'b'
    // stays Duplicate Clip) now that d/f moved back to pure note-jumping
    // instead.
    moveCursor(-commitDurationPresets[(size_t) commitDurationPresetIndex]);
}

void MainEditorComponent::deleteWholeNoteAt(int ownerIndex)
{
    // Deleting any part of a note removes the WHOLE note (all its
    // tied-continuation steps too), not just one step -- otherwise you'd
    // leave a dangling partial tie behind.
    auto length = noteTotalLengthInSteps(project.tracks[(size_t) cursorTrackIndex].clip.steps, ownerIndex);
    for (int i = 0; i < length; ++i)
    {
        ensureStepExists(cursorTrackIndex, ownerIndex + i);
        project.tracks[(size_t) cursorTrackIndex].clip.steps[(size_t) (ownerIndex + i)] = Step{};
    }
}

void MainEditorComponent::deleteAndRetreat()
{
    StepEditGuard undoGuard(*this);
    noteSelectionAnchorStep = -1; // see moveCursor()'s comment -- content is changing here too, same as clearCurrentStep()

    auto target = juce::jmax(0, cursorStepIndex - 1); // same clamping moveCursor(-1) used to apply

    auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
    auto ownerIndex = findOwningNoteStepIndex(steps, target);

    if (ownerIndex >= 0)
    {
        // Cursor lands on the note's own start, not just one step back.
        deleteWholeNoteAt(ownerIndex);
        cursorStepIndex = ownerIndex;
    }
    else
    {
        cursorStepIndex = target;
        ensureStepExists(cursorTrackIndex, cursorStepIndex);
        project.tracks[(size_t) cursorTrackIndex].clip.steps[(size_t) cursorStepIndex] = Step{};
    }

    refreshChildViews();
}

void MainEditorComponent::clearCurrentStep()
{
    StepEditGuard undoGuard(*this);

    auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
    auto cursorOwnerIndex = findOwningNoteStepIndex(steps, cursorStepIndex);
    // Every note this clears: normally just the cursor's own chord, but if
    // a Shift+D/F time-axis multi-note selection has been built, every one
    // of those notes gets cleared too -- previously this only ever looked
    // at cursorStepIndex, so clearing after a multi-note selection only
    // ever removed whichever single note the cursor happened to land on
    // last.
    auto targetStarts = effectiveSelectedNoteStarts();

    if (!targetStarts.empty())
    {
        for (auto stepIndex : targetStarts)
        {
            if (stepIndex < 0 || stepIndex >= (int) steps.size())
                continue;

            // Only the chord the cursor is actually inside honors the
            // narrowed within-chord pitch selection (t/g,
            // effectiveSelectedPitches()) -- every other step in a
            // multi-note selection has no such narrowing concept, so it's
            // cleared in full, exactly like before this feature existed.
            auto& ownerNotes = steps[(size_t) stepIndex].notes;
            std::vector<int> selected = (stepIndex == cursorOwnerIndex)
                ? effectiveSelectedPitches()
                : std::vector<int>{};
            if (stepIndex != cursorOwnerIndex)
                for (auto& note : ownerNotes)
                    selected.push_back(note.pitch);

            for (auto pitch : selected)
                for (auto it = ownerNotes.begin(); it != ownerNotes.end(); ++it)
                    if (it->pitch == pitch) { ownerNotes.erase(it); break; }

            // Nothing left in this note at all -- clean up its tied
            // continuation steps too (root + every tied continuation step),
            // not just whichever single step the cursor happens to be on.
            // Clearing only the root left its tied continuations behind as
            // orphans -- tiedFromPrevious=true, no notes of their own, no
            // longer owned by any note-start -- invisible everywhere that
            // skips tied/empty steps (the grid, ChordEstimator) but never
            // removed either, since trimTrailingEmptySteps() deliberately
            // leaves tiedFromPrevious steps alone (correct for a real
            // note's sustain, wrong once its root is gone). That's the
            // garbage nothing cleaned up.
            if (ownerNotes.empty())
                deleteWholeNoteAt(stepIndex);
        }

        noteSelectionAnchorStep = -1; // content changed -- fall back to whole-chord next time
        multiSelectedNoteStarts.clear(); // its notes are gone -- nothing left to point at
    }
    else if (cursorStepIndex < (int) steps.size())
    {
        steps[(size_t) cursorStepIndex] = Step{};
        noteSelectionAnchorStep = -1; // content changed -- fall back to whole-chord next time
    }
    // else: cursorStepIndex is already past the end of the array -- already
    // an implicit rest, nothing to clear, no need to pad the vector for it.

    // Also discards whatever's currently pending -- otherwise there was no
    // way to cancel a stray/mis-played note short of playing a new
    // (correct) one to overwrite it, or committing the wrong one with 'f'.
    pendingChord.clear();
    pendingChordIdleSinceMs = 0.0;
    stepGrid.setPreviewAlpha(1.0f); // cancel any fade-out already in progress

    // If the cursor now has nothing left under it at all, select the
    // nearest PRECEDING note instead of leaving nothing targetable --
    // otherwise the next command (another 'a', a pitch nudge, quantize,
    // copy...) would silently have nothing to act on.
    // If there's no preceding note either, fall forward to the nearest
    // FOLLOWING note instead; if there's nothing at all anywhere, there's
    // nothing left to select, so send the locator back to bar 1 rather
    // than leaving it stranded wherever the last note used to be.
    // Re-fetches steps fresh --
    // deleteWholeNoteAt() above may have grown (reallocated) the vector
    // via ensureStepExists().
    auto& stepsAfterClear = project.tracks[(size_t) cursorTrackIndex].clip.steps;
    if (findOwningNoteStepIndex(stepsAfterClear, cursorStepIndex) < 0)
    {
        auto foundIndex = -1;
        // Clamped to the array's actual last valid index, not just
        // cursorStepIndex - 1 -- cursorStepIndex can legitimately sit past
        // the end of stepsAfterClear (an "implicit rest" position, e.g.
        // after a multi-note Shift+D/F selection was built and the cursor
        // then moved further via some other navigation command without
        // clearing the selection), and indexing from an unclamped
        // cursorStepIndex - 1 crashed with an out-of-bounds vector access.
        for (int i = juce::jmin(cursorStepIndex - 1, (int) stepsAfterClear.size() - 1); i >= 0; --i)
        {
            if (!stepsAfterClear[(size_t) i].tiedFromPrevious && !stepsAfterClear[(size_t) i].notes.empty())
            {
                foundIndex = i;
                break;
            }
        }
        if (foundIndex < 0)
        {
            for (int i = cursorStepIndex + 1; i < (int) stepsAfterClear.size(); ++i)
            {
                if (!stepsAfterClear[(size_t) i].tiedFromPrevious && !stepsAfterClear[(size_t) i].notes.empty())
                {
                    foundIndex = i;
                    break;
                }
            }
        }

        if (foundIndex >= 0)
        {
            // Also move cursorStepIndex itself here, not just the
            // highlighted selection -- the piano roll's horizontal
            // auto-scroll always re-centers on cursorStepIndex (see
            // StepGridComponent::getFirstVisibleStep()), the same way d/f
            // moving between notes does. Leaving cursorStepIndex parked at
            // the now-cleared position left the newly-selected note
            // unfollowed by the view.
            cursorStepIndex = foundIndex;
            multiSelectedNoteStarts = { foundIndex };
        }
        else
            cursorStepIndex = 0; // nothing left anywhere in this track -- back to bar 1
    }

    refreshChildViews();
}

// Total sounding duration (in base steps) of the note starting at ownerIndex
// -- its own lengthInSteps plus any directly-following tiedFromPrevious
// steps' lengths, mirroring PlaybackEngine::scheduleUpTo's own duration math.
// Requires ownerIndex to already be a genuine note-start step (not tied,
// has notes).
static int noteTotalLengthInSteps(const std::vector<Step>& steps, int ownerIndex)
{
    auto totalLengthInSteps = steps[(size_t) ownerIndex].lengthInSteps;
    auto lookahead = ownerIndex + 1;
    while (lookahead < (int) steps.size() && steps[(size_t) lookahead].tiedFromPrevious)
    {
        totalLengthInSteps += steps[(size_t) lookahead].lengthInSteps;
        ++lookahead;
    }
    return totalLengthInSteps;
}

// Finds the step that actually owns the note data covering stepIndex,
// treating a single long step (lengthInSteps > 1, e.g. an eighth/quarter
// note committed as one Step), a tied chain, and a plain 1-step note all the
// same way: scans backward for the nearest note-starting step (not tied,
// has notes) and checks whether its total sounding span actually reaches far
// enough to cover stepIndex. Returns -1 if nothing covers stepIndex (a rest,
// or past the end of the nearest earlier note).
static int findOwningNoteStepIndex(const std::vector<Step>& steps, int stepIndex)
{
    for (int candidate = juce::jmin(stepIndex, (int) steps.size() - 1); candidate >= 0; --candidate)
    {
        auto& step = steps[(size_t) candidate];
        if (step.tiedFromPrevious || step.notes.empty())
            continue; // keep looking back for the actual note start

        return (candidate + noteTotalLengthInSteps(steps, candidate) > stepIndex) ? candidate : -1;
    }

    return -1;
}

void MainEditorComponent::auditionNoteAtCursor()
{
    // Suppressed during playback, always -- the transport is already
    // sounding whatever's actually playing, so a scrub-preview note (or
    // even an edit's own confirmation note) landing on top of it clashes
    // with the real audio instead of confirming anything useful. This used
    // to be conditional (edits like adjustNotePitch()/tieCurrentStep()
    // still audible mid-playback, navigation suppressed), but that
    // distinction was dropped -- moved notes should stay silent during
    // playback too.
    if (playbackEngine.isPlaying())
        return;

    auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
    auto ownerIndex = findOwningNoteStepIndex(steps, cursorStepIndex);
    if (ownerIndex < 0)
        return; // rest -- nothing to audition

    auto& step = steps[(size_t) ownerIndex];

    // Brief noteOn/noteOff, not a sustained hold -- except for a pitch
    // that's already sounding because the MIDI keyboard is still
    // physically holding it down (e.g. you committed the note with 'f'
    // while still holding it). Auditioning that pitch too would schedule a
    // delayed noteOff that fires well before the key is actually released,
    // cutting the real hold short. The physical hold always wins: skip
    // both the noteOn (redundant, it's already sounding) and the noteOff
    // for any pitch currently in activeLiveNotes for this track.
    auto selected = effectiveSelectedPitches(); // whole chord unless narrowed (see its declaration)

    std::vector<int> pitches;
    for (auto& note : step.notes)
    {
        if (std::find(selected.begin(), selected.end(), note.pitch) == selected.end())
            continue;

        auto alreadyHeld = false;
        for (auto& [rawPitch, active] : activeLiveNotes)
            if (active.trackIndex == cursorTrackIndex && active.shiftedPitch == note.pitch) { alreadyHeld = true; break; }
        if (alreadyHeld)
            continue;

        pitches.push_back(note.pitch);
        playbackEngine.liveNoteOn(cursorTrackIndex, note.pitch, note.velocity);
    }

    if (pitches.empty())
        return; // every pitch here is already sounding from a held key

    juce::Timer::callAfterDelay(150, [this, trackIndex = cursorTrackIndex, pitches]
    {
        for (auto pitch : pitches)
            playbackEngine.liveNoteOff(trackIndex, pitch);
    });
}

void MainEditorComponent::adjustNotePitch(int deltaSemitones)
{
    StepEditGuard undoGuard(*this);

    auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
    // Every note this transpose affects: normally just the cursor's own
    // chord, but if a Shift+D/F time-axis multi-note selection has been
    // built, every one of those notes moves together too -- previously this
    // only ever looked at cursorStepIndex, so transposing after a
    // multi-note selection silently did nothing to the rest of the
    // selection.
    auto targetStarts = effectiveSelectedNoteStarts();
    // effectiveSelectedNoteStarts()/multiSelectedNoteStarts store the note's
    // ROOT step index (findOwningNoteStepIndex()'s result), but cursorStepIndex
    // is wherever the cursor literally sits -- which can be a few steps into
    // a tied note's continuation, not necessarily the root. Comparing
    // stepIndex (root) against raw cursorStepIndex directly below would
    // then never match for a note whose root isn't under the cursor, so the
    // narrowed within-chord pitch selection got silently ignored (falling
    // back to "every note in the chord") any time the cursor was parked on
    // a tie's tail -- occasionally sending an unintended note into a real
    // collision with something elsewhere.
    auto cursorOwnerIndex = findOwningNoteStepIndex(steps, cursorStepIndex);

    // Remembers where the cursor's own chord ends up, so the view can
    // follow it below -- pitch shifted this data correctly the whole time,
    // but the piano roll's visible pitch WINDOW never followed along, so a
    // note transposed far enough scrolled off-screen and looked exactly
    // like it had vanished even though it hadn't -- the same "note
    // disappeared" symptom persisting after every data-level fix. Tracks
    // the LOWEST
    // and HIGHEST shifted pitch, not just one arbitrary note -- centering
    // on a single note (the first version of this fix) could scroll a
    // WIDE chord's other, correctly-moved notes off the opposite edge of
    // the view, which looked exactly like "only some notes followed".
    auto cursorChordLowestPitch = -1;
    auto cursorChordHighestPitch = -1;

    for (auto stepIndex : targetStarts)
    {
        if (stepIndex < 0 || stepIndex >= (int) steps.size())
            continue;

        auto& step = steps[(size_t) stepIndex];
        // Only the chord the cursor is actually inside honors the narrowed
        // within-chord pitch selection (t/g, effectiveSelectedPitches()) --
        // every other step in a multi-note selection has no such narrowing
        // concept, so all of its notes move together.
        std::vector<int> selected = (stepIndex == cursorOwnerIndex)
            ? effectiveSelectedPitches()
            : std::vector<int>{};
        if (stepIndex != cursorOwnerIndex)
            for (auto& note : step.notes)
                selected.push_back(note.pitch);

        // `selected` is a list of PITCH VALUES, which can't tell two notes
        // apart once they land on the identical pitch (allowed -- see
        // below). Consumed here as a MULTISET (remaining-count per pitch)
        // instead of a plain membership test, so exactly as many notes at
        // a given pitch get treated as "selected" as `selected` actually
        // contains at that pitch -- for the whole-chord case that's every
        // note sharing the pitch (selected already lists it that many
        // times), but for a NARROWED single-note selection that's still
        // just the one note, even after it collides with an unselected
        // chord-mate at the same pitch. Without this, a plain membership
        // test matched BOTH notes the instant they shared a pitch value,
        // so they moved together from then on and could never separate
        // again on a later press -- it wasn't that a note was deleted; two
        // notes got permanently welded together by an identity mix-up,
        // which looked the same in the piano roll as one note vanishing.
        std::map<int, int> remainingSelectedCount;
        for (auto pitch : selected)
            ++remainingSelectedCount[pitch];

        // Always move a selected note to its new pitch, even if another
        // note (selected or not) already sits there -- two StepNotes
        // temporarily landing on the identical pitch is allowed and just
        // renders as one overlapping block; since only the moving note(s)
        // keep changing, they separate again the next time this is
        // pressed. Earlier versions tried to detect and resolve that
        // overlap immediately (erasing whichever note collided, then later
        // blocking the move entirely) -- both still ended up "eating" a
        // note or permanently jamming it against its neighbor, which is
        // exactly the "billiards-chain" behavior the user rejected: two
        // notes are allowed to visually merge into one when they land on
        // the same pitch, but a further move must be able to separate them
        // back into two again, rather than leaving them permanently
        // chained together. The
        // only thing still blocked is going out of the valid MIDI range
        // (0-127) -- clamping there would permanently merge a note with
        // whatever else piled up at the boundary and lose the original gap
        // between them even after moving back away from it, which is a
        // real, irreversible data loss unlike the harmless temporary
        // overlap above.
        for (auto& note : step.notes)
        {
            auto countIt = remainingSelectedCount.find(note.pitch);
            if (countIt == remainingSelectedCount.end() || countIt->second <= 0)
                continue; // not selected, or already used up this pitch's selected count -- untouched
            --countIt->second;

            auto rawNewPitch = note.pitch + deltaSemitones;
            if (rawNewPitch < 0 || rawNewPitch > 127)
                continue; // out of MIDI range -- leave this note exactly where it is

            note.pitch = rawNewPitch;
            if (stepIndex == cursorOwnerIndex)
            {
                if (cursorChordLowestPitch < 0 || note.pitch < cursorChordLowestPitch)
                    cursorChordLowestPitch = note.pitch;
                if (cursorChordHighestPitch < 0 || note.pitch > cursorChordHighestPitch)
                    cursorChordHighestPitch = note.pitch;
            }
        }
    }

    if (cursorChordLowestPitch >= 0)
    {
        // Center on the chord's midpoint so both ends have the best chance
        // of staying on-screen together, then nudge toward whichever end
        // is still off-screen if the chord's own span is wider than half
        // the visible window.
        stepGrid.centerPitchView((cursorChordLowestPitch + cursorChordHighestPitch) / 2);
        stepGrid.centerPitchView(cursorChordHighestPitch);
        stepGrid.centerPitchView(cursorChordLowestPitch);
    }

    // Keep the same logical note(s) highlighted/focused after they moved --
    // otherwise the selection would silently point at pitches that no
    // longer exist in this chord. noteSelectionAnchorStep is left as-is
    // (still cursorStepIndex if it was already narrowed here).
    if (noteSelectionAnchorStep == cursorStepIndex)
    {
        for (auto& pitch : noteSelectionPitches)
            pitch = juce::jlimit(0, 127, pitch + deltaSemitones);
        if (noteSelectionFocusPitch >= 0)
            noteSelectionFocusPitch = juce::jlimit(0, 127, noteSelectionFocusPitch + deltaSemitones);
    }

    refreshChildViews();
    auditionNoteAtCursor(); // audible confirmation of the new pitch
}

void MainEditorComponent::adjustSelectionVoicingEdge(bool raiseLowest)
{
    // Unlike adjustNotePitch(), this only ever touches the cursor's own
    // chord (no multi-note time-axis selection support) -- "voicing" is a
    // within-chord concept, and effectiveSelectedPitches() already handles
    // "nothing narrowed yet" by returning the whole chord.
    auto selected = effectiveSelectedPitches();
    if (selected.size() < 2)
        return; // need at least two notes for "lowest" and "highest" to mean anything different

    auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
    auto ownerIndex = findOwningNoteStepIndex(steps, cursorStepIndex);
    if (ownerIndex < 0)
        return;

    auto targetPitch = raiseLowest ? *std::min_element(selected.begin(), selected.end())
                                    : *std::max_element(selected.begin(), selected.end());
    auto deltaSemitones = raiseLowest ? 12 : -12;
    auto rawNewPitch = targetPitch + deltaSemitones;
    if (rawNewPitch < 0 || rawNewPitch > 127)
        return; // out of MIDI range -- same hard guard as adjustNotePitch()

    StepEditGuard undoGuard(*this);

    auto& step = steps[(size_t) ownerIndex];
    for (auto& note : step.notes)
    {
        if (note.pitch == targetPitch)
        {
            note.pitch = rawNewPitch;
            break; // only the one extreme note moves -- the rest of the chord stays put
        }
    }

    // Keep the moved note tracked in the selection, same bookkeeping
    // adjustNotePitch() does after its own shift.
    if (noteSelectionAnchorStep == cursorStepIndex)
    {
        for (auto& pitch : noteSelectionPitches)
            if (pitch == targetPitch) { pitch = rawNewPitch; break; }
        if (noteSelectionFocusPitch == targetPitch)
            noteSelectionFocusPitch = rawNewPitch;
    }

    stepGrid.centerPitchView(rawNewPitch);
    refreshChildViews();
    auditionNoteAtCursor();
}

std::vector<int> MainEditorComponent::effectiveSelectedPitches() const
{
    auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
    auto ownerIndex = findOwningNoteStepIndex(steps, cursorStepIndex);
    if (ownerIndex < 0)
        return {}; // rest -- nothing to select

    std::vector<int> chordPitches;
    for (auto& note : steps[(size_t) ownerIndex].notes)
        chordPitches.push_back(note.pitch);

    if (noteSelectionAnchorStep == cursorStepIndex)
    {
        std::vector<int> intersected;
        for (auto pitch : noteSelectionPitches)
            if (std::find(chordPitches.begin(), chordPitches.end(), pitch) != chordPitches.end())
                intersected.push_back(pitch);
        if (!intersected.empty())
            return intersected;
    }

    return chordPitches; // fresh/stale selection, or nothing survived the intersection -- default to the whole chord
}

void MainEditorComponent::navigateNoteSelection(int delta, bool extend)
{
    auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
    auto ownerIndex = findOwningNoteStepIndex(steps, cursorStepIndex);
    if (ownerIndex < 0)
        return; // rest -- nothing to select

    std::vector<int> pitches;
    for (auto& note : steps[(size_t) ownerIndex].notes)
        pitches.push_back(note.pitch);
    if (pitches.empty())
        return;
    std::sort(pitches.begin(), pitches.end(), [](int a, int b) { return a > b; }); // descending -- index 0 = highest

    auto freshStart = (noteSelectionAnchorStep != cursorStepIndex || noteSelectionFocusPitch < 0);

    int newFocus;
    if (freshStart)
    {
        newFocus = pitches.front(); // highest -- first press from a fresh/stale state always starts here, either key
    }
    else
    {
        auto it = std::find(pitches.begin(), pitches.end(), noteSelectionFocusPitch);
        auto index = (it != pitches.end()) ? (int) std::distance(pitches.begin(), it) : 0;
        auto count = (int) pitches.size();
        index = ((index + delta) % count + count) % count; // circular wrap in both directions
        newFocus = pitches[(size_t) index];
    }

    if (extend && !freshStart)
    {
        if (std::find(noteSelectionPitches.begin(), noteSelectionPitches.end(), newFocus) == noteSelectionPitches.end())
            noteSelectionPitches.push_back(newFocus);
    }
    else
    {
        noteSelectionPitches = { newFocus }; // replace -- also covers a fresh start under Shift (begins from just the highest note)
    }

    noteSelectionAnchorStep = cursorStepIndex;
    noteSelectionFocusPitch = newFocus;

    refreshChildViews();
    auditionNoteAtCursor(); // audible confirmation of the newly-focused pitch
}

void MainEditorComponent::extendNoteSelection(int direction)
{
    // On the very first Shift+D/F press of a gesture, seed the selection
    // with the note the cursor was already sitting on -- previously only
    // the note the cursor moved TO got added, so the note where Shift
    // started was silently left out of every extend.
    // Looked up BEFORE the
    // move using its own reference, since moveCursorByNoteOrStep() below can
    // grow clip.steps (invalidating any reference taken before it).
    if (multiSelectedNoteStarts.empty())
    {
        auto& stepsBeforeMove = project.tracks[(size_t) cursorTrackIndex].clip.steps;
        auto startOwnerIndex = findOwningNoteStepIndex(stepsBeforeMove, cursorStepIndex);
        if (startOwnerIndex >= 0)
            multiSelectedNoteStarts.push_back(startOwnerIndex);
    }

    moveCursorByNoteOrStep(direction, true);

    auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
    auto ownerIndex = findOwningNoteStepIndex(steps, cursorStepIndex);
    if (ownerIndex >= 0 && std::find(multiSelectedNoteStarts.begin(), multiSelectedNoteStarts.end(), ownerIndex) == multiSelectedNoteStarts.end())
        multiSelectedNoteStarts.push_back(ownerIndex);

    refreshChildViews();
}

void MainEditorComponent::selectAllNotesInCurrentTrack()
{
    multiSelectedNoteStarts.clear();
    auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
    for (int i = 0; i < (int) steps.size(); ++i)
        if (!steps[(size_t) i].tiedFromPrevious && !steps[(size_t) i].notes.empty())
            multiSelectedNoteStarts.push_back(i);
    refreshChildViews();
}

std::vector<int> MainEditorComponent::effectiveSelectedNoteStarts() const
{
    if (!multiSelectedNoteStarts.empty())
        return multiSelectedNoteStarts;

    auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
    auto ownerIndex = findOwningNoteStepIndex(steps, cursorStepIndex);
    return ownerIndex >= 0 ? std::vector<int>{ ownerIndex } : std::vector<int>{};
}

void MainEditorComponent::moveNoteTo(int fromIndex, int toIndex)
{
    if (fromIndex == toIndex)
        return;

    auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
    if (fromIndex < 0 || fromIndex >= (int) steps.size()
        || steps[(size_t) fromIndex].tiedFromPrevious || steps[(size_t) fromIndex].notes.empty())
        return; // not a genuine note-start (already moved/merged away by an earlier call) -- nothing to do

    auto length = noteTotalLengthInSteps(steps, fromIndex);
    auto movedRoot = steps[(size_t) fromIndex];
    std::vector<Step> tieChain(steps.begin() + fromIndex + 1, steps.begin() + fromIndex + length);

    // Vacate the old slot(s) -- becomes rest.
    for (int i = 0; i < length; ++i)
        steps[(size_t) (fromIndex + i)] = Step{};

    toIndex = juce::jmax(0, toIndex);
    ensureStepExists(cursorTrackIndex, toIndex + length - 1);
    auto& steps2 = project.tracks[(size_t) cursorTrackIndex].clip.steps; // ensureStepExists may have reallocated

    auto existingOwner = findOwningNoteStepIndex(steps2, toIndex);
    // Only merge when landing EXACTLY on another note's own head
    // (existingOwner == toIndex) -- landing inside an EARLIER note's tied
    // continuation (existingOwner >= 0 but != toIndex) used to merge here
    // too, which silently relocated the moved note all the way back to
    // that earlier note's own start instead of its actual computed target,
    // e.g. quantizing a note to a grid line that happened to fall within a
    // longer, unrelated note's sustain silently folded it into that note's
    // chord instead of landing independently. Exactly the same bug
    // commitPendingNoteAt() already had fixed for live input -- this is
    // the same fix, applied here too. That case now falls through to the fresh-write
    // branch below, same as landing on a genuine rest.
    if (existingOwner == toIndex)
    {
        // Something else already starts EXACTLY here -- merge just the
        // pitches into it as a chord, same collision convention
        // commitPendingNoteAt() uses. The moved note's own tie chain/
        // quantizedFromStep is discarded here (it's now part of a
        // different note's timing) -- an accepted edge case, same class
        // as the tied-continuation "garbage" precedent noted elsewhere in
        // this file. Skips a pitch the target chord already has -- two
        // StepNotes at the identical pitch is exactly the "duplicate note"
        // condition adjustNotePitch() has to specially detect and collapse
        // (see its own comment), and quantizing/unquantizing several notes
        // onto the same target step was one of the ways such a duplicate
        // could get created in the first place (not created here, but
        // silently allowed to happen, traced back to this merge never having
        // checked for it).
        auto& targetNotes = steps2[(size_t) existingOwner].notes;
        for (auto& n : movedRoot.notes)
        {
            auto alreadyPresent = std::any_of(targetNotes.begin(), targetNotes.end(),
                [&](const StepNote& other) { return other.pitch == n.pitch; });
            if (!alreadyPresent)
                targetNotes.push_back(n);
        }
    }
    else
    {
        steps2[(size_t) toIndex] = movedRoot;
        // Extend the relocated tie chain only as far as it doesn't run into
        // something else already sitting there -- same truncate-on-collision
        // safety commitPendingNoteAt() already has for a fresh write.
        // Previously this unconditionally overwrote every step in the
        // chain's span regardless of what was already in it, silently
        // destroying whatever unrelated note (or another note's tied
        // continuation) happened to already be there.
        for (int i = 0; i < (int) tieChain.size(); ++i)
        {
            auto idx = toIndex + 1 + i;
            if (idx < (int) steps2.size() && (!steps2[(size_t) idx].notes.empty() || steps2[(size_t) idx].tiedFromPrevious))
                break;

            steps2[(size_t) idx] = tieChain[(size_t) i];
        }
    }
}

void MainEditorComponent::quantizeSelectedNotes(int gridSteps)
{
    StepEditGuard undoGuard(*this);
    quantizeSelectedNotesImpl(gridSteps);
}

void MainEditorComponent::quantizeSelectedNotesImpl(int gridSteps)
{
    lastQuantizeGridSteps = gridSteps; // remembered for auto-quantize-on-record -- see its declaration

    if (quantizeTripletMode)
        gridSteps = gridSteps * 2 / 3; // exact -- base grid is 12 steps/quarter, see commitDurationPresets' comment
    if (gridSteps <= 0)
        return;

    // Descending order so a note that's about to move out of the way never
    // gets processed AFTER something that already moved into its old slot
    // (moveNoteTo() itself is index-stable -- overwriting a vector element
    // in place doesn't shift anyone else's index -- but processing target
    // steps from the back avoids a moved note being picked up a second
    // time if two selected notes' original positions are adjacent).
    auto targets = effectiveSelectedNoteStarts();
    std::sort(targets.rbegin(), targets.rend());

    for (auto rawIndex : targets)
    {
        auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
        if (rawIndex < 0 || rawIndex >= (int) steps.size()
            || steps[(size_t) rawIndex].tiedFromPrevious || steps[(size_t) rawIndex].notes.empty())
            continue; // no longer a genuine note-start (already moved/merged away above)

        // If this note was already quantized before (re-quantizing to a
        // different grid or amount), always measure from the TRUE original
        // raw (as-played) position, never from wherever a previous quantize
        // pass already rounded it to -- otherwise quantizing to 1/8 and then
        // re-quantizing to 1/16 would snap the already-rounded 1/8 position
        // instead of the real original timing. This is
        // also what lets '5' restore the true as-played timing no matter
        // how many times it's been re-quantized in between.
        auto originalStep = steps[(size_t) rawIndex].quantizedFromStep >= 0
            ? steps[(size_t) rawIndex].quantizedFromStep : rawIndex;

        auto fullyQuantizedIndex = juce::jmax(0, ((originalStep + gridSteps / 2) / gridSteps) * gridSteps);

        // quantizeAmountPercent < 100 pulls the note only partway from its
        // ORIGINAL position to the grid line instead of snapping fully onto
        // it (rounded to the nearest whole step, since step positions are
        // integers) -- 100% (the default) reduces to the exact old
        // behavior, targetIndex == fullyQuantizedIndex.
        auto targetIndex = originalStep + (int) std::round((fullyQuantizedIndex - originalStep) * (quantizeAmountPercent / 100.0));

        steps[(size_t) rawIndex].quantizedFromStep = originalStep;
        if (targetIndex != rawIndex)
        {
            // Checked BEFORE the move -- once moveNoteTo() below vacates
            // rawIndex's span, findOwningNoteStepIndex() would no longer
            // find anything there to compare against.
            auto cursorFollows = findOwningNoteStepIndex(steps, cursorStepIndex) == rawIndex;
            moveNoteTo(rawIndex, targetIndex);
            updateSelectionAfterNoteMove(rawIndex, targetIndex, cursorFollows);
        }
    }

    refreshChildViews();
}

void MainEditorComponent::unquantizeSelectedNotes()
{
    StepEditGuard undoGuard(*this);

    auto targets = effectiveSelectedNoteStarts();
    std::sort(targets.rbegin(), targets.rend());

    for (auto index : targets)
    {
        auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
        if (index < 0 || index >= (int) steps.size()
            || steps[(size_t) index].tiedFromPrevious || steps[(size_t) index].notes.empty())
            continue;

        auto originalStep = steps[(size_t) index].quantizedFromStep;
        if (originalStep < 0)
            continue; // never quantized

        steps[(size_t) index].quantizedFromStep = -1;
        if (originalStep != index)
        {
            // Checked BEFORE the move, same reasoning as
            // quantizeSelectedNotes() above.
            auto cursorFollows = findOwningNoteStepIndex(steps, cursorStepIndex) == index;
            moveNoteTo(index, originalStep);
            updateSelectionAfterNoteMove(index, originalStep, cursorFollows);
        }
    }

    refreshChildViews();
}

void MainEditorComponent::nudgeSelectedNotes(int direction)
{
    StepEditGuard undoGuard(*this);

    auto targets = effectiveSelectedNoteStarts();
    // Same collision-safe processing order reasoning as
    // quantizeSelectedNotesImpl() -- moving right must process the
    // rightmost note first so it never lands on a not-yet-moved sibling's
    // still-occupied slot (and symmetrically for moving left).
    if (direction > 0)
        std::sort(targets.rbegin(), targets.rend());
    else
        std::sort(targets.begin(), targets.end());

    for (auto rawIndex : targets)
    {
        auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
        if (rawIndex < 0 || rawIndex >= (int) steps.size()
            || steps[(size_t) rawIndex].tiedFromPrevious || steps[(size_t) rawIndex].notes.empty())
            continue; // no longer a genuine note-start (already moved/merged away above)

        auto targetIndex = juce::jmax(0, rawIndex + direction);
        if (targetIndex == rawIndex)
            continue;

        auto cursorFollows = findOwningNoteStepIndex(steps, cursorStepIndex) == rawIndex;
        moveNoteTo(rawIndex, targetIndex);
        updateSelectionAfterNoteMove(rawIndex, targetIndex, cursorFollows);
    }

    refreshChildViews();
}

void MainEditorComponent::updateSelectionAfterNoteMove(int fromIndex, int toIndex, bool cursorWasOnFromIndex)
{
    for (auto& start : multiSelectedNoteStarts)
        if (start == fromIndex)
            start = toIndex;

    if (cursorWasOnFromIndex)
        cursorStepIndex = toIndex;
}

void MainEditorComponent::toggleQuantizeTripletMode()
{
    quantizeTripletMode = !quantizeTripletMode;
    refreshChildViews();
}

void MainEditorComponent::cycleQuantizeAmount()
{
    quantizeAmountPercent = quantizeAmountPercent >= 100 ? 25 : quantizeAmountPercent + 25;
    refreshChildViews();
}

void MainEditorComponent::toggleAutoQuantizeOnRecord()
{
    autoQuantizeOnRecordEnabled = !autoQuantizeOnRecordEnabled;
    refreshChildViews();
}

void MainEditorComponent::setNoteRepeatRate(int gridSteps)
{
    if (noteRepeatEnabled && noteRepeatGridSteps == gridSteps)
        noteRepeatEnabled = false; // pressing the already-active rate again is its own off-switch
    else
    {
        noteRepeatGridSteps = gridSteps;
        noteRepeatEnabled = true;
    }
    // Resync -- don't fire immediately off a stale schedule from before
    // this change.
    noteRepeatNextTriggerMs = 0.0;
    lastNoteRepeatStepBucket = -1;
    refreshChildViews();
}

void MainEditorComponent::toggleNoteRepeatTripletMode()
{
    noteRepeatTripletMode = !noteRepeatTripletMode;
    noteRepeatNextTriggerMs = 0.0;
    lastNoteRepeatStepBucket = -1;
    refreshChildViews();
}

void MainEditorComponent::updateNoteRepeat()
{
    if (!noteRepeatEnabled || heldMidiNotes.empty())
    {
        noteRepeatNextTriggerMs = 0.0;
        lastNoteRepeatStepBucket = -1;
        return;
    }

    auto intervalSteps = noteRepeatGridSteps;
    if (noteRepeatTripletMode)
        intervalSteps = intervalSteps * 2 / 3;
    if (intervalSteps <= 0)
        return;

    bool shouldFire = false;

    if (playbackEngine.isPlaying())
    {
        // Locked to the transport's own sample-accurate step position
        // instead of wall-clock timing from whenever the key happened to
        // be pressed, so every repeat lands exactly on the beat grid, in
        // sync with the rest of the song/tempo (and follows a live tempo
        // change instantly, since it's driven by the transport's own
        // already-tempo-aware step advancement) rather than an
        // independently-phased, merely tempo-RATED timer.
        auto currentStep = playbackEngine.getTrackPlaybackStep(cursorTrackIndex);
        if (currentStep >= 0)
        {
            auto bucket = currentStep / intervalSteps;
            if (bucket != lastNoteRepeatStepBucket)
            {
                if (lastNoteRepeatStepBucket >= 0) // don't fire on the very first tick a hold starts mid-interval -- the hold's own first attack already sounded
                    shouldFire = true;
                lastNoteRepeatStepBucket = bucket;
            }
        }
    }
    else
    {
        // Stopped -- no transport grid to lock to. Falls back to a plain
        // wall-clock interval (still derived from the current tempo/rate)
        // purely so the rate is audible while previewing/practicing before
        // recording; not grid-locked to anything since there's nothing
        // playing to lock to.
        lastNoteRepeatStepBucket = -1;

        auto& clip = project.tracks[(size_t) cursorTrackIndex].clip;
        auto intervalMs = clip.stepDurationSeconds(project.tempoBpm) * intervalSteps * 1000.0;
        if (intervalMs <= 0.0)
            return;

        auto now = juce::Time::getMillisecondCounterHiRes();
        if (noteRepeatNextTriggerMs <= 0.0)
        {
            noteRepeatNextTriggerMs = now + intervalMs; // this tick just starts the clock
        }
        else if (now >= noteRepeatNextTriggerMs)
        {
            noteRepeatNextTriggerMs += intervalMs;
            if (noteRepeatNextTriggerMs < now) // fell far behind (a hitch) -- resync to now instead of bursting to catch up
                noteRepeatNextTriggerMs = now + intervalMs;
            shouldFire = true;
        }
    }

    if (!shouldFire)
        return;

    // Re-fire every currently-held note: a synthetic note-off then note-on
    // per pitch, through the exact same handleMidiNoteChange() path a real
    // repeated key-press would take. The note-off naturally auto-commits
    // (Auto/Realtime REC) or just re-triggers the live preview (Browse/
    // Manual) exactly like tapping the key yourself would, and the
    // following note-on immediately starts the next interval's gesture --
    // reusing the whole existing pipeline instead of a parallel one.
    auto notesToRepeat = heldMidiNotes; // copy -- handleMidiNoteChange() mutates heldMidiNotes as it goes
    for (auto& n : notesToRepeat)
        handleMidiNoteChange(n.pitch, n.velocity, false);
    for (auto& n : notesToRepeat)
        handleMidiNoteChange(n.pitch, n.velocity, true);
}

void MainEditorComponent::copySelectedNotes()
{
    auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
    auto targets = effectiveSelectedNoteStarts();
    if (targets.empty())
        return;

    auto anchor = *std::min_element(targets.begin(), targets.end());

    noteClipboard.clear();
    for (auto stepIndex : targets)
    {
        if (stepIndex < 0 || stepIndex >= (int) steps.size()
            || steps[(size_t) stepIndex].tiedFromPrevious || steps[(size_t) stepIndex].notes.empty())
            continue; // not a genuine note-start -- nothing here to copy

        CopiedNote copy;
        copy.offsetSteps = stepIndex - anchor;
        copy.rootStep = steps[(size_t) stepIndex];
        copy.rootStep.quantizedFromStep = -1; // fresh note once pasted, not tied to the original's quantize history

        auto length = noteTotalLengthInSteps(steps, stepIndex);
        for (int i = 1; i < length; ++i)
            copy.tieContinuation.push_back(steps[(size_t) (stepIndex + i)]);

        noteClipboard.push_back(std::move(copy));
    }
}

void MainEditorComponent::pasteNotesAtCursor()
{
    if (noteClipboard.empty())
        return;

    StepEditGuard undoGuard(*this);

    // Descending by target position, same reasoning as
    // quantizeSelectedNotes() -- keeps behavior deterministic and
    // consistent with this file's other multi-note operations, even though
    // paste WRITES rather than MOVES (nothing here vacates a slot that a
    // later iteration could collide with).
    std::vector<const CopiedNote*> ordered;
    for (auto& copy : noteClipboard)
        ordered.push_back(&copy);
    std::sort(ordered.begin(), ordered.end(), [](const CopiedNote* a, const CopiedNote* b) { return a->offsetSteps > b->offsetSteps; });

    for (auto* copyPtr : ordered)
    {
        auto& copy = *copyPtr;
        auto targetIndex = juce::jmax(0, cursorStepIndex + copy.offsetSteps);

        auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
        auto existingOwner = findOwningNoteStepIndex(steps, targetIndex);
        if (existingOwner >= 0)
        {
            // Something already occupies the target -- merge just the
            // pitches into it as a chord, same collision convention
            // commitPendingNoteAt()/moveNoteTo() use. Skips a pitch already
            // present there, same reasoning as moveNoteTo()'s merge branch.
            auto& ownerNotes = steps[(size_t) existingOwner].notes;
            for (auto& n : copy.rootStep.notes)
            {
                auto alreadyPresent = std::any_of(ownerNotes.begin(), ownerNotes.end(),
                    [&](const StepNote& other) { return other.pitch == n.pitch; });
                if (!alreadyPresent)
                    ownerNotes.push_back(n);
            }
        }
        else
        {
            ensureStepExists(cursorTrackIndex, targetIndex + (int) copy.tieContinuation.size());
            auto& steps2 = project.tracks[(size_t) cursorTrackIndex].clip.steps; // ensureStepExists may have reallocated
            steps2[(size_t) targetIndex] = copy.rootStep;
            for (int i = 0; i < (int) copy.tieContinuation.size(); ++i)
                steps2[(size_t) (targetIndex + 1 + i)] = copy.tieContinuation[(size_t) i];
        }
    }

    refreshChildViews();
}

void MainEditorComponent::tieCurrentStep()
{
    StepEditGuard undoGuard(*this);
    noteSelectionAnchorStep = -1; // see moveCursor()'s comment -- the cursor moves forward below

    auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;

    // Find the note to extend: either the one the cursor is currently ON
    // (e.g. landed there via d/f's note-jump navigation), or -- if the
    // cursor is sitting in the gap right where a note just ended -- the one
    // that finished immediately before the cursor. Either way, extension is
    // always appended at the note's actual current end, not at the cursor
    // position (which could be anywhere within the note, not just its end).
    auto ownerIndex = findOwningNoteStepIndex(steps, cursorStepIndex);
    if (ownerIndex < 0 && cursorStepIndex > 0)
        ownerIndex = findOwningNoteStepIndex(steps, cursorStepIndex - 1);
    if (ownerIndex < 0)
        return; // nothing sounding to extend

    // Extend by a fixed amount -- the currently-selected duration preset
    // (Shift+Z/X), same as commitPendingNote() uses -- NOT the note's own
    // current total length, which would double on every repeated tie press
    // (extend by X, now length is 2X, next tie extends by 2X making it 4X,
    // and so on). Written as a genuine contiguous chain of 1-step tied
    // continuations, same reasoning as commitPendingNote(): a single step with
    // lengthInSteps > 1 leaves intermediate grid slots untouched and
    // unmarked, which silently breaks the chain-walk both playback and
    // rendering rely on.
    auto extendBySteps = commitDurationPresets[(size_t) commitDurationPresetIndex];
    auto noteEnd = ownerIndex + noteTotalLengthInSteps(steps, ownerIndex);

    for (int i = 0; i < extendBySteps; ++i)
    {
        ensureStepExists(cursorTrackIndex, noteEnd + i);
        Step tieStep;
        tieStep.tiedFromPrevious = true;
        project.tracks[(size_t) cursorTrackIndex].clip.steps[(size_t) (noteEnd + i)] = tieStep;
    }

    // Tying explicitly makes this note LONGER -- any per-note measured
    // duration it's carrying (e.g. from a brief tap-and-release when
    // originally entered, see StepNote::durationSteps) would otherwise
    // still cap playback at the OLD, shorter length even after the tie
    // chain above extends the container, making Tie look like it silently
    // does nothing audibly. Clearing it falls
    // back to the tie-chain envelope (now correctly extended) instead.
    for (auto& n : project.tracks[(size_t) cursorTrackIndex].clip.steps[(size_t) ownerIndex].notes)
        n.durationSteps = -1;

    cursorStepIndex = noteEnd + extendBySteps;
    refreshChildViews();
    auditionNoteAtCursor();
}

void MainEditorComponent::shiftOctave(int deltaOctaves)
{
    octaveShiftOctaves = juce::jlimit(-3, 3, octaveShiftOctaves + deltaOctaves);
    refreshChildViews();
}

void MainEditorComponent::adjustTempo(double deltaBpm)
{
    project.tempoBpm = juce::jlimit(20.0, 300.0, project.tempoBpm + deltaBpm);
    refreshChildViews();
}

void MainEditorComponent::toggleLoopEnabled()
{
    project.loopEnabled = !project.loopEnabled;
    refreshChildViews();
}

void MainEditorComponent::setLoopStartHere()
{
    project.loopStartStep = cursorStepIndex;
    refreshChildViews();
}

void MainEditorComponent::setLoopEndHere()
{
    project.loopEndStep = cursorStepIndex;
    refreshChildViews();
}

void MainEditorComponent::setClipEndHere()
{
    // Cursor at 0 sets explicitLengthInSteps back to 0 too -- the same
    // "unset" sentinel, so this doubles as the "clear it" gesture. Pressing
    // again at the position that's ALREADY the clip end also clears it,
    // instead of just re-setting it to the same value -- previously the
    // only way to clear an established marker was moving the cursor all
    // the way back to step 0 first.
    auto& clip = project.tracks[(size_t) cursorTrackIndex].clip;
    clip.explicitLengthInSteps = (cursorStepIndex != 0 && cursorStepIndex == clip.explicitLengthInSteps)
        ? 0
        : cursorStepIndex;
    refreshChildViews();
}

void MainEditorComponent::setRangeSelectionStart()
{
    rangeSelectionStart = cursorStepIndex;
    refreshChildViews();
}

void MainEditorComponent::setRangeSelectionEnd()
{
    rangeSelectionEnd = cursorStepIndex;
    refreshChildViews();
}

void MainEditorComponent::duplicateSelectedRange()
{
    if (rangeSelectionEnd <= rangeSelectionStart)
        return; // no range marked

    StepEditGuard undoGuard(*this);
    noteSelectionAnchorStep = -1; // see moveCursor()'s comment -- the cursor jumps to the new copy below

    // Steps past the end of the array within the marked range are implicit
    // rests (same convention used everywhere else in this file) -- pad the
    // clip out to the range's end first so there's real Step data to copy.
    ensureStepExists(cursorTrackIndex, rangeSelectionEnd - 1);

    auto& clip = project.tracks[(size_t) cursorTrackIndex].clip;
    auto& steps = clip.steps;

    std::vector<Step> rangeCopy(steps.begin() + rangeSelectionStart, steps.begin() + rangeSelectionEnd);
    steps.insert(steps.begin() + rangeSelectionEnd, rangeCopy.begin(), rangeCopy.end());

    // The clip just got longer by the range's length -- keep an explicit
    // length in sync so it doesn't silently fall short of the new content.
    if (clip.explicitLengthInSteps > 0)
        clip.explicitLengthInSteps += (int) rangeCopy.size();

    // Move both the cursor and the range markers onto the new copy, so
    // repeated presses of 'r' chain further copies rightward -- same
    // "duplicate and follow it" convention duplicateClipAtCursor() (Session
    // View) already uses.
    cursorStepIndex = rangeSelectionEnd;
    rangeSelectionStart = rangeSelectionEnd;
    rangeSelectionEnd = rangeSelectionStart + (int) rangeCopy.size();

    refreshChildViews();
}

void MainEditorComponent::toggleMetronome()
{
    project.metronomeEnabled = !project.metronomeEnabled;
    refreshChildViews();
}

void MainEditorComponent::toggleCountIn()
{
    project.countInEnabled = !project.countInEnabled;
    refreshChildViews();
}

void MainEditorComponent::toggleViewMode()
{
    if (currentViewMode == ViewMode::PianoRoll)
    {
        currentViewMode = ViewMode::Session;
        resized(); // swap which central component (stepGrid vs. sessionGrid) is laid out/visible
        refreshChildViews();
        return;
    }

    // Session -> Piano Roll: always go in through a specific, linked slot
    // (creating a fresh one at the cursor if it's empty) rather than just
    // revealing whatever the editing buffer happens to hold -- same as
    // pressing 't' on the slot at the cursor, so 's' and 't' agree on what
    // "enter the piano roll" means.
    loadSlotAtCursorToEditor();
}

void MainEditorComponent::moveSessionCursor(int deltaSlots)
{
    sessionCursorSlotIndex = juce::jmax(0, sessionCursorSlotIndex + deltaSlots);
    refreshChildViews();
}

void MainEditorComponent::launchSlotAtCursor()
{
    auto& track = project.tracks[(size_t) cursorTrackIndex];
    if (sessionCursorSlotIndex < 0 || sessionCursorSlotIndex >= (int) track.sceneClips.size())
        return; // nothing captured in this slot yet -- nothing to launch

    track.playingSlotIndex = sessionCursorSlotIndex;
    playbackEngine.retriggerTrack(cursorTrackIndex);
    refreshChildViews();
}

void MainEditorComponent::stopCurrentTrackSlot()
{
    auto& track = project.tracks[(size_t) cursorTrackIndex];
    track.playingSlotIndex = -2;
    playbackEngine.retriggerTrack(cursorTrackIndex);
    refreshChildViews();
}

void MainEditorComponent::captureClipToSlotAtCursor()
{
    auto& track = project.tracks[(size_t) cursorTrackIndex];
    while ((int) track.sceneClips.size() <= sessionCursorSlotIndex)
        track.sceneClips.push_back(MidiClip{});

    track.sceneClips[(size_t) sessionCursorSlotIndex] = track.clip;
    // Link: from here on, further edits to `clip` auto-sync back to this
    // slot (see refreshChildViews()) instead of needing 'g' again.
    track.editingSlotIndex = sessionCursorSlotIndex;
    refreshChildViews();
}

void MainEditorComponent::loadSlotAtCursorToEditor()
{
    auto& track = project.tracks[(size_t) cursorTrackIndex];
    if (sessionCursorSlotIndex < 0)
        return; // shouldn't happen -- moveSessionCursor() clamps to >= 0

    // No clip at this slot yet -- create a fresh, empty one so there's
    // always something to open and start writing into (matching how
    // captureClipToSlotAtCursor() already grows sceneClips to fit),
    // instead of silently doing nothing on an empty slot.
    while ((int) track.sceneClips.size() <= sessionCursorSlotIndex)
        track.sceneClips.push_back(MidiClip{});

    track.clip = track.sceneClips[(size_t) sessionCursorSlotIndex];
    track.editingSlotIndex = sessionCursorSlotIndex; // live-link, same as captureClipToSlotAtCursor()
    cursorStepIndex = 0;
    noteSelectionAnchorStep = -1; // see moveCursor()'s comment -- an entirely different clip just loaded in
    currentViewMode = ViewMode::PianoRoll;
    resized();
    refreshChildViews();
}

void MainEditorComponent::deleteClipAtCursor()
{
    auto& track = project.tracks[(size_t) cursorTrackIndex];
    if (sessionCursorSlotIndex < 0 || sessionCursorSlotIndex >= (int) track.sceneClips.size())
        return; // nothing there to delete

    if (track.playingSlotIndex == sessionCursorSlotIndex)
    {
        track.playingSlotIndex = -2;
        playbackEngine.retriggerTrack(cursorTrackIndex);
    }

    if (track.editingSlotIndex == sessionCursorSlotIndex)
        track.editingSlotIndex = -1; // unlink -- otherwise the next edit's sync would just resurrect this slot

    track.sceneClips[(size_t) sessionCursorSlotIndex] = MidiClip{};
    refreshChildViews();
}

void MainEditorComponent::duplicateClipAtCursor()
{
    auto& track = project.tracks[(size_t) cursorTrackIndex];
    if (sessionCursorSlotIndex < 0 || sessionCursorSlotIndex >= (int) track.sceneClips.size())
        return; // nothing at the cursor to duplicate

    auto sourceClip = track.sceneClips[(size_t) sessionCursorSlotIndex];
    auto destSlotIndex = sessionCursorSlotIndex + 1;
    while ((int) track.sceneClips.size() <= destSlotIndex)
        track.sceneClips.push_back(MidiClip{});

    track.sceneClips[(size_t) destSlotIndex] = sourceClip;
    sessionCursorSlotIndex = destSlotIndex; // follow the copy, so repeated presses chain rightward
    refreshChildViews();
}

void MainEditorComponent::toggleChordEstimateForCurrentTrack()
{
    auto& track = project.tracks[(size_t) cursorTrackIndex];
    track.includeInChordEstimate = !track.includeInChordEstimate;
    refreshChildViews();
}

int MainEditorComponent::shiftedPendingPitch(int rawPitch) const
{
    return juce::jlimit(0, 127, rawPitch + octaveShiftOctaves * 12);
}

void MainEditorComponent::scrollStepGridPitch(int deltaSemitones)
{
    stepGrid.scrollPitchView(deltaSemitones);
}

void MainEditorComponent::zoomStepGridHorizontal(float factor)
{
    stepGrid.zoomHorizontal(factor);
}

void MainEditorComponent::zoomStepGridVertical(float factor)
{
    // Targets the note grid's pitch rows or the automation lanes'
    // heights, never both -- see StepGridComponent::zoomVerticalNoteRows/
    // zoomVerticalAutomationLanes's declarations. Follows automation edit
    // mode the same way d/f/t/g's meanings already do, rather than
    // introducing a whole separate set of keys just for this.
    if (automationEditModeActive)
        stepGrid.zoomVerticalAutomationLanes(factor);
    else
        stepGrid.zoomVerticalNoteRows(factor);
}

void MainEditorComponent::cycleCommitDuration(int delta)
{
    auto numPresets = (int) (sizeof(commitDurationPresets) / sizeof(commitDurationPresets[0]));
    commitDurationPresetIndex = juce::jlimit(0, numPresets - 1, commitDurationPresetIndex + delta);
    refreshChildViews();
}

void MainEditorComponent::commitPendingNoteAt(int targetStep, int fallbackDurationSteps, const std::vector<StepNote>& notes)
{
    if (notes.empty())
        return; // nothing pending to commit

    // Every successful commit resets the idle countdown (see
    // pendingChordIdleSinceMs's declaration) -- keeps pendingChord alive
    // indefinitely as long as it keeps getting re-committed (e.g. 'f'
    // pressed repeatedly to repeat the same chord), only actually expiring
    // after a full pendingChordTimeoutMs with no commit at all.
    pendingChordIdleSinceMs = juce::Time::getMillisecondCounterHiRes();
    stepGrid.setPreviewAlpha(1.0f); // cancel any fade-out already in progress

    // The tie chain is an editing/visual envelope, not a per-note duration
    // -- sized to whichever note in `notes` is longest (or
    // fallbackDurationSteps, for a note that never got an individually
    // measured length, e.g. a manual 'f' commit while notes are still
    // held). Each note's OWN sounding length -- what actually gets
    // scheduled for playback -- is n.durationSteps if set (see
    // StepNote::durationSteps).
    auto envelopeSteps = juce::jmax(1, fallbackDurationSteps);
    for (auto& n : notes)
        if (n.durationSteps > 0)
            envelopeSteps = juce::jmax(envelopeSteps, n.durationSteps);

    auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
    auto ownerIndex = findOwningNoteStepIndex(steps, targetStep);

    if (ownerIndex == targetStep)
    {
        // An existing note already starts EXACTLY here -- merge the
        // pending note(s) into it as a chord instead of overwriting/
        // deleting what was already there. Skips a pitch already present
        // there, same reasoning as moveNoteTo()'s merge branch -- a
        // duplicate-pitch StepNote pair silently created here (e.g. two
        // real-time-REC'd notes an octave-plus-remainder apart landing on
        // the same shifted pitch) is exactly what later made
        // adjustNotePitch() collapse a note that looked fine in the piano
        // roll (two identical-pitch notes render as one overlapping block)
        // the next time the chord was semitone-shifted. Deliberately NOT
        // triggered by landing inside an EARLIER note's tied continuation
        // (ownerIndex >= 0 but != targetStep) -- that used to merge here
        // too, which silently relocated a real-time-REC'd note back to the
        // earlier note's own start instead of where it was actually
        // played, making it look like the new pitch couldn't be placed at
        // all while a long note was still sounding.
        // That case now falls through to the fresh-write branch below,
        // same as landing on a genuine rest.
        auto& ownerNotes = steps[(size_t) ownerIndex].notes;
        for (auto& n : notes)
        {
            auto shiftedPitch = shiftedPendingPitch(n.pitch);
            auto alreadyPresent = std::any_of(ownerNotes.begin(), ownerNotes.end(),
                [&](const StepNote& other) { return other.pitch == shiftedPitch; });
            if (!alreadyPresent)
                ownerNotes.push_back({ shiftedPitch, n.velocity, clampDurationForPitchConflict(ownerIndex, shiftedPitch, n.durationSteps) });
        }
        return;
    }

    // Nothing here yet (a genuine rest), OR targetStep lands inside an
    // EARLIER note's tied continuation -- either way, write a fresh note
    // starting exactly at targetStep. Overwriting a tied-continuation step
    // this way only truncates that earlier note's own visual/tie-chain
    // envelope at this point; its actual sounding length is unaffected
    // since that lives per-note in StepNote::durationSteps, not the tie
    // chain. Written as a genuine contiguous chain (one note-start step +
    // N-1 explicit tiedFromPrevious continuation steps), never as a single
    // Step with lengthInSteps > 1 -- a single long step left the
    // intermediate grid slots untouched, and if a later action (e.g.
    // tieCurrentStep) wrote a NEW tied step past that gap, the gap itself
    // wasn't marked tiedFromPrevious, which silently broke the contiguous-
    // chain walk both PlaybackEngine::scheduleUpTo and StepGridComponent's
    // rendering rely on -- the note played/drew as if it were only 1 step
    // long.
    ensureStepExists(cursorTrackIndex, targetStep);
    Step noteStep;
    noteStep.notes.reserve(notes.size());
    for (auto& n : notes)
    {
        auto shiftedPitch = shiftedPendingPitch(n.pitch);
        noteStep.notes.push_back({ shiftedPitch, n.velocity, clampDurationForPitchConflict(targetStep, shiftedPitch, n.durationSteps) });
    }
    project.tracks[(size_t) cursorTrackIndex].clip.steps[(size_t) targetStep] = noteStep;

    // Extend the tie chain up to envelopeSteps, but stop the instant it
    // would run into a step that already holds something else -- a
    // different note's own start, or a continuation tied back to an
    // earlier note (targetStep itself was already confirmed to be a rest
    // above, but a DIFFERENT note can easily start a few steps later,
    // especially for real-time-recorded notes whose actual gaps don't line
    // up with the fixed duration preset). Truncating here means the new
    // note simply ends early instead of silently overwriting/deleting
    // whatever was already there.
    for (int i = 1; i < envelopeSteps; ++i)
    {
        auto idx = targetStep + i;
        auto& existingSteps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
        if (idx < (int) existingSteps.size() && (!existingSteps[(size_t) idx].notes.empty() || existingSteps[(size_t) idx].tiedFromPrevious))
            break;

        ensureStepExists(cursorTrackIndex, idx);
        Step tieStep;
        tieStep.tiedFromPrevious = true;
        project.tracks[(size_t) cursorTrackIndex].clip.steps[(size_t) idx] = tieStep;
    }
}

int MainEditorComponent::clampDurationForPitchConflict(int targetStep, int shiftedPitch, int durationSteps) const
{
    if (durationSteps <= 0)
        return durationSteps;

    auto& existingSteps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
    auto limit = juce::jmin((int) existingSteps.size(), targetStep + durationSteps);
    for (int idx = targetStep + 1; idx < limit; ++idx)
        for (auto& existingNote : existingSteps[(size_t) idx].notes)
            if (existingNote.pitch == shiftedPitch)
                return idx - targetStep;

    return durationSteps;
}

void MainEditorComponent::commitPendingNote()
{
    if (pendingChord.empty())
        return; // nothing pending to commit

    auto durationSteps = commitDurationPresets[(size_t) commitDurationPresetIndex];
    commitPendingNoteAt(cursorStepIndex, durationSteps, pendingChord);
    moveCursor(durationSteps);
}

void MainEditorComponent::refreshChildViews()
{
    trimTrailingEmptySteps();

    // Session View: if the current track's piano-roll editing buffer is
    // live-linked to a clip slot (see editingSlotIndex's declaration),
    // keep that slot's stored copy in sync with every edit -- lets a
    // captured/loaded clip be edited directly in the piano roll instead of
    // needing an explicit "capture" (g) after every change.
    auto& currentTrack = project.tracks[(size_t) cursorTrackIndex];
    if (currentTrack.editingSlotIndex >= 0 && currentTrack.editingSlotIndex < (int) currentTrack.sceneClips.size())
        currentTrack.sceneClips[(size_t) currentTrack.editingSlotIndex] = currentTrack.clip;

    juce::StringArray instrumentNames;
    for (int i = 0; i < (int) project.tracks.size(); ++i)
    {
        auto* instrument = playbackEngine.getTrackInstrument(i);
        instrumentNames.add(instrument != nullptr ? instrument->getName() : juce::String());
    }

    trackList.setTracks(project.tracks, cursorTrackIndex, instrumentNames);
    stepGrid.setClip(&project.tracks[(size_t) cursorTrackIndex].clip, cursorStepIndex);
    stepGrid.setLoopRegion(project.loopStartStep, project.loopEndStep, project.loopEnabled);
    stepGrid.setSelectedPitches(effectiveSelectedPitches());
    stepGrid.setSelectedNoteStarts(effectiveSelectedNoteStarts());
    stepGrid.setRangeSelection(rangeSelectionStart, rangeSelectionEnd);
    // Cast relies on StepGridComponent::AutomationLane mirroring this
    // enum's values/order exactly (see its own declaration) -- kept as a
    // separate enum there rather than including this header, same as
    // every other MainEditorComponent-state-mirrored-into-StepGridComponent
    // pattern in this file.
    stepGrid.setAutomationEditMode(automationEditModeActive, (StepGridComponent::AutomationLane) automationEditLane, automationEditParameterLaneIndex);
    // -1 = no preview to draw -- Sustain has no continuous value, and
    // there's nothing pending to show once automation edit mode is off.
    stepGrid.setAutomationPendingValue(!automationEditModeActive ? -1
        : automationEditLane == AutomationLane::PitchBend    ? pitchBendPendingValue
        : automationEditLane == AutomationLane::FilterCutoff ? filterCutoffPendingValue
                                                                : -1);
    stepGrid.setAutomationPendingCurveType(automationEditLane == AutomationLane::FilterCutoff
        ? filterCutoffPendingCurveType : pitchBendPendingCurveType);
    stepGrid.setAutomationPendingCurveAmount(automationEditLane == AutomationLane::FilterCutoff
        ? filterCutoffPendingCurveAmount : pitchBendPendingCurveAmount);
    // -1.0f = no preview -- same sentinel convention as setAutomationPendingValue()
    // above, just float-valued to match ParameterAutomationPoint. Fed by
    // either Cmd+Ctrl+Z/X (keyboard) or a live Touch gesture while stopped
    // (previewTouchedParameterValue()) -- both write the same
    // parameterPendingValue, so the lane shows whichever one happened most
    // recently.
    stepGrid.setParameterAutomationPendingValue(automationEditModeActive && automationEditLane == AutomationLane::Parameter
        ? parameterPendingValue : -1.0f);
    stepGrid.setParameterAutomationPendingCurveType(parameterPendingCurveType);
    stepGrid.setParameterAutomationPendingCurveAmount(parameterPendingCurveAmount);
    // Every OTHER lane (on the current track) with a live touch-preview
    // value of its own -- see touchPreviewValues' declaration. Shown
    // regardless of automationEditModeActive/which lane is selected, so a
    // multi-parameter touch's other points are visible even when they
    // aren't the one Cmd+Ctrl+L happens to have selected right now.
    std::map<int, float> parameterPreviewValuesForCurrentTrack;
    for (auto& [key, value] : touchPreviewValues)
        if (key.first == cursorTrackIndex)
            parameterPreviewValuesForCurrentTrack[key.second] = value;
    stepGrid.setParameterAutomationPreviewValues(parameterPreviewValuesForCurrentTrack);
    stepGrid.setSelectedAutomationSteps(multiSelectedAutomationSteps);

    transportBar.setPlaying(playbackEngine.isPlaying());
    transportBar.setBpm(project.tempoBpm);
    transportBar.setOctaveShift(octaveShiftOctaves);
    transportBar.setVirtualKeyboardVelocity(virtualKeyboardVelocity);
    transportBar.setQuantizeAmountPercent(quantizeAmountPercent);
    transportBar.setQuantizeTripletMode(quantizeTripletMode);
    transportBar.setCountInEnabled(project.countInEnabled);
    transportBar.setRecMode((int) recMode);
    transportBar.setLoopEnabled(project.loopEnabled);
    transportBar.setMetronomeEnabled(project.metronomeEnabled);
    transportBar.setAutoQuantizeOnRecordEnabled(autoQuantizeOnRecordEnabled);
    transportBar.setNoteRepeat(noteRepeatEnabled, noteRepeatGridSteps, noteRepeatTripletMode);
    transportBar.setDrumGridMode(drumGridModeActive);
    transportBar.setAutomationTouchMode(automationTouchModeEnabled);

    updatePendingNoteDisplays(); // also syncs shortcutHelpBar's note-pending state, see its own declaration
    updateStepGridScale();
    updateChordEstimates();
    shortcutHelpBar.setViewMode(currentViewMode == ViewMode::Session);
    shortcutHelpBar.setAutomationEditMode(automationEditModeActive);

    if (currentViewMode == ViewMode::Session)
        sessionGrid.setTracks(project.tracks, cursorTrackIndex, sessionCursorSlotIndex);

    // transportBar's badge content pushed above (REC mode, NOTE: pending
    // chord text, KEY badge appearing/disappearing, etc.) can change how
    // many rows its wrapping layout needs even though the WINDOW itself
    // hasn't been resized -- JUCE only calls resized() on an actual
    // component resize, so this has to be re-run explicitly here to pick
    // up any such change (a no-op, cheap re-layout whenever the row count
    // hasn't actually changed -- Component::setBounds() skips work for
    // unchanged bounds). See TransportBarComponent::getRequiredHeightForWidth()'s
    // declaration.
    resized();
}

void MainEditorComponent::trimTrailingEmptySteps()
{
    for (auto& track : project.tracks)
    {
        auto& steps = track.clip.steps;
        // explicitLengthInSteps (see MidiClip's declaration) is a floor on
        // how far this trims -- 0 (unset) preserves the original "trim all
        // the way down to the last note" behavior.
        auto minSize = track.clip.explicitLengthInSteps;
        while ((int) steps.size() > minSize && !steps.empty() && steps.back().notes.empty() && !steps.back().tiedFromPrevious)
            steps.pop_back();
    }
}

void MainEditorComponent::updateChordEstimates()
{
    auto stepsPerQuarterNote = project.tracks.empty() ? 960 : project.tracks[0].clip.stepsPerQuarterNote;
    auto halfBeatLengthInSteps = juce::jmax(1, stepsPerQuarterNote / 2); // 0.5-beat analysis granularity
    chordEstimateBar.setChords(ChordEstimator::estimate(project, halfBeatLengthInSteps,
        scaleRootPitchClass, scaleIsMinor, currentScaleType != ScaleType::Off));
}

void MainEditorComponent::updatePendingNoteDisplays()
{
    // Preview shows the note(s) as they will actually be written -- i.e.
    // after the same octave shift commitPendingNote() applies -- not the
    // raw detected pitches, so the preview position matches where they'll
    // really land.
    std::vector<int> shiftedPitches;
    for (auto& n : pendingChord)
        shiftedPitches.push_back(shiftedPendingPitch(n.pitch));

    stepGrid.setPreviewNotes(shiftedPitches, commitDurationPresets[(size_t) commitDurationPresetIndex]);
    if (!shiftedPitches.empty())
        stepGrid.centerPitchView(shiftedPitches.front()); // auto-scroll so the pending pitch stays on screen

    transportBar.setPendingNoteStatus(shiftedPitches, commitDurationPresets[(size_t) commitDurationPresetIndex]);
    // Here rather than only in refreshChildViews() -- the ~2s idle
    // auto-clear (see pendingChordIdleSinceMs's declaration) calls this
    // function directly from timerCallback() without going through
    // refreshChildViews(), so putting the shortcut-help update only there
    // would leave the note-operations help block stuck showing after a
    // forgotten chord silently auto-cleared.
    shortcutHelpBar.setNotePending(!pendingChord.empty());
}

void MainEditorComponent::updateStepGridScale()
{
    // Auto: keep scaleRootPitchClass/scaleIsMinor synced to the whole-piece
    // key estimate. hasEnoughData is false only when
    // there are no notes anywhere yet -- leave the previous key in place
    // rather than snapping to a meaningless default in that case.
    if (currentScaleType == ScaleType::Auto)
    {
        auto keyEstimate = KeyEstimator::estimate(project);
        if (keyEstimate.hasEnoughData)
        {
            scaleRootPitchClass = keyEstimate.rootPitchClass;
            scaleIsMinor = keyEstimate.isMinor;
        }
    }

    std::array<bool, 12> inScale {};
    inScale.fill(currentScaleType == ScaleType::Off); // Off = every row treated as "in scale" (no tint difference)

    if (currentScaleType != ScaleType::Off)
    {
        static constexpr int majorIntervals[] = { 0, 2, 4, 5, 7, 9, 11 };
        static constexpr int naturalMinorIntervals[] = { 0, 2, 3, 5, 7, 8, 10 };

        auto* intervals = scaleIsMinor ? naturalMinorIntervals : majorIntervals;
        for (int i = 0; i < 7; ++i)
            inScale[(size_t) ((scaleRootPitchClass + intervals[i]) % 12)] = true;
    }

    stepGrid.setScale(inScale);
    transportBar.setEstimatedKey(scaleRootPitchClass, scaleIsMinor, currentScaleType != ScaleType::Off);
}

void MainEditorComponent::cycleScale()
{
    currentScaleType = currentScaleType == ScaleType::Auto ? ScaleType::Off : ScaleType::Auto;
    updateStepGridScale();
}

bool MainEditorComponent::keyPressed(const juce::KeyPress& key)
{
    // Left-hand-only layout: the right hand stays on the MIDI keyboard the
    // whole time, so every command here lives on the QWERTY left side.
    // d/f (step left/right) let the hand stay put and just rock two fingers
    // sideways -- see handleForwardKey()/handleBackwardKey() for their full
    // Ableton-Live-style behavior (place/delete when there's something to
    // place/delete, navigate otherwise). Plain 3/e scroll the piano-roll's
    // visible pitch range (3 = up, e = down); Option+3/E do semitone pitch
    // nudge on the note at the cursor instead, and Shift+Option+3/E do
    // octave nudge -- see the Option/Shift+Option blocks below. Track prev/
    // next moved to Cmd+R/Cmd+B once Cmd+3/Cmd+E were needed for vertical
    // zoom instead.
    // Every branch below routes through trigger() so the shortcut help bar's
    // "last action" indicator always shows what was just pressed and what
    // it did (e.g. "Cmd+E - Next Track") -- separate from the always-visible
    // static shortcut list, this is live, per-keypress feedback. Also
    // records the raw key code for KeyboardOverlayComponent's "last
    // pressed" highlight -- getKeyCode() (not getTextCharacter()) to match
    // the same uppercase, shift-independent identity every isKeyCode(...)
    // check elsewhere in this function already uses.
    auto trigger = [this, keyCode = key.getKeyCode()](const juce::String& label, const std::function<void()>& action)
    {
        lastPressedKeyCode = keyCode;
        shortcutHelpBar.setLastAction(label);
        action();
        return true;
    };

    // Ctrl is reserved entirely for the virtual-keyboard/drum-grid note
    // input (see pollVirtualKeyboardInput()) -- every other modifier here is
    // Cmd, never Ctrl (see the top-of-file convention notes), EXCEPT
    // Ctrl+Z/X/Shift+Z/X and Ctrl+T/G below (kept on plain Ctrl -- see their
    // own comments). Zoom lives on Cmd+Ctrl (see the isCommandDown() branch
    // below) -- it was briefly on plain Ctrl alone, but moved once Cmd+Ctrl
    // was needed for it instead. The note keys themselves aren't handled
    // here at all -- keyPressed() has no matching "key up" callback, so
    // proper hold/release note-on/off is polled via isKeyCurrentlyDown()
    // instead. Returning true unconditionally for every other Ctrl combo
    // just claims it as handled -- returning false left it "unhandled" as
    // far as JUCE/macOS was concerned, which triggered the OS's system beep.
    if (key.getModifiers().isCtrlDown())
    {
        if (key.getModifiers().isCommandDown())
        {
            // Add Track / Prev Track / Next Track -- displaced off
            // Cmd+Ctrl+T/G/B once those became zoom (later moved on to
            // Cmd+Shift, see the Cmd block below), onto a fully mnemonic
            // Add/Prev/Next set instead of the old arbitrary T/G/B pairing.
            // Add Track itself later moved on again, off Cmd+Ctrl+A once
            // that became the automation-edit-mode toggle below --
            // 'T' for "Track", freely available since T/G/B
            // themselves moved off Cmd+Ctrl to Cmd+Shift long ago.
            if (key.isKeyCode('T'))
                return trigger("Cmd+Ctrl+T - Add Track", [this] { addTrack(); });
            if (key.isKeyCode('P'))
                return trigger("Cmd+Ctrl+P - Prev Track", [this] { switchTrack(-1); });
            if (key.isKeyCode('N'))
                return trigger("Cmd+Ctrl+N - Next Track", [this] { switchTrack(1); });
            // Set Loop End -- displaced off plain Cmd+C once that became
            // note Copy (see the Cmd block below), per the user's request.
            // Shift+C (loop start) stays put -- only Cmd+C needed the
            // note-copy meaning.
            if (key.isKeyCode('C'))
                return trigger("Cmd+Ctrl+C - Set Loop End", [this] { setLoopEndHere(); });
            // Set Clip End -- displaced off plain Cmd+B once that became
            // Select Note Down (Cmd+G/Cmd+B, see the Cmd block below).
            // 'E' for "End", also conveniently
            // free at this tier and unrelated to the Ctrl-block's own
            // "up/down" pair migration.
            if (key.isKeyCode('E') && currentViewMode != ViewMode::Session)
                return trigger("Cmd+Ctrl+E - Set Clip End", [this] { setClipEndHere(); });
            // Toggle Loop -- displaced off plain 'b' once that became the
            // duration-move pair's retreat/advance half's neighbor... no,
            // see switch(c) below for the full story of how it ended up
            // needing to relocate again once plain 'b' was needed for
            // Pitch/Automation-Value Down instead.
            if (key.isKeyCode('B'))
                return trigger("Cmd+Ctrl+B - Toggle Loop", [this] { toggleLoopEnabled(); });
            // Range start/end markers -- displaced here as a pair since
            // plain Cmd+5/Cmd+R are needed for the chord-voicing octave
            // shift instead (see the Cmd block below).
            if (key.isKeyCode('5'))
                return trigger("Cmd+Ctrl+5 - Set Range Start", [this] { setRangeSelectionStart(); });
            if (key.isKeyCode('R'))
                return trigger("Cmd+Ctrl+R - Set Range End", [this] { setRangeSelectionEnd(); });
            // Toggle the current track's inclusion in ChordEstimator's
            // pooled analysis -- displaced off plain Cmd+A once that became
            // Select All Notes (see the Cmd block below, standard OS
            // convention). 'H' for
            // "Harmony" (its closest available mnemonic once 'A' and 'C'
            // were both already spoken for).
            if (key.isKeyCode('H'))
                return trigger("Cmd+Ctrl+H - Toggle Chord Track", [this] { toggleChordEstimateForCurrentTrack(); });

            // Automation editing (sustain / pitch bend / filter cutoff) --
            // see AutomationLane's declaration. 'A' for "Automation" (freed
            // up by Add Track's move to 'T' above).
            if (key.isKeyCode('A'))
                return trigger("Cmd+Ctrl+A - Toggle Automation Edit Mode", [this] { toggleAutomationEditMode(); });
            if (key.isKeyCode('L'))
                return trigger("Cmd+Ctrl+L - Cycle Automation Lane", [this] { cycleAutomationLane(); });
            // 'W' for "Write" (the mainstream DAW term this most resembles,
            // even though the actual behavior is Touch -- see
            // toggleAutomationTouchMode()'s declaration and the transport
            // bar badge for the precise distinction).
            if (key.isKeyCode('W'))
                return trigger("Cmd+Ctrl+W - Toggle Automation Touch Mode", [this] { toggleAutomationTouchMode(); });
            if (key.isKeyCode('S'))
                return trigger("Cmd+Ctrl+S - Toggle Sustain Point", [this] { toggleSustainEventAtCursor(); });
            if (key.isKeyCode('I'))
                return trigger("Cmd+Ctrl+I - Insert Automation Point", [this] { insertAutomationPointAtCursor(); });
            if (key.isKeyCode('D'))
                return trigger("Cmd+Ctrl+D - Delete Automation Point", [this] { deleteAutomationPointAtCursor(); });
            // 'V' for cur[V]e -- 'C' was already spoken for (Set Loop End,
            // above).
            if (key.isKeyCode('V'))
                return trigger("Cmd+Ctrl+V - Toggle Automation Curve Type", [this] { cycleAutomationCurveTypeAtCursor(); });
            // Fine/coarse curve-AMOUNT adjust -- same "Z=down, X=up"
            // convention used everywhere else in this app (octave, tempo,
            // velocity, quantize amount, automation value on t/g). This
            // tier used to just duplicate t/g's own value-adjust (it was
            // literally documented as "same as t/g, alternate binding");
            // repurposed for curveAmount once that became a continuous
            // parameter needing its own input, since t/g were already
            // spoken for by value and the ease-in/ease-out slope needed a
            // way to be adjusted independently. Checked before the
            // Ctrl+Shift-only Z/X bindings below since Shift can be held
            // alongside these too.
            if (key.getModifiers().isShiftDown() && key.isKeyCode('Z'))
                return trigger("Cmd+Ctrl+Shift+Z - Curve Amount Down (Coarse)", [this] { adjustAutomationPendingCurveAmount(-1, true); });
            if (key.getModifiers().isShiftDown() && key.isKeyCode('X'))
                return trigger("Cmd+Ctrl+Shift+X - Curve Amount Up (Coarse)", [this] { adjustAutomationPendingCurveAmount(1, true); });
            if (key.isKeyCode('Z'))
                return trigger("Cmd+Ctrl+Z - Curve Amount Down", [this] { adjustAutomationPendingCurveAmount(-1, false); });
            if (key.isKeyCode('X'))
                return trigger("Cmd+Ctrl+X - Curve Amount Up", [this] { adjustAutomationPendingCurveAmount(1, false); });
        }
        else if (key.getModifiers().isShiftDown())
        {
            if (key.isKeyCode('Z'))
                return trigger("Ctrl+Shift+Z - Velocity Down", [this] { adjustVirtualKeyboardVelocity(-0.1f); });
            if (key.isKeyCode('X'))
                return trigger("Ctrl+Shift+X - Velocity Up", [this] { adjustVirtualKeyboardVelocity(0.1f); });
        }
        else
        {
            // Tie / Jump Forward 1 Bar -- displaced off plain T/G once those
            // became the in-chord note-selection keys (see the switch(c)
            // block below), which the user asked for directly.
            // Every plain left-hand key is already spoken for elsewhere in
            // this file, so these two landed here instead -- Piano Roll
            // only, matching their original scope (Session View has no tie/
            // bar-jump concept).
            if (key.isKeyCode('T') && currentViewMode != ViewMode::Session)
                return trigger("Ctrl+T - Tie", [this] { tieCurrentStep(); });
            if (key.isKeyCode('G') && currentViewMode != ViewMode::Session)
                return trigger("Ctrl+G - Jump Forward 1 Bar", [this]
                {
                    moveCursor(4 * project.tracks[(size_t) cursorTrackIndex].clip.stepsPerQuarterNote);
                });
            if (key.isKeyCode('Z'))
                return trigger("Ctrl+Z - Transpose Down", [this] { adjustVirtualKeyboardTranspose(-1); });
            if (key.isKeyCode('X'))
                return trigger("Ctrl+X - Transpose Up", [this] { adjustVirtualKeyboardTranspose(1); });
            // Commits a pending chord at the cursor -- moved here off
            // Cmd+F, which turned out to be the wrong key by mistake, then
            // off Ctrl+F itself onto Ctrl+V. Piano
            // Roll only, same scope as Ctrl+T/G above. Automation edit mode
            // redefines this same physical key as "commit" too, just for a
            // point instead of a note -- same muscle memory either way,
            // moved along with Commit so the two stay on the same key.
            // Cmd+Ctrl+I stays as the alternate binding (see
            // cycleAutomationLane()'s Cmd+Ctrl+L, same "keep the old one
            // too" precedent).
            if (key.isKeyCode('V') && currentViewMode != ViewMode::Session && automationEditModeActive)
                return trigger("Ctrl+V - Insert Automation Point", [this] { insertAutomationPointAtCursor(); });
            if (key.isKeyCode('V') && currentViewMode != ViewMode::Session)
                return trigger("Ctrl+V - Commit", [this] { commitPendingNoteManually(); });
        }
        return true;
    }

    if (key.getModifiers().isCommandDown())
    {
        // Checked before the plain Cmd+G/Cmd+B pitch-nudge binding below --
        // piano-roll pitch-view scroll moved here off plain 3/e once those
        // became the individual-note selection navigation (see
        // navigateNoteSelection()). G/B (not 3/E, and not T/G anymore
        // either) -- moved off the digit pair entirely because '3'/'e'
        // (and their Shift/Option-modified forms) have repeatedly hit
        // macOS-level interception in this app (Shift+digit never
        // arriving, Option+E being a dead key, Cmd+Shift+3 being reserved
        // for screenshots) -- G/B are a plain letter pair with no such
        // history anywhere in this file (see the Option+G/B comment
        // below), so every "up means G, down means B" pair in this file
        // now uses them instead of 3/E/W/R/T at whichever modifier tier
        // it lives on. Originally lived on T/G, moved one more letter over
        // once T/G's own physical position turned out not to be the most
        // comfortable spot either.
        if (key.getModifiers().isAltDown() && key.isKeyCode('G'))
            return trigger("Cmd+Option+G - Scroll Pitch Up", [this] { scrollStepGridPitch(1); });
        if (key.getModifiers().isAltDown() && key.isKeyCode('B'))
            return trigger("Cmd+Option+B - Scroll Pitch Down", [this] { scrollStepGridPitch(-1); });
        // Zoom -- moved here off Cmd+Ctrl at the user's request, swapping
        // places with Octave Up/Down (moved to
        // Cmd+Ctrl+T/G, see the Ctrl block above -- Cmd+Shift+D/F were free,
        // no swap needed for those two).
        if (key.getModifiers().isShiftDown() && key.isKeyCode('G'))
            return trigger("Cmd+Shift+G - Zoom In (Vertical)", [this] { zoomStepGridVertical(0.8f); });
        if (key.getModifiers().isShiftDown() && key.isKeyCode('B'))
            return trigger("Cmd+Shift+B - Zoom Out (Vertical)", [this] { zoomStepGridVertical(1.25f); });
        if (key.getModifiers().isShiftDown() && key.isKeyCode('D'))
            return trigger("Cmd+Shift+D - Zoom Out (Horizontal)", [this] { zoomStepGridHorizontal(1.25f); });
        if (key.getModifiers().isShiftDown() && key.isKeyCode('F'))
            return trigger("Cmd+Shift+F - Zoom In (Horizontal)", [this] { zoomStepGridHorizontal(0.8f); });
        if (key.isKeyCode('S') && key.getModifiers().isShiftDown())
            return trigger("Cmd+Shift+S - Save As", [this] { saveProjectAs(); });
        // Set Loop Start -- displaced off plain Shift+C once that became
        // Jump Back 1 Bar (paired with plain 'c' becoming Retreat, see
        // switch(c) below). Kept on the same letter
        // 'C' as before, just with Cmd added on top, so it still pairs
        // cleanly by letter with Cmd+Ctrl+C's Set Loop End.
        if (key.isKeyCode('C') && key.getModifiers().isShiftDown())
            return trigger("Cmd+Shift+C - Set Loop Start", [this] { setLoopStartHere(); });
        // Shift+A's twin (see jumpToClipEnd()'s declaration) -- shared,
        // unconditional pure cursor movement, same as Shift+A itself, so
        // it behaves identically whether editing notes or automation.
        if (key.isKeyCode('A') && key.getModifiers().isShiftDown() && currentViewMode != ViewMode::Session)
            return trigger("Cmd+Shift+A - Jump to End", [this] { jumpToClipEnd(); });
        // Groups with Cmd+U (Cycle Quantize Amount, below) under the same
        // "quantize toggles" letter -- toggles whether Real-time REC auto-
        // quantizes every note it commits (see autoQuantizeOnRecordEnabled's
        // declaration).
        if (key.isKeyCode('U') && key.getModifiers().isShiftDown())
            return trigger("Cmd+Shift+U - Toggle Auto-Quantize on Record", [this] { toggleAutoQuantizeOnRecord(); });
        // Save moved off Cmd+S onto Cmd+0 (below) to free Cmd+S up for
        // Unquantize -- Cmd+5 was already taken by the chord-voicing octave
        // shift. Piano Roll
        // only, same scope the old plain-'5' Unquantize binding had.
        if (key.isKeyCode('S') && currentViewMode != ViewMode::Session)
            return trigger("Cmd+S - Unquantize", [this] { unquantizeSelectedNotes(); });
        if (key.isKeyCode('O')) // '0' alias moved to Save (see below), Cmd+O is Open's only binding now
            return trigger("Cmd+O - Open", [this] { openProject(); });
        if (key.isKeyCode('0'))
            return trigger("Cmd+0 - Save", [this] { saveProject(); });
        if (key.isKeyCode('N'))
            return trigger("Cmd+N - New", [this] { newProject(); });
        if (key.isKeyCode('Y')) // moved off Cmd+I, used infrequently enough that the reach is fine
            return trigger("Cmd+Y - Instrument", [this] { openInstrumentPanel(); });
        if (key.isKeyCode('P')) // show/hide the current track's plugin editor window
            return trigger("Cmd+P - Plugin Editor", [this] { togglePluginEditor(); });
        if (key.isKeyCode(',')) // macOS's standard "Preferences" shortcut
            return trigger("Cmd+, - Audio/MIDI Settings", [this] { openAudioMidiSettings(); });
        if (key.isKeyCode('K')) // 'K' for "Keyboard" -- show/hide the live shortcut cheat-sheet window
            return trigger("Cmd+K - Keyboard Overlay", [this] { toggleKeyboardOverlay(); });
        // Alias for plain 'q' (Duplicate Range) -- added alongside it, not
        // instead of it, per the user's request. Piano Roll only, same
        // scope as 'q'.
        if (key.isKeyCode('D') && currentViewMode != ViewMode::Session)
            return trigger("Cmd+D - Duplicate Range", [this] { duplicateSelectedRange(); });
        // In-chord note selection (navigateNoteSelection()) -- swapped with
        // plain G/B (see switch(c) below), per the user's request,
        // relocated off T/G onto G/B along with every other "up/down" pair.
        // Cmd+B displaces the old Set Clip End (moved
        // to Cmd+Ctrl+E, see the Ctrl block above).
        if (key.isKeyCode('G'))
            return trigger("Cmd+G - Select Top Note", [this] { navigateNoteSelection(-1, false); });
        if (key.isKeyCode('B'))
            return trigger("Cmd+B - Select Note Down", [this] { navigateNoteSelection(1, false); });
        if (key.isKeyCode('M')) // toggle piano-roll scale tint: Auto (estimated key) -> Off
            return trigger("Cmd+M - Cycle Scale", [this] { cycleScale(); });
        // Automation edit mode redefines Cmd+C/V as copying/pasting
        // automation points instead -- see copySelectedAutomationPoints()'s
        // declaration.
        if (key.isKeyCode('C') && automationEditModeActive && currentViewMode != ViewMode::Session)
            return trigger("Cmd+C - Copy Automation Points", [this] { copySelectedAutomationPoints(); });
        if (key.isKeyCode('V') && automationEditModeActive && currentViewMode != ViewMode::Session)
            return trigger("Cmd+V - Paste Automation Points", [this] { pasteAutomationPointsAtCursor(); });
        // Note copy/paste -- standard OS convention, taking priority over
        // Cmd+C's old "Set Loop End" role (moved to Cmd+Ctrl+C, see the
        // Ctrl block above). Piano Roll only (no step cursor/note concept
        // in Session View).
        if (key.isKeyCode('C') && currentViewMode != ViewMode::Session)
            return trigger("Cmd+C - Copy", [this] { copySelectedNotes(); });
        if (key.isKeyCode('V') && currentViewMode != ViewMode::Session)
            return trigger("Cmd+V - Paste", [this] { pasteNotesAtCursor(); });
        // Standard OS convention -- selects every note in the current
        // track's clip into multiSelectedNoteStarts so a single following
        // action (delete, transpose, quantize, velocity, ...) applies to
        // the whole clip at once. Displaces
        // the old "toggle chord track" binding, moved to Cmd+Ctrl+H (see
        // the Ctrl block above). Piano Roll only (no note concept in
        // Session View).
        if (key.isKeyCode('A') && currentViewMode != ViewMode::Session)
            return trigger("Cmd+A - Select All Notes", [this] { selectAllNotesInCurrentTrack(); });
        // Chord-voicing edge shift -- only moves the ONE extreme note of the
        // currently-selected chord notes (see effectiveSelectedPitches()),
        // leaving the rest of the chord untouched; a no-op below 2 selected
        // notes, since shifting a single note's own extreme is already
        // Shift+T/G's whole-note octave shift. Moved back here from
        // Shift+5/Shift+R -- Shift+digit combos don't reliably arrive at
        // all on this keyboard/JUCE setup (Shift+5 types '%' instead), so
        // the whole pair came back to Cmd rather than leave just one half
        // on Shift. Displaces the range-duplication start/end markers,
        // which stay on Cmd+Ctrl+5/Cmd+Ctrl+R (see the Ctrl block above).
        if (key.isKeyCode('5'))
            return trigger("Cmd+5 - Raise Lowest Note (Octave)", [this] { adjustSelectionVoicingEdge(true); });
        if (key.isKeyCode('R'))
            return trigger("Cmd+R - Lower Highest Note (Octave)", [this] { adjustSelectionVoicingEdge(false); });
        // Cmd+U cycles the quantize AMOUNT (25/50/75/100%, see
        // quantizeAmountPercent's declaration) -- distinct from the
        // quantize GRID keys below. Piano Roll only (no step cursor/
        // quantize concept in Session View).
        if (key.isKeyCode('U') && currentViewMode != ViewMode::Session)
            return trigger("Cmd+U - Cycle Quantize Amount", [this] { cycleQuantizeAmount(); });
        // Quantize grid -- moved here off plain 1/2/3/4 (see switch(c)
        // below) so plain 3/e could become track prev/next in the Piano
        // Roll too, matching Session View.
        // Unquantize itself is Cmd+S instead of Cmd+5, since Cmd+5/Cmd+R
        // were already the chord-voicing octave shift.
        if (key.isKeyCode('1') && currentViewMode != ViewMode::Session)
            return trigger("Cmd+1 - Quantize 1/4", [this]
            {
                quantizeSelectedNotes(project.tracks[(size_t) cursorTrackIndex].clip.stepsPerQuarterNote);
            });
        if (key.isKeyCode('2') && currentViewMode != ViewMode::Session)
            return trigger("Cmd+2 - Quantize 1/8", [this]
            {
                quantizeSelectedNotes(project.tracks[(size_t) cursorTrackIndex].clip.stepsPerQuarterNote / 2);
            });
        if (key.isKeyCode('3') && currentViewMode != ViewMode::Session)
            return trigger("Cmd+3 - Quantize 1/16", [this]
            {
                quantizeSelectedNotes(project.tracks[(size_t) cursorTrackIndex].clip.stepsPerQuarterNote / 4);
            });
        if (key.isKeyCode('4') && currentViewMode != ViewMode::Session)
            return trigger("Cmd+4 - Toggle Triplet Quantize", [this] { toggleQuantizeTripletMode(); });
        // Moved off plain 'g' once that became the bar-jump-back key (see
        // switch(c) below) -- 'X' for the "Cut"-like association most other
        // apps already have with it. Mode-aware, same as 'g' used to be.
        if (key.isKeyCode('X'))
            return currentViewMode == ViewMode::Session
                ? trigger("Cmd+X - Capture to Slot", [this] { captureClipToSlotAtCursor(); })
                : trigger("Cmd+X - Delete+Retreat", [this] { deleteAndRetreat(); });
        // Standard macOS undo/redo shortcuts (Cmd, not Ctrl, for consistency
        // with every other modifier in this app -- see the top-of-file
        // convention notes). Scoped to note edits only, see StepEditGuard.
        if (key.isKeyCode('Z') && key.getModifiers().isShiftDown())
            return trigger("Cmd+Shift+Z - Redo", [this] { performRedo(); });
        if (key.isKeyCode('Z'))
            return trigger("Cmd+Z - Undo", [this] { performUndo(); });
        return false;
    }

    // Piano Roll, Browse mode (recMode == Off) only: Shift+Option+G/B
    // directly nudge the note at the cursor by a semitone, up/down --
    // Browse mode has no pending input to narrow a selection FOR, so this
    // is more useful here than track switching. Everywhere else (Session
    // View, or Manual/Auto REC mode), Shift+Option+G/B keep the normal
    // track-switch meaning. Checked before the plain-Shift and plain-Option
    // blocks below so this more specific combo takes priority. This used to
    // be Octave Up/Down -- swapped with plain Shift+T/G's old role once
    // Octave moved to Cmd+Ctrl+T/G (see the Ctrl block above) and Extend
    // Selection needed plain Shift+T/G instead (see the Shift block below),
    // per the user's request. Relocated off T/G onto G/B along with every
    // other "up/down" pair.
    if (key.getModifiers().isShiftDown() && key.getModifiers().isAltDown())
    {
        auto browseModePitchNudge = currentViewMode == ViewMode::PianoRoll && recMode == RecMode::Off;
        if (key.isKeyCode('G'))
            return browseModePitchNudge
                ? trigger("Shift+Option+G - Pitch Up", [this] { adjustNotePitch(1); })
                : trigger("Shift+Option+G - Prev Track", [this] { switchTrack(-1); });
        if (key.isKeyCode('B'))
            return browseModePitchNudge
                ? trigger("Shift+Option+B - Pitch Down", [this] { adjustNotePitch(-1); })
                : trigger("Shift+Option+B - Next Track", [this] { switchTrack(1); });
        return false;
    }

    if (key.getModifiers().isAltDown())
    {
        // Tempo: reuses the octave-preview Z (down) / X (up) keys with
        // Option, same "down/up" sense those already have for z/x, just
        // scoped to BPM instead of live-input octave. Semitone pitch nudge
        // used to live here (Option+T/G) -- moved to Cmd+Ctrl+3/E (see the
        // Ctrl block above), so T/G are free at this tier now.
        if (key.isKeyCode('Z'))
            return trigger("Option+Z - Tempo Down", [this] { adjustTempo(-1.0); });
        if (key.isKeyCode('X'))
            return trigger("Option+X - Tempo Up", [this] { adjustTempo(1.0); });
        // Nudges the note/automation point at the cursor (or every one in
        // a Shift+D/F multi-selection) left/right by a single base step --
        // direction: -1 (Option+D) or +1 (Option+F). Piano Roll only (no
        // note/point concept in Session View). Automation edit mode
        // redefines this to nudge automation points/events instead of
        // notes, same "same physical key, different target depending on
        // mode" pattern already used elsewhere in this file (Ctrl+V/
        // Cmd+Ctrl+I, a/Cmd+Ctrl+D, etc.).
        if (key.isKeyCode('D') && currentViewMode != ViewMode::Session)
            return trigger("Option+D - Nudge Left", [this]
            {
                if (automationEditModeActive) nudgeSelectedAutomationPoints(-1);
                else nudgeSelectedNotes(-1);
            });
        if (key.isKeyCode('F') && currentViewMode != ViewMode::Session)
            return trigger("Option+F - Nudge Right", [this]
            {
                if (automationEditModeActive) nudgeSelectedAutomationPoints(1);
                else nudgeSelectedNotes(1);
            });
        return false;
    }

    // Commit-duration cycling reuses the octave-shift Z/X keys with Shift
    // instead of adding new plain keys -- economize by reusing existing
    // keys via a modifier.
    if (key.getModifiers().isShiftDown())
    {
        if (key.isKeyCode('Z')) // finer duration
            return trigger("Shift+Z - Finer Duration", [this] { cycleCommitDuration(-1); });
        if (key.isKeyCode('X')) // coarser duration
            return trigger("Shift+X - Coarser Duration", [this] { cycleCommitDuration(1); });
        // Automation edit mode redefines these as extending the automation
        // multi-point selection instead -- see extendAutomationSelection()'s
        // declaration.
        if (key.isKeyCode('F') && automationEditModeActive && currentViewMode != ViewMode::Session)
            return trigger("Shift+F - Extend Automation Selection Fwd", [this] { extendAutomationSelection(1); });
        if (key.isKeyCode('D') && automationEditModeActive && currentViewMode != ViewMode::Session)
            return trigger("Shift+D - Extend Automation Selection Back", [this] { extendAutomationSelection(-1); });
        // Extends the multi-note quantize selection (see
        // extendNoteSelection()) -- moved here from "jump 1 bar", which
        // relocated to plain 's'/'g' below.
        if (key.isKeyCode('F'))
            return trigger("Shift+F - Extend Note Selection Fwd", [this] { extendNoteSelection(1); });
        if (key.isKeyCode('D'))
            return trigger("Shift+D - Extend Note Selection Back", [this] { extendNoteSelection(-1); });
        // Extend the individual-note selection (navigateNoteSelection()) --
        // moved off Shift+T/G once those became track switching, per the
        // user's request.
        if (key.isKeyCode('Q'))
            return trigger("Shift+Q - Extend Selection Up", [this] { navigateNoteSelection(-1, true); });
        // Extend Selection Down moved off Shift+A onto Shift+R (freed up by
        // the voicing edge shift moving back to Cmd+5/Cmd+R below) -- Shift+A
        // is now "jump to start" instead.
        // Reuses moveCursor()'s own clamp-to-0 rather than a dedicated
        // method: moveCursor(-cursorStepIndex) always lands exactly on 0.
        if (key.isKeyCode('A'))
            return trigger("Shift+A - Jump to Start", [this] { moveCursor(-cursorStepIndex); });
        if (key.isKeyCode('R'))
            return trigger("Shift+R - Extend Selection Down", [this] { navigateNoteSelection(1, true); });
        // Octave shift -- moved here off Cmd+Ctrl+T/G (now free), displacing
        // Prev/Next Track (still reachable via Cmd+Ctrl+P/N), per the
        // user's request.
        // Automation edit mode: coarse value up/down -- see plain g/b's
        // matching check in switch(c) below. Relocated off Shift+T/G onto
        // Shift+G/B along with every other "up/down" pair, freeing plain
        // Shift+T entirely.
        if (key.isKeyCode('G') && automationEditModeActive && currentViewMode != ViewMode::Session)
            return trigger("Shift+G - Automation Value Up (Coarse)", [this] { adjustAutomationPendingValue(1, true); });
        if (key.isKeyCode('B') && automationEditModeActive && currentViewMode != ViewMode::Session)
            return trigger("Shift+B - Automation Value Down (Coarse)", [this] { adjustAutomationPendingValue(-1, true); });
        if (key.isKeyCode('G'))
            return trigger("Shift+G - Octave Up", [this] { adjustNotePitch(12); });
        if (key.isKeyCode('B'))
            return trigger("Shift+B - Octave Down", [this] { adjustNotePitch(-12); });
        // Same letter as plain 'w' (Toggle Metronome, see switch(c) below)
        // -- both are playback click/pre-roll settings.
        if (key.isKeyCode('W'))
            return trigger("Shift+W - Toggle Count-In", [this] { toggleCountIn(); });
        // Jump by a full measure (4 beats, 4/4 assumed) -- pairs directly
        // with plain 'c'/'v' below (duration-preset step, back/forward),
        // Shift making the same two keys jump a whole bar instead of one
        // step, relocated onto c/v once those took over v/b's old job.
        // Piano Roll only, same scope 's'/Ctrl+G's older bar-jump bindings
        // already have (no step cursor in Session View to jump). Displaces
        // the old Shift+C (Set Loop Start, moved to Cmd+Shift+C above).
        if (key.isKeyCode('C') && currentViewMode != ViewMode::Session)
            return trigger("Shift+C - Jump Back 1 Bar", [this]
            {
                moveCursor(-4 * project.tracks[(size_t) cursorTrackIndex].clip.stepsPerQuarterNote);
            });
        if (key.isKeyCode('V') && currentViewMode != ViewMode::Session)
            return trigger("Shift+V - Jump Forward 1 Bar", [this]
            {
                moveCursor(4 * project.tracks[(size_t) cursorTrackIndex].clip.stepsPerQuarterNote);
            });
        // Checked here (inside the Shift block) rather than alongside plain
        // Space below -- this whole block is entered whenever Shift is
        // held, so Shift+Space would otherwise fall through to this
        // block's own `return false;` and never reach the plain-Space
        // check at all.
        if (key.isKeyCode(juce::KeyPress::spaceKey))
            return trigger("Shift+Space - Play From Locator", [this] { playFromLocator(); });
        return false;
    }

    // Space is the more natural "play" key (matches virtually every media
    // player/DAW convention), and pairs naturally with Shift+Space ("play
    // from locator") right above. Tab toggles Session View -- moved here
    // from plain 's' so 's' could take over the bar-jump role (see
    // switch(c) below).
    if (key == juce::KeyPress::spaceKey)
        return trigger("Space - Play/Stop", [this] { togglePlayback(); });
    if (key == juce::KeyPress::tabKey)
        return trigger("Tab - Toggle Session View", [this] { toggleViewMode(); });
    // Toggles the always-on virtual keyboard between melodic/drum-pad mode
    // (see toggleDrumGridMode()) -- neither map has a held modifier of its
    // own anymore, so this is what disambiguates them.
    if (key == juce::KeyPress::returnKey)
        return trigger("Enter - Toggle Drum Grid", [this] { toggleDrumGridMode(); });

    auto c = juce::CharacterFunctions::toLowerCase((juce::juce_wchar) key.getTextCharacter());

    // 3/e/d/f/t/z/x/b are the only keys that mean something different in
    // Session View -- q/s/g/1/2/4/5/0 exist ONLY in the Piano Roll (no-op
    // in Session View, same as 'b' used to be alone) since they all operate
    // on the step cursor/note data Session View doesn't have. Everything
    // else in this switch (v,c,r,w) plus every Space/Tab/Cmd/Shift/Option
    // shortcut above stays identical in both views. Session View's own
    // scheme: 3/e move the track (row) cursor, d/f move the slot (column)
    // cursor -- mirroring how 3/e and d/f already move things in the piano
    // roll, just repurposed onto the grid's two axes -- while z/x (stop/
    // launch) and t (load, capture is Cmd+X) carry the actual clip
    // actions, kept off the navigation keys so moving the cursor around
    // never accidentally launches or stops anything.
    auto inSessionView = currentViewMode == ViewMode::Session;

    switch (c)
    {
        case 'd': return inSessionView
            ? trigger("d - Prev Slot", [this] { moveSessionCursor(-1); })
            : trigger("d - Prev", [this] { handleBackwardKey(); });
        case 'f': return inSessionView
            ? trigger("f - Next Slot", [this] { moveSessionCursor(1); })
            : trigger("f - Next", [this] { handleForwardKey(); });
        // Automation edit mode redefines 'a' as clearing the point/event at
        // the cursor -- same key and (via deleteAutomationPointAtCursor()'s
        // own fallback) same "select the nearest remaining one, or back to
        // bar 1 if none" behavior as clearing a note.
        case 'a': if (!inSessionView && automationEditModeActive) return trigger("a - Delete Automation Point", [this] { deleteAutomationPointAtCursor(); });
            return inSessionView
            ? trigger("a - Delete Clip", [this] { deleteClipAtCursor(); })
            : trigger("a - Clear Step", [this] { clearCurrentStep(); });
        // Delete+Retreat/Capture to Slot moved off plain 'g' to Cmd+X once
        // 'g' was needed for the bar-jump-back role below. Piano Roll 't'
        // used to be Tie -- moved to Ctrl+T (see the Ctrl block above) once
        // 't' was needed here for the semitone pitch nudge instead, swapped
        // with Cmd+G's old role (see the Cmd block above) per the user's
        // request. Piano-Roll/
        // automation "up" meaning relocated off plain 't' onto plain 'g'
        // below along with every other "up/down" pair -- only the
        // unrelated Session-View meaning
        // (Load Slot) stays here, since that was never part of the
        // up/down convention to begin with.
        case 't': if (inSessionView) return trigger("t - Load Slot to Editor", [this] { loadSlotAtCursorToEditor(); });
            break;
        case 'z': return inSessionView
            ? trigger("z - Stop Track", [this] { stopCurrentTrackSlot(); })
            : trigger("z - Octave Down", [this] { shiftOctave(-1); });
        case 'x': return inSessionView
            ? trigger("x - Launch Slot", [this] { launchSlotAtCursor(); })
            : trigger("x - Octave Up", [this] { shiftOctave(1); });
        // Pure cursor advance by the current duration preset, no write --
        // works the same in both views. Pairs with 'c' below (retreat) --
        // relocated here off plain 'b' so the whole back/forward pair sits
        // directly beneath d/f on the keyboard instead of one row further
        // right, once 'd'/'f' settled on pure note/point jumping and this
        // pair needed a home of its own.
        case 'v': return trigger("v - Advance", [this] { advanceByDuration(); });
        // Retreat's other half of the pair above -- displaces the old
        // plain 'c' (Toggle Loop, relocated to plain 'b' below, see its
        // comment).
        case 'c': return trigger("c - Retreat", [this] { retreatByDuration(); });
        // Duplicates the Cmd+Ctrl+5/Cmd+Ctrl+R marked range -- Piano Roll only (no
        // step cursor/range concept in Session View), falls through to
        // no-op there same as 'b'. Moved here from plain 'v' once 'v' took
        // over Retreat's job.
        case 'q': if (!inSessionView) return trigger("q - Duplicate Range", [this] { duplicateSelectedRange(); });
            break;
        // Jump the locator by a full measure (4 beats, 4/4 assumed -- this
        // app has no separate time-signature field), Piano Roll only (no
        // step cursor in Session View to jump) -- moved here from
        // Shift+S/Shift+D once those became note-selection extension.
        case 's': if (!inSessionView) return trigger("s - Jump Back 1 Bar", [this]
            {
                moveCursor(-4 * project.tracks[(size_t) cursorTrackIndex].clip.stepsPerQuarterNote);
            });
            break;
        // Jump Forward 1 Bar used to live here -- moved to Ctrl+G (see the
        // Ctrl block above) once 'g' was needed for the semitone pitch nudge
        // instead, swapped with Cmd+G's old role, same as 't' above. Now the
        // app-wide "up" half of the up/down convention (absorbing plain
        // 't's old Piano-Roll/automation meaning -- see 't' above) --
        // relocated off T/G onto G/B along with every other "up/down" pair.
        // Still no Session-View meaning of its own, same as before.
        case 'g': if (!inSessionView && automationEditModeActive) return trigger("g - Automation Value Up", [this] { adjustAutomationPendingValue(1, false); });
            if (!inSessionView) return trigger("g - Pitch Up", [this] { adjustNotePitch(1); });
            break;
        // Session View: Duplicate Clip (unrelated to the up/down convention,
        // stays put regardless of what plain 'b' does elsewhere). Piano
        // Roll: the "down" half of the up/down pair now that it moved off
        // G onto G/B -- Toggle Loop (which briefly lived here after v/b
        // took over c/v's old job) relocated again to Cmd+Ctrl+B to make
        // room.
        case 'b': if (inSessionView) return trigger("b - Duplicate Clip", [this] { duplicateClipAtCursor(); });
            if (automationEditModeActive) return trigger("b - Automation Value Down", [this] { adjustAutomationPendingValue(-1, false); });
            return trigger("b - Pitch Down", [this] { adjustNotePitch(-1); });
        case 'w': return trigger("w - Toggle Metronome", [this] { toggleMetronome(); });
        // Cycles recMode: Off (Browse) -> Manual (Step REC, confirm) ->
        // Auto (Step REC auto-commit) -> Realtime (dedicated real-time
        // recording -- preview-only while stopped, actual capture once
        // playback starts) -> back to Off. See recMode's declaration and
        // handleMidiNoteChange()/commitPendingNoteManually(). Not mode-
        // gated (works the same in both views, same as 'c'/'w' above) --
        // it only actually changes anything once you're back in the Piano
        // Roll actually playing/committing notes.
        case 'r': return trigger("r - Cycle REC Mode", [this] { cycleRecMode(); });
        // Track prev/next -- now the same in both views (used to be
        // Session-View-only, with Piano Roll repurposing '3' for quantize
        // and 'e' as a no-op; quantize moved to Cmd+1-4/Cmd+S instead, see
        // the Cmd block above), per the user's request.
        case '3': return trigger("3 - Prev Track", [this] { switchTrack(-1); });
        case 'e': return trigger("e - Next Track", [this] { switchTrack(1); });
        // Note Repeat rate -- previously-unused plain digits (no note map
        // or editing shortcut ever claimed 1/2/4, see VirtualKeyboardMaps.h;
        // '0' IS a note-map key, '3' is Prev Track, so those two are
        // skipped). Pressing the already-active rate again turns note
        // repeat off instead of re-selecting it -- see setNoteRepeatRate()'s
        // declaration. Works the same in both views, same as 'c'/
        // 'w'/'r' above -- it only actually writes anything once you're
        // back in the Piano Roll with REC on, but the live audio retrigger
        // is meaningful either way.
        case '1': return trigger("1 - Note Repeat 1/4", [this]
            {
                setNoteRepeatRate(project.tracks[(size_t) cursorTrackIndex].clip.stepsPerQuarterNote);
            });
        case '2': return trigger("2 - Note Repeat 1/8", [this]
            {
                setNoteRepeatRate(project.tracks[(size_t) cursorTrackIndex].clip.stepsPerQuarterNote / 2);
            });
        case '4': return trigger("4 - Note Repeat 1/16", [this]
            {
                setNoteRepeatRate(project.tracks[(size_t) cursorTrackIndex].clip.stepsPerQuarterNote / 4);
            });
        case '5': return trigger("5 - Toggle Note Repeat Triplet", [this] { toggleNoteRepeatTripletMode(); });
        default: break;
    }

    // The virtual keyboard/drum grid keys are read by polling
    // (pollVirtualKeyboardInput()), not through this switch -- but
    // returning false here for them leaves the keystroke "unhandled" as
    // far as JUCE/macOS is concerned, which triggered the OS system alert
    // beep on every single note press once these keys stopped requiring
    // Ctrl (the old Ctrl block used to unconditionally return true for
    // every Ctrl combo for exactly this reason -- see its comment history).
    // Claim them here instead so they're silent, same as any other
    // recognized key.
    if (virtualKeyboardKeyMap().count((char) key.getKeyCode()) || virtualDrumKeyMap().count((char) key.getKeyCode()))
        return true;

    return false;
}

void MainEditorComponent::syncProjectInstrumentState()
{
    for (int i = 0; i < (int) project.tracks.size(); ++i)
    {
        auto* instrument = playbackEngine.getTrackInstrument(i);
        auto& track = project.tracks[(size_t) i];

        if (instrument != nullptr)
        {
            track.instrumentDescription = instrument->getPluginDescription();
            instrument->getStateInformation(track.instrumentState);
        }
        else
        {
            track.instrumentDescription = juce::PluginDescription();
            track.instrumentState.reset();
        }
    }
}

void MainEditorComponent::restoreInstrumentsFromProject()
{
    for (int i = 0; i < (int) project.tracks.size(); ++i)
    {
        auto& track = project.tracks[(size_t) i];
        if (track.instrumentDescription.name.isEmpty())
            continue;

        auto description = track.instrumentDescription;
        auto state = track.instrumentState;

        pluginHost.createInstrument(description, 44100.0, 512,
            [this, trackIndex = i, state](std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error)
            {
                if (instance != nullptr)
                {
                    if (state.getSize() > 0)
                        instance->setStateInformation(state.getData(), (int) state.getSize());

                    playbackEngine.setTrackInstrument(trackIndex, std::move(instance));
                    registerParameterAutomationListener(trackIndex);
                    refreshChildViews();
                }
                else
                {
                    DBG("Failed to restore instrument for track " << trackIndex << ": " << error);
                }
            });
    }
}

void MainEditorComponent::writeProjectToFile(const juce::File& file)
{
    syncProjectInstrumentState();

    if (auto xml = project.toValueTree().createXml())
        xml->writeTo(file);
}

void MainEditorComponent::saveProject()
{
    if (currentProjectFile != juce::File())
        writeProjectToFile(currentProjectFile);
    else
        saveProjectAs();
}

void MainEditorComponent::saveProjectAs()
{
    fileChooser = std::make_unique<juce::FileChooser>(
        "Save PianoRollMaestro Project",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
        juce::String("*") + Project::fileExtension);

    fileChooser->launchAsync(
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
        [this](const juce::FileChooser& chooser)
        {
            auto file = chooser.getResult();
            if (file == juce::File())
            {
                grabKeyboardFocus();
                return;
            }

            if (!file.hasFileExtension(Project::fileExtension))
                file = file.withFileExtension(Project::fileExtension);

            writeProjectToFile(file);
            currentProjectFile = file;
            grabKeyboardFocus();
        });
}

void MainEditorComponent::openProject()
{
    fileChooser = std::make_unique<juce::FileChooser>(
        "Open PianoRollMaestro Project",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
        juce::String("*") + Project::fileExtension);

    fileChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& chooser)
        {
            auto file = chooser.getResult();
            if (file == juce::File())
            {
                grabKeyboardFocus();
                return;
            }

            if (auto xml = juce::XmlDocument::parse(file))
            {
                auto tree = juce::ValueTree::fromXml(*xml);
                if (tree.isValid())
                {
                    playbackEngine.stop();
                    project.loadFromValueTree(tree);
                    // loadFromValueTree() may have replaced tracks with a
                    // freshly (un)reserved vector -- re-reserve with
                    // headroom beyond whatever it actually loaded, same as
                    // newProject(), so addTrack() stays safe to push_back()
                    // during playback afterward.
                    project.tracks.reserve(juce::jmax(reservedTrackCapacity, project.tracks.size() + 16));
                    // Pre-builds every loaded track's TrackAudioState here
                    // rather than leaving the audio thread's next
                    // renderNextBlock() call to do it -- same reasoning as
                    // addTrack(), just for however many tracks this project
                    // actually has.
                    playbackEngine.prepareTrackAudioStates();
                    currentProjectFile = file;
                    cursorTrackIndex = 0;
                    cursorStepIndex = 0;
                    noteSelectionAnchorStep = -1; // see moveCursor()'s comment -- an entirely different project just loaded in
                    sessionCursorSlotIndex = 0;
                    // Old undo entries reference step data by track index
                    // into the project that just got replaced -- meaningless
                    // (and, if track counts differ, unsafe) to keep around.
                    undoManager.clearUndoHistory();
                    // Every existing plugin editor window points at an
                    // instrument that's about to be replaced/destroyed.
                    pluginEditorWindowsByTrack.clear();
                    pluginEditorDesiredVisible = false;
                    // editingSlotIndex isn't persisted (always -1 right
                    // after a load, see its declaration) -- land in Session
                    // View rather than an editing buffer that isn't linked
                    // to anything in it yet.
                    currentViewMode = ViewMode::Session;
                    resized();
                    refreshChildViews();
                    restoreInstrumentsFromProject();
                }
            }

            grabKeyboardFocus();
        });
}

void MainEditorComponent::newProject()
{
    playbackEngine.stop();
    unregisterParameterAutomationListener(0);
    playbackEngine.setTrackInstrument(0, nullptr); // clear any leftover instrument from the previous project
    pluginEditorWindowsByTrack.clear(); // pointed at that now-destroyed instrument
    pluginEditorDesiredVisible = false;

    project = Project{};
    project.tracks.reserve(reservedTrackCapacity); // operator= above replaced tracks with a fresh, unreserved vector
    project.tracks.push_back(Track{});
    playbackEngine.prepareTrackAudioStates(); // see its declaration -- avoids building this on the audio thread instead
    currentProjectFile = juce::File();
    cursorTrackIndex = 0;
    cursorStepIndex = 0;
    noteSelectionAnchorStep = -1; // see moveCursor()'s comment -- brand new project
    sessionCursorSlotIndex = 0;
    undoManager.clearUndoHistory(); // see the matching comment in openProject()

    // A blank project has nothing linked to any slot yet -- start where
    // that's created, not in an empty piano roll disconnected from Session
    // View's grid.
    currentViewMode = ViewMode::Session;
    resized();
    refreshChildViews();
}

void MainEditorComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::darkgrey);
}

void MainEditorComponent::resized()
{
    auto bounds = getLocalBounds();

    auto topRow = bounds.removeFromTop(30);
    midiDeviceBox.setBounds(topRow.removeFromLeft(300).reduced(4));
    playButton.setBounds(topRow.removeFromLeft(80).reduced(4));
    instrumentButton.setBounds(topRow.removeFromLeft(100).reduced(4));
    audioSettingsButton.setBounds(topRow.removeFromLeft(110).reduced(4));

    shortcutHelpBar.setBounds(bounds.removeFromBottom(48));

    // transportBar's badges wrap onto as many rows as its actual width
    // needs (see TransportBarComponent::getRequiredHeightForWidth()'s
    // declaration) rather than running off the right edge, which is what
    // used to happen once enough badges accumulated. Its width here is
    // always the full window width (nothing's been subtracted from bounds
    // yet), so the required height can be computed before actually sizing it.
    transportBar.setBounds(bounds.removeFromTop(transportBar.getRequiredHeightForWidth(bounds.getWidth())));
    trackList.setBounds(bounds.removeFromLeft(200));

    // Session View and the piano roll share this same central area --
    // exactly one is visible at a time, per currentViewMode.
    auto pianoRollMode = currentViewMode == ViewMode::PianoRoll;
    chordEstimateBar.setVisible(pianoRollMode);
    stepGrid.setVisible(pianoRollMode);
    sessionGrid.setVisible(!pianoRollMode);

    if (pianoRollMode)
    {
        chordEstimateBar.setBounds(bounds.removeFromTop(22));
        stepGrid.setBounds(bounds);
    }
    else
    {
        sessionGrid.setBounds(bounds);
    }
}
