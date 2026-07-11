#include "MainEditorComponent.h"

// Defined further below; forward-declared here so earlier methods (e.g.
// moveCursorByNoteOrStep) can use them too.
static int noteTotalLengthInSteps(const std::vector<Step>& steps, int ownerIndex);
static int findOwningNoteStepIndex(const std::vector<Step>& steps, int stepIndex);

MainEditorComponent::MainEditorComponent()
{
    project.tracks.push_back(Track{});
    playbackEngine.setProject(&project);

    addAndMakeVisible(transportBar);
    addAndMakeVisible(trackList);
    addAndMakeVisible(stepGrid);
    addAndMakeVisible(shortcutHelpBar);

    addAndMakeVisible(midiDeviceBox);
    midiDeviceBox.onChange = [this] { midiDeviceSelected(); };
    refreshMidiDeviceList();

    addAndMakeVisible(playButton);
    playButton.onClick = [this] { togglePlayback(); grabKeyboardFocus(); };

    addAndMakeVisible(instrumentButton);
    instrumentButton.onClick = [this] { openInstrumentPanel(); };

    addAndMakeVisible(audioSettingsButton);
    audioSettingsButton.onClick = [this] { openAudioMidiSettings(); };

    addAndMakeVisible(micLevelMeter);

    midiInputRouter.onStepChordCaptured = [this](const std::vector<StepNote>& notes)
    {
        stepChordCaptured(notes);
    };
    midiInputRouter.onLiveNote = [this](int noteNumber, float velocity, bool isOn)
    {
        liveNote(noteNumber, velocity, isOn);
    };
    midiInputRouter.onLiveControllerMessage = [this](const juce::MidiMessage& message)
    {
        playbackEngine.liveMidiMessage(cursorTrackIndex, message);
    };

    // 'v' toggles hum listening on/off (see toggleHumInput()). Deliberately
    // NOT routed through midiInputRouter.injectNote() -- humming is monitor-
    // only (a continuously updating live tone, like PlayMonitor), it never
    // auto-writes to the step grid. Shift+F (commitHumNote()) explicitly
    // commits whatever's currently sounding -- or, if nothing's sounding
    // right now, whatever was last heard (currentHumNote is deliberately NOT
    // cleared when humming stops, only when a NEW pitch is detected or hum
    // listening is toggled off in toggleHumInput()). Humming and pressing a
    // key at the exact same instant is awkward, so committing works on a
    // delay: hum a note, stop, then press Shift+F (possibly repeatedly) at
    // your own pace.
    humInputListener.onNoteChange = [this](int noteNumber, float velocity, bool isOn)
    {
        if (isOn)
            currentHumNote = noteNumber;
        liveNote(noteNumber, velocity, isOn);
        // Update the HUM status text AND the step-grid preview outline right
        // here, the instant the pitch is detected -- not just whenever
        // refreshChildViews() happens to run for some unrelated reason (e.g.
        // cursor movement). Otherwise the preview only reflects reality
        // after some later action, defeating its purpose of showing what
        // Shift+F would commit before you actually press it.
        updateHumDisplays();
    };

    setWantsKeyboardFocus(true);

    setSize(900, 600);

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

    // Mic manager: entirely separate AudioDeviceManager, input-only, opened
    // independently of the output stream above -- see micDeviceManager's
    // declaration in the header for why. Hum LISTENING itself still only
    // starts when 'v' is pressed (HumInputListener::active); only the
    // hardware stream is open early.
    std::unique_ptr<juce::XmlElement> savedMicState;
    if (auto settingsFile = getMicAudioSettingsFile(); settingsFile.existsAsFile())
        savedMicState = juce::XmlDocument::parse(settingsFile);

    micDeviceManager.initialise(1, 0, savedMicState.get(), true);
    micDeviceManager.addAudioCallback(&humInputListener);
    micDeviceManager.addChangeListener(this);

    startTimerHz(30);

    refreshChildViews();
}

MainEditorComponent::~MainEditorComponent()
{
    micDeviceManager.removeChangeListener(this);
    micDeviceManager.removeAudioCallback(&humInputListener);
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

juce::File MainEditorComponent::getMicAudioSettingsFile()
{
    auto appDataDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                           .getChildFile("Application Support")
                           .getChildFile("PianoRollMaestro");
    appDataDir.createDirectory();
    return appDataDir.getChildFile("MicDeviceState.xml");
}

void MainEditorComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &micDeviceManager)
    {
        if (auto xml = micDeviceManager.createStateXml())
            xml->writeTo(getMicAudioSettingsFile());
    }
    else if (source == &deviceManager)
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
    micLevelMeter.setLevel(humInputListener.getCurrentLevel());

    if (!playbackEngine.isPlaying())
    {
        stepGrid.setPlaybackStep(-1);
        return;
    }

    auto& clip = project.tracks[(size_t) cursorTrackIndex].clip;
    auto stepSeconds = clip.stepDurationSeconds(project.tempoBpm);
    if (stepSeconds <= 0.0)
        return;

    auto positionSeconds = (double) playbackEngine.getPlaybackPositionSamples() / playbackSampleRate;
    stepGrid.setPlaybackStep((int) (positionSeconds / stepSeconds));
}

void MainEditorComponent::toggleHumInput()
{
    auto turningOn = !humInputListener.isActive();
    humInputListener.setActive(turningOn);

    if (!turningOn)
        currentHumNote = -1; // leaving hum mode clears whatever was pending to commit

    refreshChildViews();
}

void MainEditorComponent::togglePlayback()
{
    if (playbackEngine.isPlaying())
    {
        playbackEngine.stop();

        // Land the edit cursor where playback actually stopped, instead of
        // leaving it wherever it was before Tab started playing -- same
        // step-index math timerCallback() uses for the playhead locator.
        // stop() doesn't reset the playback position, so this still reads
        // the last position reached.
        auto& clip = project.tracks[(size_t) cursorTrackIndex].clip;
        auto stepSeconds = clip.stepDurationSeconds(project.tempoBpm);
        if (stepSeconds > 0.0)
        {
            auto positionSeconds = (double) playbackEngine.getPlaybackPositionSamples() / playbackSampleRate;
            cursorStepIndex = juce::jmax(0, (int) (positionSeconds / stepSeconds));
        }
    }
    else
    {
        playbackEngine.start();
    }

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
                        playbackEngine.setTrackInstrument(trackIndex, std::move(instance));
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
                pluginEditorWindow = std::make_unique<PluginEditorWindow>(*instrument, instrument->getName());
        },
        [this, trackIndex]
        {
            playbackEngine.setTrackInstrument(trackIndex, nullptr);
            refreshChildViews();
            instrumentPanelWindow = nullptr;
        });
}

void MainEditorComponent::openAudioMidiSettings()
{
    audioMidiSettingsWindow = std::make_unique<AudioMidiSettingsWindow>(deviceManager, micDeviceManager);
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
}

void MainEditorComponent::ensureStepExists(int trackIndex, int stepIndex)
{
    auto& steps = project.tracks[(size_t) trackIndex].clip.steps;
    while ((int) steps.size() <= stepIndex)
        steps.push_back(Step{});
}

void MainEditorComponent::stepChordCaptured(const std::vector<StepNote>& notes)
{
    ensureStepExists(cursorTrackIndex, cursorStepIndex);

    Step step;
    step.notes.reserve(notes.size());
    for (auto& n : notes)
        step.notes.push_back({ juce::jlimit(0, 127, n.pitch + octaveShiftOctaves * 12), n.velocity });

    project.tracks[(size_t) cursorTrackIndex].clip.steps[(size_t) cursorStepIndex] = step;

    moveCursor(1);
}

void MainEditorComponent::liveNote(int noteNumber, float velocity, bool isOn)
{
    // Remember exactly which (track, shifted pitch) a raw note-on actually
    // sounded as, and reuse that same pair for its note-off -- recomputing
    // the shift fresh at note-off time used whatever octaveShiftOctaves/
    // cursorTrackIndex happen to be RIGHT NOW, which is wrong if either
    // changed while the note was still sounding (e.g. pressing z/x to shift
    // octave while a hummed note is still ringing). That sent the note-off
    // to a different pitch than the one actually on, leaving the original
    // note stuck sounding forever.
    if (isOn)
    {
        auto shiftedNote = juce::jlimit(0, 127, noteNumber + octaveShiftOctaves * 12);
        activeLiveNotes[noteNumber] = { cursorTrackIndex, shiftedNote };
        playbackEngine.liveNoteOn(cursorTrackIndex, shiftedNote, velocity);
    }
    else
    {
        auto it = activeLiveNotes.find(noteNumber);
        if (it == activeLiveNotes.end())
            return; // nothing actually sounding for this raw pitch

        playbackEngine.liveNoteOff(it->second.trackIndex, it->second.shiftedPitch);
        activeLiveNotes.erase(it);
    }
}

void MainEditorComponent::moveCursor(int deltaSteps)
{
    cursorStepIndex = juce::jmax(0, cursorStepIndex + deltaSteps);
    refreshChildViews();
    auditionNoteAtCursor(); // hear whatever's under the cursor as it moves, like scrubbing
}

void MainEditorComponent::moveCursorByNoteOrStep(int direction)
{
    auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
    auto ownerIndex = findOwningNoteStepIndex(steps, cursorStepIndex);

    // If the cursor is currently on/within a note, jump to the previous/next
    // note's start instead of moving step by step -- much faster for
    // browsing already-composed content. On a genuine rest, fall back to
    // moving by the CURRENTLY SELECTED duration preset (Shift+Z/X) rather
    // than a fixed single grid step -- since the grid resolution is 12
    // steps/quarter note (fine enough for triplets), a literal 1-step move
    // is much finer than any practical note length.
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
        // already exactly at this note's boundary -- fall through to single-step
    }

    moveCursor(direction * humDurationPresets[(size_t) humDurationPresetIndex]);
}

void MainEditorComponent::switchTrack(int deltaTracks)
{
    auto numTracks = (int) project.tracks.size();
    cursorTrackIndex = juce::jlimit(0, numTracks - 1, cursorTrackIndex + deltaTracks);
    refreshChildViews();
}

void MainEditorComponent::addTrack()
{
    // Must stop playback first: PlaybackEngine reads project.tracks (a raw
    // pointer into this same vector, set once via setProject()) from the
    // audio thread with no locking. push_back() can reallocate the whole
    // vector -- if the audio thread is mid-iteration over the old memory
    // when that happens, it's a use-after-free/data race, which crashed
    // when this was pressed during playback. newProject()/openProject()
    // already stop() first for the same reason; this one was missed.
    playbackEngine.stop();

    Track newTrack;
    newTrack.name = "Track " + juce::String(project.tracks.size() + 1);
    project.tracks.push_back(newTrack);

    cursorTrackIndex = (int) project.tracks.size() - 1;
    cursorStepIndex = 0;
    refreshChildViews();
}

void MainEditorComponent::insertRestAndAdvance()
{
    ensureStepExists(cursorTrackIndex, cursorStepIndex);
    project.tracks[(size_t) cursorTrackIndex].clip.steps[(size_t) cursorStepIndex] = Step{};
    moveCursor(1);
}

void MainEditorComponent::deleteAndRetreat()
{
    auto target = juce::jmax(0, cursorStepIndex - 1); // same clamping moveCursor(-1) used to apply

    auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
    auto ownerIndex = findOwningNoteStepIndex(steps, target);

    if (ownerIndex >= 0)
    {
        // Deleting any part of a note removes the WHOLE note (all its
        // tied-continuation steps too), not just the single step under the
        // cursor -- otherwise you'd leave a dangling partial tie behind.
        // Cursor lands on the note's own start, not just one step back.
        auto length = noteTotalLengthInSteps(steps, ownerIndex);
        for (int i = 0; i < length; ++i)
        {
            ensureStepExists(cursorTrackIndex, ownerIndex + i);
            project.tracks[(size_t) cursorTrackIndex].clip.steps[(size_t) (ownerIndex + i)] = Step{};
        }
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
    ensureStepExists(cursorTrackIndex, cursorStepIndex);
    project.tracks[(size_t) cursorTrackIndex].clip.steps[(size_t) cursorStepIndex] = Step{};
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
    auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
    auto ownerIndex = findOwningNoteStepIndex(steps, cursorStepIndex);
    if (ownerIndex < 0)
        return; // rest -- nothing to audition

    auto& step = steps[(size_t) ownerIndex];

    // Brief noteOn/noteOff, not a sustained hold.
    std::vector<int> pitches;
    for (auto& note : step.notes)
    {
        pitches.push_back(note.pitch);
        playbackEngine.liveNoteOn(cursorTrackIndex, note.pitch, note.velocity);
    }

    juce::Timer::callAfterDelay(150, [this, trackIndex = cursorTrackIndex, pitches]
    {
        for (auto pitch : pitches)
            playbackEngine.liveNoteOff(trackIndex, pitch);
    });
}

void MainEditorComponent::adjustNotePitch(int deltaSemitones)
{
    auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
    auto ownerIndex = findOwningNoteStepIndex(steps, cursorStepIndex);
    if (ownerIndex < 0)
        return; // rest -- nothing here to adjust

    auto& step = steps[(size_t) ownerIndex];
    for (auto& note : step.notes)
        note.pitch = juce::jlimit(0, 127, note.pitch + deltaSemitones);

    refreshChildViews();
    auditionNoteAtCursor(); // audible confirmation of the new pitch
}

void MainEditorComponent::tieCurrentStep()
{
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
    // (Shift+Z/X), same as commitHumNote() uses -- NOT the note's own
    // current total length, which would double on every repeated tie press
    // (extend by X, now length is 2X, next tie extends by 2X making it 4X,
    // and so on). Written as a genuine contiguous chain of 1-step tied
    // continuations, same reasoning as commitHumNote(): a single step with
    // lengthInSteps > 1 leaves intermediate grid slots untouched and
    // unmarked, which silently breaks the chain-walk both playback and
    // rendering rely on.
    auto extendBySteps = humDurationPresets[(size_t) humDurationPresetIndex];
    auto noteEnd = ownerIndex + noteTotalLengthInSteps(steps, ownerIndex);

    for (int i = 0; i < extendBySteps; ++i)
    {
        ensureStepExists(cursorTrackIndex, noteEnd + i);
        Step tieStep;
        tieStep.tiedFromPrevious = true;
        project.tracks[(size_t) cursorTrackIndex].clip.steps[(size_t) (noteEnd + i)] = tieStep;
    }

    cursorStepIndex = noteEnd + extendBySteps;
    refreshChildViews();
    auditionNoteAtCursor();
}

void MainEditorComponent::shiftOctave(int deltaOctaves)
{
    octaveShiftOctaves = juce::jlimit(-3, 3, octaveShiftOctaves + deltaOctaves);
    refreshChildViews();
}

void MainEditorComponent::scrollStepGridPitch(int deltaSemitones)
{
    stepGrid.scrollPitchView(deltaSemitones);
}

void MainEditorComponent::zoomStepGrid(float factor)
{
    stepGrid.zoomVertical(factor);
    stepGrid.zoomHorizontal(factor);
}

void MainEditorComponent::toggleInputMode()
{
    auto newMode = midiInputRouter.getMode() == MidiInputMode::StepRecord
                       ? MidiInputMode::PlayMonitor
                       : MidiInputMode::StepRecord;
    midiInputRouter.setMode(newMode);
    refreshChildViews();
}

void MainEditorComponent::cycleHumDuration(int delta)
{
    auto numPresets = (int) (sizeof(humDurationPresets) / sizeof(humDurationPresets[0]));
    humDurationPresetIndex = juce::jlimit(0, numPresets - 1, humDurationPresetIndex + delta);
    refreshChildViews();
}

void MainEditorComponent::commitHumNote()
{
    if (currentHumNote < 0)
        return; // nothing currently sounding from the hum monitor

    auto durationSteps = humDurationPresets[(size_t) humDurationPresetIndex];
    auto shiftedNote = juce::jlimit(0, 127, currentHumNote + octaveShiftOctaves * 12);

    // Written as a genuine contiguous chain (one note-start step + N-1
    // explicit tiedFromPrevious continuation steps), never as a single Step
    // with lengthInSteps > 1 -- a single long step left the intermediate
    // grid slots untouched, and if a later action (e.g. tieCurrentStep)
    // wrote a NEW tied step past that gap, the gap itself wasn't marked
    // tiedFromPrevious, which silently broke the contiguous-chain walk both
    // PlaybackEngine::scheduleUpTo and StepGridComponent's rendering rely
    // on -- the note played/drew as if it were only 1 step long.
    ensureStepExists(cursorTrackIndex, cursorStepIndex);
    Step noteStep;
    noteStep.notes.push_back({ shiftedNote, 0.8f });
    project.tracks[(size_t) cursorTrackIndex].clip.steps[(size_t) cursorStepIndex] = noteStep;

    for (int i = 1; i < durationSteps; ++i)
    {
        ensureStepExists(cursorTrackIndex, cursorStepIndex + i);
        Step tieStep;
        tieStep.tiedFromPrevious = true;
        project.tracks[(size_t) cursorTrackIndex].clip.steps[(size_t) (cursorStepIndex + i)] = tieStep;
    }

    moveCursor(durationSteps);
}

void MainEditorComponent::refreshChildViews()
{
    juce::StringArray instrumentNames;
    for (int i = 0; i < (int) project.tracks.size(); ++i)
    {
        auto* instrument = playbackEngine.getTrackInstrument(i);
        instrumentNames.add(instrument != nullptr ? instrument->getName() : juce::String());
    }

    trackList.setTracks(project.tracks, cursorTrackIndex, instrumentNames);
    stepGrid.setClip(&project.tracks[(size_t) cursorTrackIndex].clip, cursorStepIndex);

    transportBar.setPlaying(playbackEngine.isPlaying());
    transportBar.setBpm(project.tempoBpm);
    transportBar.setMode(midiInputRouter.getMode());
    transportBar.setOctaveShift(octaveShiftOctaves);

    updateHumDisplays();
}

void MainEditorComponent::updateHumDisplays()
{
    // Preview shows the note as it will actually be written -- i.e. after
    // the same octave shift commitHumNote() applies -- not the raw detected
    // pitch, so the preview position matches where it'll really land.
    if (currentHumNote >= 0)
    {
        auto shiftedNote = juce::jlimit(0, 127, currentHumNote + octaveShiftOctaves * 12);
        stepGrid.setPreviewNote(shiftedNote);
        stepGrid.centerPitchView(shiftedNote); // auto-scroll so the hum pitch stays on screen
    }
    else
    {
        stepGrid.setPreviewNote(-1);
    }

    transportBar.setHumStatus(humInputListener.isActive(), currentHumNote, humDurationPresets[(size_t) humDurationPresetIndex]);
}

bool MainEditorComponent::keyPressed(const juce::KeyPress& key)
{
    DBG("keyPressed: textChar='" << (char) key.getTextCharacter() << "' keyCode=" << key.getKeyCode()
        << " shift=" << (int) key.getModifiers().isShiftDown()
        << " cmd=" << (int) key.getModifiers().isCommandDown()
        << " isKeyCode3=" << (int) key.isKeyCode('3')
        << " isKeyCodeE=" << (int) key.isKeyCode('E'));

    // Left-hand-only layout: the right hand stays on the MIDI keyboard the
    // whole time, so every command here lives on the QWERTY left side.
    // d/f (step left/right) let the hand stay put and just rock two fingers
    // sideways. Track prev/next is Cmd+3/Cmd+E (directly above d), Cmd-
    // modified to stay consistent with the same shortcut in the Instrument
    // panel's search box (where plain 3/e can't be used -- they're needed
    // for typing search text there). Plain (unmodified) 3/e are free
    // elsewhere in the main editor, so they're reused there for pitch nudge
    // on the note at the cursor: 3 = up a semitone, e = down (opposite of
    // Cmd+3/Cmd+E's track direction -- picked by ear, not for consistency
    // with the Cmd-modified pair).
    // Every branch below routes through trigger() so the shortcut help bar's
    // "last action" indicator always shows what was just pressed and what
    // it did (e.g. "Cmd+E - Next Track") -- separate from the always-visible
    // static shortcut list, this is live, per-keypress feedback.
    auto trigger = [this](const juce::String& label, const std::function<void()>& action)
    {
        shortcutHelpBar.setLastAction(label);
        action();
        return true;
    };

    if (key.getModifiers().isCommandDown())
    {
        if (key.isKeyCode('S') && key.getModifiers().isShiftDown())
            return trigger("Cmd+Shift+S - Save As", [this] { saveProjectAs(); });
        if (key.isKeyCode('S'))
            return trigger("Cmd+S - Save", [this] { saveProject(); });
        if (key.isKeyCode('O') || key.isKeyCode('0')) // '0' aliased: easy to mis-press next to 'O'
            return trigger("Cmd+O - Open", [this] { openProject(); });
        if (key.isKeyCode('N'))
            return trigger("Cmd+N - New", [this] { newProject(); });
        if (key.isKeyCode('T'))
            return trigger("Cmd+T - Add Track", [this] { addTrack(); });
        if (key.isKeyCode('Y')) // moved off Cmd+I, used infrequently enough that the reach is fine
            return trigger("Cmd+Y - Instrument", [this] { openInstrumentPanel(); });
        if (key.isKeyCode(',')) // macOS's standard "Preferences" shortcut
            return trigger("Cmd+, - Audio/MIDI Settings", [this] { openAudioMidiSettings(); });
        if (key.isKeyCode('3')) // previous track (up)
            return trigger("Cmd+3 - Prev Track", [this] { switchTrack(-1); });
        if (key.isKeyCode('E')) // next track (down)
            return trigger("Cmd+E - Next Track", [this] { switchTrack(1); });
        if (key.isKeyCode('Z')) // zoom out on both axes
            return trigger("Cmd+Z - Zoom Out", [this] { zoomStepGrid(1.25f); });
        if (key.isKeyCode('X')) // zoom in on both axes
            return trigger("Cmd+X - Zoom In", [this] { zoomStepGrid(0.8f); });
        return false;
    }

    // Hum input: reuses the octave-shift Z/X and step-right F keys with
    // Shift instead of adding new plain keys ("同じキーを装飾で使い回して
    // 節約して" -- economize by reusing existing keys via a modifier).
    if (key.getModifiers().isShiftDown())
    {
        if (key.isKeyCode('Z')) // finer duration
            return trigger("Shift+Z - Finer Duration", [this] { cycleHumDuration(-1); });
        if (key.isKeyCode('X')) // coarser duration
            return trigger("Shift+X - Coarser Duration", [this] { cycleHumDuration(1); });
        if (key.isKeyCode('F')) // commit current hum pitch
            return trigger("Shift+F - Commit Hum Note", [this] { commitHumNote(); });
        if (key.isKeyCode('3')) // octave up on the note at the cursor -- adjustNotePitch takes any semitone delta
            return trigger("Shift+3 - Octave Up", [this] { adjustNotePitch(12); });
        if (key.isKeyCode('E')) // octave down on the note at the cursor
            return trigger("Shift+E - Octave Down", [this] { adjustNotePitch(-12); });
        return false;
    }

    if (key == juce::KeyPress::spaceKey)
        return trigger("Space - Rest", [this] { insertRestAndAdvance(); });
    if (key == juce::KeyPress::tabKey)
        return trigger("Tab - Play/Stop", [this] { togglePlayback(); });

    auto c = juce::CharacterFunctions::toLowerCase((juce::juce_wchar) key.getTextCharacter());

    switch (c)
    {
        case 'd': return trigger("d - Prev Note/Step", [this] { moveCursorByNoteOrStep(-1); });
        case 'f': return trigger("f - Next Note/Step", [this] { moveCursorByNoteOrStep(1); });
        case 'a': return trigger("a - Clear Step", [this] { clearCurrentStep(); });
        case 'g': return trigger("g - Delete+Retreat", [this] { deleteAndRetreat(); });
        case 't': return trigger("t - Tie", [this] { tieCurrentStep(); });
        case 'z': return trigger("z - Octave Down", [this] { shiftOctave(-1); });
        case 'x': return trigger("x - Octave Up", [this] { shiftOctave(1); });
        case 'c': return trigger("c - Toggle Mode", [this] { toggleInputMode(); });
        case 'v': return trigger("v - Toggle Hum", [this] { toggleHumInput(); }); // toggle hum-listening mode on/off
        case '3': return trigger("3 - Pitch Up", [this] { adjustNotePitch(1); });   // distinct from Cmd+3 track switch
        case 'e': return trigger("e - Pitch Down", [this] { adjustNotePitch(-1); }); // distinct from Cmd+E track switch
        case '1': return trigger("1 - Scroll Pitch Down", [this] { scrollStepGridPitch(-1); });
        case '2': return trigger("2 - Scroll Pitch Up", [this] { scrollStepGridPitch(1); });
        default: break;
    }

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
                    currentProjectFile = file;
                    cursorTrackIndex = 0;
                    cursorStepIndex = 0;
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
    playbackEngine.setTrackInstrument(0, nullptr); // clear any leftover instrument from the previous project

    project = Project{};
    project.tracks.push_back(Track{});
    currentProjectFile = juce::File();
    cursorTrackIndex = 0;
    cursorStepIndex = 0;

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
    micLevelMeter.setBounds(topRow.removeFromLeft(100).reduced(4));

    shortcutHelpBar.setBounds(bounds.removeFromBottom(20));

    transportBar.setBounds(bounds.removeFromTop(28));
    trackList.setBounds(bounds.removeFromLeft(200));
    stepGrid.setBounds(bounds);
}
