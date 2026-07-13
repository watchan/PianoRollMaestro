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
          cursorBefore(ownerIn.cursorStepIndex)
    {
    }

    ~StepEditGuard()
    {
        auto& stepsNow = owner.project.tracks[(size_t) trackIndex].clip.steps;
        if (stepsNow == before)
            return; // pure navigation, nothing to undo

        auto after = stepsNow;
        auto cursorAfter = owner.cursorStepIndex;
        auto* ownerPtr = &owner;
        auto trackIndexCopy = trackIndex;
        auto beforeCopy = before;
        auto cursorBeforeCopy = cursorBefore;

        // Without this, UndoManager merges every perform() into whatever
        // transaction is already open -- it only auto-starts a fresh one
        // for the very first perform() call ever made (newTransaction
        // defaults to true, but perform() clears it and never sets it back
        // on its own). Left out, undo() would revert the ENTIRE session's
        // edits in one shot instead of one note-edit command at a time.
        owner.undoManager.beginNewTransaction();
        owner.undoManager.perform(new LambdaUndoableAction(
            [ownerPtr, trackIndexCopy, after, cursorAfter]
            { ownerPtr->applyStepEdit(trackIndexCopy, after, cursorAfter); },
            [ownerPtr, trackIndexCopy, beforeCopy, cursorBeforeCopy]
            { ownerPtr->applyStepEdit(trackIndexCopy, beforeCopy, cursorBeforeCopy); }));
    }

private:
    MainEditorComponent& owner;
    int trackIndex;
    std::vector<Step> before;
    int cursorBefore;
};

void MainEditorComponent::applyStepEdit(int trackIndex, const std::vector<Step>& steps, int cursorStep)
{
    // Reassigning clip.steps while the audio thread might be concurrently
    // iterating that exact vector (scheduleUpTo(), which only runs while
    // playbackEngine.isPlaying()) is a genuine data race -- stop() first to
    // avoid it, same reasoning as addTrack()/newProject()/openProject().
    // But this runs on EVERY note commit (StepEditGuard pushes an undo
    // entry -> undoManager.perform() -> here, for every actual edit), so
    // calling stop() unconditionally also sent All-Notes-Off/All-Sound-Off
    // to every track's synth on every single 'f' press -- including
    // whatever's still physically held on the MIDI keyboard's live
    // monitor, cutting it off mid-note. Skipped entirely while not
    // playing: there's no concurrent audio-thread access to guard against
    // then (the overwhelmingly common case -- editing with the transport
    // stopped), so there's nothing to protect and no reason to kill the
    // live monitor.
    if (playbackEngine.isPlaying())
        playbackEngine.stop();

    project.tracks[(size_t) trackIndex].clip.steps = steps;
    cursorTrackIndex = trackIndex;
    cursorStepIndex = cursorStep;
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

    addAndMakeVisible(playButton);
    playButton.onClick = [this] { togglePlayback(); grabKeyboardFocus(); };

    addAndMakeVisible(instrumentButton);
    instrumentButton.onClick = [this] { openInstrumentPanel(); };

    addAndMakeVisible(audioSettingsButton);
    audioSettingsButton.onClick = [this] { openAudioMidiSettings(); };

    addAndMakeVisible(micLevelMeter);

    // Real MIDI keyboard input is "just a controller" that feeds the exact
    // same pending-chord-to-commit pipeline hum input does -- it's purely a
    // live monitor, never auto-writing to the step grid. Unlike hum
    // (monophonic), it can hold a chord -- see handleMidiNoteChange().
    // Committing is always the explicit action in handleForwardKey()
    // (plain 'f'), for whichever source (hum or MIDI) was heard from most
    // recently.
    midiInputRouter.onLiveNote = [this](int noteNumber, float velocity, bool isOn)
    {
        handleMidiNoteChange(noteNumber, velocity, isOn);
    };
    midiInputRouter.onLiveControllerMessage = [this](const juce::MidiMessage& message)
    {
        playbackEngine.liveMidiMessage(cursorTrackIndex, message);
    };

    // 'v' toggles hum listening on/off (see toggleHumInput()). Deliberately
    // NOT routed through midiInputRouter.injectNote() -- humming is monitor-
    // only (a continuously updating live tone), it never auto-writes to the
    // step grid either, same as MIDI above.
    humInputListener.onNoteChange = [this](int noteNumber, float velocity, bool isOn)
    {
        handleHumNoteChange(noteNumber, velocity, isOn);
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
    pollVirtualKeyboardInput();

    // stepGrid's pan/zoom can change via calls that don't go through
    // refreshChildViews() (zoomStepGridHorizontal() etc. call straight into
    // stepGrid) -- repainting every tick keeps chordEstimateBar's bar labels
    // aligned to stepGrid's current view without hooking every such call site.
    chordEstimateBar.repaint();

    if (!playbackEngine.isPlaying())
    {
        stepGrid.setPlaybackStep(-1);
        return;
    }

    // Per-track step position, NOT the global sample clock -- a launched
    // Session View clip loops on its own track cursor independently of the
    // transport's single global position (see getTrackPlaybackStep()'s
    // comment), so deriving the playhead from the global sample count made
    // it run straight past a looping clip's own boundary instead of
    // wrapping with it.
    stepGrid.setPlaybackStep(playbackEngine.getTrackPlaybackStep(cursorTrackIndex));
}

void MainEditorComponent::pollVirtualKeyboardInput()
{
    auto mods = juce::ModifierKeys::getCurrentModifiers();
    // Ctrl+Shift is the drum grid; plain Ctrl (no Shift) is the melodic
    // keyboard -- mutually exclusive even though several physical keys are
    // shared between the two maps (see virtualDrumKeyMap()'s comment), so
    // holding Shift never lets one keypress trigger both at once.
    auto melodicActive = mods.isCtrlDown() && !mods.isShiftDown();
    auto drumActive = mods.isCtrlDown() && mods.isShiftDown();

    // Ctrl+F ("Sustain") -- 'F' is unmapped in both virtualKeyboardKeyMap()
    // and virtualDrumKeyMap(), so it's free to hold alongside actual note
    // keys without colliding. Melodic-only (sustain doesn't really apply to
    // one-shot drum hits).
    auto sustainKeyDown = melodicActive && juce::KeyPress::isKeyCurrentlyDown('F');
    auto sustainActive = sustainKeyDown;
    // Rising edge only (not held-down-every-tick), same reasoning as the
    // note-press highlight below -- otherwise F would dominate
    // lastPressedKeyCode for as long as it's held, drowning out whatever
    // note keys get pressed while sustaining.
    if (sustainKeyDown && !wasSustainKeyDown)
        lastPressedKeyCode = 'F';
    wasSustainKeyDown = sustainKeyDown;

    // Shared diff-and-fire logic for one map: figures out which of its keys
    // are currently down (given whether this map is even "active" this
    // poll), fires injectNote() for anything newly pressed/released versus
    // last poll, and remembers each held key's ACTUAL sounded pitch (not
    // just the key) so a note-off always targets the same pitch its note-on
    // used, even if virtualKeyboardTransposeSemitones changes while it's
    // still held (same reasoning as liveNote()'s ActiveLiveNote tracking).
    // sustainActive/sustained implement a real sustain-pedal gesture: a key
    // release while sustain is held doesn't send note-off immediately, it
    // moves into `sustained` and keeps ringing until sustain itself
    // releases (or the same key is struck again, which un-sustains it and
    // retriggers a fresh note-on -- pressing a still-ringing sustained key
    // again behaves like a real piano's sustain pedal + retrike).
    auto pollOneMap = [this](const std::map<char, int>& keyMap, bool active, int baseNote,
                              std::vector<std::pair<char, int>>& held,
                              bool sustain, std::vector<std::pair<char, int>>& sustained)
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

            sustained.erase(std::remove_if(sustained.begin(), sustained.end(),
                                            [ch = ch](auto& s) { return s.first == ch; }),
                             sustained.end());
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

            if (sustain)
                sustained.push_back({ ch, pitch });
            else
                midiInputRouter.injectNote(pitch, 0.0f, false);
        }

        held = std::move(currentlyDown);

        // Sustain just released -- flush every note it was still holding over.
        if (!sustain && !sustained.empty())
        {
            for (auto& [ch, pitch] : sustained)
                midiInputRouter.injectNote(pitch, 0.0f, false);
            sustained.clear();
        }
    };

    pollOneMap(virtualKeyboardKeyMap(), melodicActive, 60 + virtualKeyboardTransposeSemitones,
               heldVirtualKeyboardKeys, sustainActive, sustainedVirtualKeyboardNotes);
    pollOneMap(virtualDrumKeyMap(), drumActive, 48, heldVirtualDrumKeys, false, sustainedVirtualDrumNotes);
}

void MainEditorComponent::adjustVirtualKeyboardTranspose(int deltaSemitones)
{
    virtualKeyboardTransposeSemitones = juce::jlimit(-48, 48, virtualKeyboardTransposeSemitones + deltaSemitones);
}

void MainEditorComponent::adjustVirtualKeyboardVelocity(float delta)
{
    virtualKeyboardVelocity = juce::jlimit(0.0f, 1.0f, virtualKeyboardVelocity + delta);
    refreshChildViews();
}

void MainEditorComponent::toggleHumInput()
{
    // Deliberately does NOT clear pendingChord when turning off --
    // pendingChord is shared with MIDI keyboard input (both are just
    // "controllers" feeding the same commit slot), so turning hum listening
    // off shouldn't wipe out a chord that came from the MIDI keyboard instead.
    humInputListener.setActive(!humInputListener.isActive());
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
                        // The old instance (if any) is about to be destroyed --
                        // any editor window still pointing at it must go first.
                        invalidatePluginEditorWindow(trackIndex);
                        playbackEngine.setTrackInstrument(trackIndex, std::move(instance));

                        // Show the freshly loaded plugin's editor right away.
                        if (auto* newInstrument = playbackEngine.getTrackInstrument(trackIndex))
                        {
                            pluginEditorWindowsByTrack[trackIndex] =
                                std::make_unique<PluginEditorWindow>(*newInstrument, newInstrument->getName());
                            pluginEditorDesiredVisible[trackIndex] = true;
                            updatePluginEditorWindowVisibility(true);
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
                    window = std::make_unique<PluginEditorWindow>(*instrument, instrument->getName());
                pluginEditorDesiredVisible[trackIndex] = true;
                updatePluginEditorWindowVisibility(true);
            }
        },
        [this, trackIndex]
        {
            invalidatePluginEditorWindow(trackIndex);
            playbackEngine.setTrackInstrument(trackIndex, nullptr);
            refreshChildViews();
            instrumentPanelWindow = nullptr;
        });
}

void MainEditorComponent::openAudioMidiSettings()
{
    audioMidiSettingsWindow = std::make_unique<AudioMidiSettingsWindow>(deviceManager, micDeviceManager);
}

void MainEditorComponent::toggleKeyboardOverlay()
{
    if (keyboardOverlayWindow == nullptr)
    {
        keyboardOverlayWindow = std::make_unique<KeyboardOverlayWindow>(
            [this] { return currentViewMode == ViewMode::Session; },
            [this] { return humInputListener.isActive(); },
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
            });
        return;
    }

    keyboardOverlayWindow->setVisible(!keyboardOverlayWindow->isVisible());
}

void MainEditorComponent::togglePluginEditor()
{
    auto* instrument = playbackEngine.getTrackInstrument(cursorTrackIndex);
    if (instrument == nullptr)
        return; // no plugin loaded on the current track

    auto& window = pluginEditorWindowsByTrack[cursorTrackIndex];
    if (window == nullptr)
        window = std::make_unique<PluginEditorWindow>(*instrument, instrument->getName());

    pluginEditorDesiredVisible[cursorTrackIndex] = !pluginEditorDesiredVisible[cursorTrackIndex];
    updatePluginEditorWindowVisibility(true);
}

void MainEditorComponent::updatePluginEditorWindowVisibility(bool allowStealFocus)
{
    for (auto& [trackIndex, window] : pluginEditorWindowsByTrack)
    {
        if (window == nullptr)
            continue;

        auto shouldShow = trackIndex == cursorTrackIndex && pluginEditorDesiredVisible[trackIndex];
        window->setVisible(shouldShow);
        if (shouldShow && allowStealFocus)
            window->toFront(true);
    }

    // setVisible(true) on a previously-hidden native window can still make
    // it key/frontmost at the OS level even without an explicit toFront()
    // call -- reclaim keyboard focus for the main editor whenever this
    // wasn't a deliberate "show the plugin" action, so track-switching
    // never silently breaks every other keyboard shortcut.
    if (!allowStealFocus)
        grabKeyboardFocus();
}

void MainEditorComponent::invalidatePluginEditorWindow(int trackIndex)
{
    pluginEditorWindowsByTrack.erase(trackIndex);
    pluginEditorDesiredVisible.erase(trackIndex);
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

void MainEditorComponent::handleHumNoteChange(int noteNumber, float velocity, bool isOn)
{
    // Hum is inherently monophonic (one pitch at a time from the YIN
    // detector), so a new pitch always REPLACES pendingChord entirely,
    // unlike MIDI's held-notes chord tracking below.
    if (isOn)
    {
        pendingChord = { { noteNumber, velocity } };
        pendingChordSource = PendingChordSource::Hum;
    }

    liveNote(noteNumber, velocity, isOn);

    // Update the status text AND the step-grid preview outlines right here,
    // the instant a new pitch arrives -- not just whenever refreshChildViews()
    // happens to run for some unrelated reason (e.g. cursor movement).
    // Otherwise the preview only reflects reality after some later action,
    // defeating its purpose of showing what 'f' would commit before you
    // actually press it.
    updatePendingNoteDisplays();
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
            pendingChord.clear();
        pendingChordSource = PendingChordSource::Midi;

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

void MainEditorComponent::handleForwardKey()
{
    StepEditGuard undoGuard(*this);

    // Ableton-Live-style step input: if there's a pending chord waiting to
    // be placed (from hum OR the MIDI keyboard -- both just feed the same
    // pendingChord slot) AND input mode ('v') is armed, 'f' places it
    // instead of just navigating. Gated on 'v' so a stray note played on the
    // MIDI keyboard while not actively recording can't get committed by a
    // 'f' press that was only meant to move the cursor. Otherwise it's plain
    // note-aware forward navigation -- see handleBackwardKey() for the same
    // gating on delete.
    if (!pendingChord.empty() && humInputListener.isActive())
    {
        commitPendingNote();
        return;
    }

    moveCursorByNoteOrStep(1);
}

void MainEditorComponent::handleBackwardKey()
{
    StepEditGuard undoGuard(*this);

    // 'd' is plain backward navigation by default. It only deletes instead
    // when input mode ('v') is armed AND the cursor sits on an existing note
    // that shares at least one pitch with the currently pending chord (last
    // heard from hum/MIDI) -- i.e. you're holding/just played that exact
    // pitch and pressing 'd' removes the matching pitch(es) here, like
    // Ableton's toggle-off gesture. Only the matching pitch(es) come out of
    // the chord; any other notes sharing that step are left sounding. With
    // 'v' off, or a note with no pitch in common, 'd' just navigates past it
    // -- see handleForwardKey() for the same gating on commit.
    if (!pendingChord.empty() && humInputListener.isActive())
    {
        auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
        auto ownerIndex = findOwningNoteStepIndex(steps, cursorStepIndex);

        if (ownerIndex >= 0)
        {
            auto& ownerNotes = steps[(size_t) ownerIndex].notes;

            auto removedAny = false;
            for (auto& pending : pendingChord)
            {
                auto shiftedPending = juce::jlimit(0, 127, pending.pitch + octaveShiftOctaves * 12);
                for (auto it = ownerNotes.begin(); it != ownerNotes.end(); ++it)
                {
                    if (it->pitch == shiftedPending)
                    {
                        ownerNotes.erase(it);
                        removedAny = true;
                        break;
                    }
                }
            }

            if (removedAny)
            {
                // Nothing left in this note at all -- clean up its tied
                // continuation steps too, same as a full delete used to.
                if (ownerNotes.empty())
                    deleteWholeNoteAt(ownerIndex);

                cursorStepIndex = ownerIndex;
                refreshChildViews();
                return;
            }
        }
    }

    moveCursorByNoteOrStep(-1);
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

void MainEditorComponent::advanceByDuration()
{
    // Pure navigation -- doesn't touch step content at all, so whatever's
    // already at the cursor (a note or a rest) is left exactly as it was.
    // Advances by the current duration preset (Shift+Z/X), same amount
    // commitPendingNote()/tieCurrentStep() use, so the locator lands at the
    // next beat position consistently regardless of what's under it.
    moveCursor(humDurationPresets[(size_t) humDurationPresetIndex]);
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
    auto ownerIndex = findOwningNoteStepIndex(steps, cursorStepIndex);

    if (ownerIndex >= 0)
    {
        // Only the effectively-selected pitches come out of the chord (see
        // effectiveSelectedPitches()) -- when nothing's been narrowed
        // (navigateNoteSelection()), that's every pitch in the chord, so
        // this clears the WHOLE note exactly like before this feature
        // existed. Mirrors handleBackwardKey()'s matching-pitch removal.
        auto& ownerNotes = steps[(size_t) ownerIndex].notes;
        auto selected = effectiveSelectedPitches();

        for (auto pitch : selected)
            for (auto it = ownerNotes.begin(); it != ownerNotes.end(); ++it)
                if (it->pitch == pitch) { ownerNotes.erase(it); break; }

        // Nothing left in this note at all -- clean up its tied
        // continuation steps too (root + every tied continuation step), not
        // just whichever single step the cursor happens to be on. Clearing
        // only the root left its tied continuations behind as orphans --
        // tiedFromPrevious=true, no notes of their own, no longer owned by
        // any note-start -- invisible everywhere that skips tied/empty
        // steps (the grid, ChordEstimator) but never removed either, since
        // trimTrailingEmptySteps() deliberately leaves tiedFromPrevious
        // steps alone (correct for a real note's sustain, wrong once its
        // root is gone). That's the "ゴミ" -- garbage nothing cleaned up.
        if (ownerNotes.empty())
            deleteWholeNoteAt(ownerIndex);

        noteSelectionAnchorStep = -1; // content changed -- fall back to whole-chord next time
    }
    else if (cursorStepIndex < (int) steps.size())
    {
        steps[(size_t) cursorStepIndex] = Step{};
    }
    // else: cursorStepIndex is already past the end of the array -- already
    // an implicit rest, nothing to clear, no need to pad the vector for it.

    // Also discards whatever's currently pending (hum or MIDI) -- otherwise
    // there was no way to cancel a stray/misdetected pitch short of
    // humming/playing a new (correct) one to overwrite it, or committing
    // the wrong one with 'f'.
    pendingChord.clear();
    pendingChordSource = PendingChordSource::None;

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
    auto ownerIndex = findOwningNoteStepIndex(steps, cursorStepIndex);
    if (ownerIndex < 0)
        return; // rest -- nothing here to adjust

    auto& step = steps[(size_t) ownerIndex];
    auto selected = effectiveSelectedPitches(); // whole chord unless narrowed (see its declaration)
    for (auto& note : step.notes)
        if (std::find(selected.begin(), selected.end(), note.pitch) != selected.end())
            note.pitch = juce::jlimit(0, 127, note.pitch + deltaSemitones);

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

void MainEditorComponent::tieCurrentStep()
{
    StepEditGuard undoGuard(*this);

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
    // "unset" sentinel, so this doubles as the "clear it" gesture.
    project.tracks[(size_t) cursorTrackIndex].clip.explicitLengthInSteps = cursorStepIndex;
    refreshChildViews();
}

void MainEditorComponent::toggleMetronome()
{
    project.metronomeEnabled = !project.metronomeEnabled;
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

void MainEditorComponent::nudgeHumPitch(int deltaSemitones)
{
    humSemitoneNudge = juce::jlimit(-24, 24, humSemitoneNudge + deltaSemitones);
    refreshChildViews(); // re-applies the new nudge to the live preview immediately
}

int MainEditorComponent::shiftedPendingPitch(int rawPitch) const
{
    auto semitones = octaveShiftOctaves * 12 + (pendingChordSource == PendingChordSource::Hum ? humSemitoneNudge : 0);
    return juce::jlimit(0, 127, rawPitch + semitones);
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
    stepGrid.zoomVertical(factor);
}

void MainEditorComponent::cycleHumDuration(int delta)
{
    auto numPresets = (int) (sizeof(humDurationPresets) / sizeof(humDurationPresets[0]));
    humDurationPresetIndex = juce::jlimit(0, numPresets - 1, humDurationPresetIndex + delta);
    refreshChildViews();
}

void MainEditorComponent::commitPendingNote()
{
    if (pendingChord.empty())
        return; // nothing pending to commit

    auto durationSteps = humDurationPresets[(size_t) humDurationPresetIndex];
    auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
    auto ownerIndex = findOwningNoteStepIndex(steps, cursorStepIndex);

    if (ownerIndex >= 0)
    {
        // A note already occupies this position (whole or via a tied
        // continuation) -- merge the pending note(s) into it as a chord
        // instead of overwriting/deleting what was already there.
        auto& ownerNotes = steps[(size_t) ownerIndex].notes;
        for (auto& n : pendingChord)
            ownerNotes.push_back({ shiftedPendingPitch(n.pitch), n.velocity });

        moveCursor(durationSteps);
        return;
    }

    // Nothing here yet -- write a fresh note. Written as a genuine
    // contiguous chain (one note-start step + N-1 explicit tiedFromPrevious
    // continuation steps), never as a single Step with lengthInSteps > 1 --
    // a single long step left the intermediate grid slots untouched, and if
    // a later action (e.g. tieCurrentStep) wrote a NEW tied step past that
    // gap, the gap itself wasn't marked tiedFromPrevious, which silently
    // broke the contiguous-chain walk both PlaybackEngine::scheduleUpTo and
    // StepGridComponent's rendering rely on -- the note played/drew as if it
    // were only 1 step long.
    ensureStepExists(cursorTrackIndex, cursorStepIndex);
    Step noteStep;
    noteStep.notes.reserve(pendingChord.size());
    for (auto& n : pendingChord)
        noteStep.notes.push_back({ shiftedPendingPitch(n.pitch), n.velocity });
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
    stepGrid.setSelectedPitches(humInputListener.isActive() ? std::vector<int>{} : effectiveSelectedPitches());

    transportBar.setPlaying(playbackEngine.isPlaying());
    transportBar.setBpm(project.tempoBpm);
    transportBar.setOctaveShift(octaveShiftOctaves);
    transportBar.setVirtualKeyboardVelocity(virtualKeyboardVelocity);
    transportBar.setHumInputActive(humInputListener.isActive());
    transportBar.setLoopEnabled(project.loopEnabled);
    transportBar.setMetronomeEnabled(project.metronomeEnabled);

    updatePendingNoteDisplays();
    updateStepGridScale();
    updateChordEstimates();
    shortcutHelpBar.setViewMode(currentViewMode == ViewMode::Session);

    if (currentViewMode == ViewMode::Session)
        sessionGrid.setTracks(project.tracks, cursorTrackIndex, sessionCursorSlotIndex);
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
    auto stepsPerQuarterNote = project.tracks.empty() ? 12 : project.tracks[0].clip.stepsPerQuarterNote;
    auto halfBeatLengthInSteps = juce::jmax(1, stepsPerQuarterNote / 2); // 0.5-beat analysis granularity
    chordEstimateBar.setChords(ChordEstimator::estimate(project, halfBeatLengthInSteps));
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

    stepGrid.setPreviewNotes(shiftedPitches);
    if (!shiftedPitches.empty())
        stepGrid.centerPitchView(shiftedPitches.front()); // auto-scroll so the pending pitch stays on screen

    transportBar.setPendingNoteStatus(shiftedPitches, humDurationPresets[(size_t) humDurationPresetIndex]);
}

void MainEditorComponent::updateStepGridScale()
{
    std::array<bool, 12> inScale {};
    inScale.fill(currentScaleType == ScaleType::Off); // Off = every row treated as "in scale" (no tint difference)

    if (currentScaleType != ScaleType::Off)
    {
        static constexpr int majorIntervals[] = { 0, 2, 4, 5, 7, 9, 11 };
        static constexpr int naturalMinorIntervals[] = { 0, 2, 3, 5, 7, 8, 10 };

        auto* intervals = currentScaleType == ScaleType::Major ? majorIntervals : naturalMinorIntervals;
        for (int i = 0; i < 7; ++i)
            inScale[(size_t) ((scaleRootPitchClass + intervals[i]) % 12)] = true;
    }

    stepGrid.setScale(inScale);
}

void MainEditorComponent::cycleScale()
{
    currentScaleType = currentScaleType == ScaleType::Major   ? ScaleType::NaturalMinor
                      : currentScaleType == ScaleType::NaturalMinor ? ScaleType::Off
                                                                     : ScaleType::Major;
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
    // Cmd, never Ctrl (see the top-of-file convention notes). The note keys
    // themselves aren't handled here at all -- keyPressed() has no matching
    // "key up" callback, so proper hold/release note-on/off is polled via
    // isKeyCurrentlyDown() instead. Ctrl+Z/X (transpose, melodic keyboard
    // only) and Ctrl+Shift+Z/X (velocity, both PC-keyboard note sources --
    // Z/X are unmapped in virtualDrumKeyMap(), so this doesn't collide with
    // any actual drum pad) are the only Ctrl combos actually handled here,
    // since they're discrete "press" actions rather than something that
    // needs to track held/released state. Returning true unconditionally
    // for every other Ctrl combo just claims it as handled -- returning
    // false left it "unhandled" as far as JUCE/macOS was concerned, which
    // triggered the OS's system beep.
    if (key.getModifiers().isCtrlDown())
    {
        if (key.getModifiers().isShiftDown())
        {
            if (key.isKeyCode('Z'))
                return trigger("Ctrl+Shift+Z - Velocity Down", [this] { adjustVirtualKeyboardVelocity(-0.1f); });
            if (key.isKeyCode('X'))
                return trigger("Ctrl+Shift+X - Velocity Up", [this] { adjustVirtualKeyboardVelocity(0.1f); });
        }
        else
        {
            if (key.isKeyCode('Z'))
                return trigger("Ctrl+Z - Transpose Down", [this] { adjustVirtualKeyboardTranspose(-1); });
            if (key.isKeyCode('X'))
                return trigger("Ctrl+X - Transpose Up", [this] { adjustVirtualKeyboardTranspose(1); });
        }
        return true;
    }

    if (key.getModifiers().isCommandDown())
    {
        // Checked before the plain Cmd+3/Cmd+E zoom bindings below --
        // piano-roll pitch-view scroll moved here off plain 3/e once those
        // became the hum semitone nudge (see nudgeHumPitch()). Cmd anchors
        // this as an app shortcut rather than a text-composition combo, so
        // (unlike plain Option+E) it isn't at risk of the macOS dead-key
        // interception issue noted elsewhere in this file.
        if (key.getModifiers().isAltDown() && key.isKeyCode('3'))
            return trigger("Cmd+Option+3 - Scroll Pitch Up", [this] { scrollStepGridPitch(1); });
        if (key.getModifiers().isAltDown() && key.isKeyCode('E'))
            return trigger("Cmd+Option+E - Scroll Pitch Down", [this] { scrollStepGridPitch(-1); });
        // Extend the individual-note selection (navigateNoteSelection(),
        // HUM-off only -- no-op while hum's active, same as plain 3/e).
        // 'W' is the primary bind for the "up" slot, not just an alias --
        // Cmd+Shift+3 is unusable, macOS reserves it system-wide for a
        // full-screen screenshot (arrives before the app ever sees it, same
        // category of problem as Cmd+M being reserved for window minimize).
        // 'R' aliases 'E' for the same dead-key reasons as Option+E elsewhere.
        if (key.getModifiers().isShiftDown() && key.isKeyCode('W'))
            return trigger("Cmd+Shift+W - Extend Selection Up", [this]
            { if (!humInputListener.isActive()) navigateNoteSelection(-1, true); });
        if (key.getModifiers().isShiftDown() && (key.isKeyCode('E') || key.isKeyCode('R')))
            return trigger("Cmd+Shift+E - Extend Selection Down", [this]
            { if (!humInputListener.isActive()) navigateNoteSelection(1, true); });
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
        if (key.isKeyCode('P')) // show/hide the current track's plugin editor window
            return trigger("Cmd+P - Plugin Editor", [this] { togglePluginEditor(); });
        if (key.isKeyCode(',')) // macOS's standard "Preferences" shortcut
            return trigger("Cmd+, - Audio/MIDI Settings", [this] { openAudioMidiSettings(); });
        if (key.isKeyCode('K')) // 'K' for "Keyboard" -- show/hide the live shortcut cheat-sheet window
            return trigger("Cmd+K - Keyboard Overlay", [this] { toggleKeyboardOverlay(); });
        if (key.isKeyCode('G')) // previous track -- moved off Cmd+R onto Cmd+G/B as a pair
            return trigger("Cmd+G - Prev Track", [this] { switchTrack(-1); });
        if (key.isKeyCode('B')) // next track -- moved off Cmd+E, which is now vertical zoom out
            return trigger("Cmd+B - Next Track", [this] { switchTrack(1); });
        if (key.isKeyCode('3')) // vertical zoom out -- moved off Cmd+3's old track-switch role
            return trigger("Cmd+3 - Zoom Out (Vertical)", [this] { zoomStepGridVertical(1.25f); });
        if (key.isKeyCode('E')) // vertical zoom in -- moved off Cmd+E's old track-switch role
            return trigger("Cmd+E - Zoom In (Vertical)", [this] { zoomStepGridVertical(0.8f); });
        if (key.isKeyCode('F')) // horizontal zoom in -- Cmd+F no longer commits (that's plain 'f' now)
            return trigger("Cmd+F - Zoom In (Horizontal)", [this] { zoomStepGridHorizontal(0.8f); });
        if (key.isKeyCode('D')) // horizontal zoom out
            return trigger("Cmd+D - Zoom Out (Horizontal)", [this] { zoomStepGridHorizontal(1.25f); });
        if (key.isKeyCode('M')) // cycle piano-roll scale tint: Major -> Natural Minor -> Off
            return trigger("Cmd+M - Cycle Scale", [this] { cycleScale(); });
        if (key.isKeyCode('C')) // drop the loop END marker at the cursor -- Shift+C does the start marker. 'C' for "Cycle" (Ableton/Cubase's name for loop playback), and left-hand -- 'L' was rejected for landing on the right-hand side of the keyboard, breaking the left-hand-only rule.
            return trigger("Cmd+C - Set Loop End", [this] { setLoopEndHere(); });
        if (key.isKeyCode('A')) // toggle the current track's inclusion in ChordEstimator's pooled analysis -- 'A' for "Analysis"
            return trigger("Cmd+A - Toggle Chord Track", [this] { toggleChordEstimateForCurrentTrack(); });
        // Standard macOS undo/redo shortcuts (Cmd, not Ctrl, for consistency
        // with every other modifier in this app -- see the top-of-file
        // convention notes). Scoped to note edits only, see StepEditGuard.
        if (key.isKeyCode('Z') && key.getModifiers().isShiftDown())
            return trigger("Cmd+Shift+Z - Redo", [this] { performRedo(); });
        if (key.isKeyCode('Z'))
            return trigger("Cmd+Z - Undo", [this] { performUndo(); });
        return false;
    }

    // Octave shift on the note at the cursor: Shift+Option+3/E (checked
    // before the plain-Shift and plain-Option blocks below so this more
    // specific combo takes priority). NOTE: Shift+digit combos (Shift+3
    // alone) were previously found to never reach the app at all here
    // (intercepted by the OS/active input source before JUCE saw them) --
    // if Shift+Option+3 has the same problem, 'W' is a working substitute
    // for '3' throughout this app's key map for exactly that reason.
    if (key.getModifiers().isShiftDown() && key.getModifiers().isAltDown())
    {
        if (key.isKeyCode('3') || key.isKeyCode('W'))
            return trigger("Shift+Option+3/W - Octave Up", [this] { adjustNotePitch(12); });
        // 'R' is also accepted for 'E' here -- Option+E is a dead key on the
        // standard US layout (Option+E waits for a second keystroke to
        // compose an accented character, e.g. Option+E then A -> "á"),
        // so the OS's text input system swallows the combo before JUCE ever
        // sees a normal keyDown for it. Same root cause as the Shift+digit
        // issue elsewhere in this file, different trigger (dead-key
        // composition vs. IME digit interception) -- same fix shape: offer
        // a working alias on an adjacent, non-dead key.
        if (key.isKeyCode('E') || key.isKeyCode('R'))
            return trigger("Shift+Option+E/R - Octave Down", [this] { adjustNotePitch(-12); });
        return false;
    }

    // Semitone nudge on the note at the cursor: Option+T/G (moved off plain
    // 3/e, which now scroll the piano-roll's visible pitch range instead).
    // Was originally Option+3 (up) / Option+E-or-R (down), but Option+E's
    // dead-key composition issue (see the Shift+Option block above) kept
    // tripping people up even with the 'R' alias -- T/G are a plain letter
    // pair with no OS/IME interception or dead-key history anywhere in this
    // file, and sit directly above/below each other on the keyboard (a
    // natural up/down feel), so there's no alias needed here at all.
    if (key.getModifiers().isAltDown())
    {
        if (key.isKeyCode('T'))
            return trigger("Option+T - Pitch Up", [this] { adjustNotePitch(1); });
        if (key.isKeyCode('G'))
            return trigger("Option+G - Pitch Down", [this] { adjustNotePitch(-1); });
        // Tempo: reuses the octave-preview Z (down) / X (up) keys with
        // Option, same "down/up" sense those already have for z/x, just
        // scoped to BPM instead of live-input octave.
        if (key.isKeyCode('Z'))
            return trigger("Option+Z - Tempo Down", [this] { adjustTempo(-1.0); });
        if (key.isKeyCode('X'))
            return trigger("Option+X - Tempo Up", [this] { adjustTempo(1.0); });
        return false;
    }

    // Hum duration cycling reuses the octave-shift Z/X keys with Shift
    // instead of adding new plain keys ("同じキーを装飾で使い回して節約して"
    // -- economize by reusing existing keys via a modifier).
    if (key.getModifiers().isShiftDown())
    {
        if (key.isKeyCode('Z')) // finer duration
            return trigger("Shift+Z - Finer Duration", [this] { cycleHumDuration(-1); });
        if (key.isKeyCode('X')) // coarser duration
            return trigger("Shift+X - Coarser Duration", [this] { cycleHumDuration(1); });
        // Move the locator by a full measure (4 beats, 4/4 assumed -- this
        // app has no separate time-signature field) -- a bigger jump than
        // d/f's note-aware navigation, for skipping ahead/back through a
        // song quickly regardless of what's under the cursor.
        if (key.isKeyCode('F'))
            return trigger("Shift+F - Jump Forward 1 Bar", [this]
            {
                moveCursor(4 * project.tracks[(size_t) cursorTrackIndex].clip.stepsPerQuarterNote);
            });
        if (key.isKeyCode('D'))
            return trigger("Shift+D - Jump Back 1 Bar", [this]
            {
                moveCursor(-4 * project.tracks[(size_t) cursorTrackIndex].clip.stepsPerQuarterNote);
            });
        // Track switch, same up/down feel as plain 3/e's pitch-scroll ('3' =
        // up = prev track, 'e' = down = next track). 'W' is also accepted
        // for '3' here -- Shift+digit combos were confirmed earlier to never
        // reach this app at all (intercepted by the OS/active input source
        // before JUCE sees them), same reasoning as Shift+Option+3/W.
        if (key.isKeyCode('3') || key.isKeyCode('W'))
            return trigger("Shift+3/W - Prev Track", [this] { switchTrack(-1); });
        if (key.isKeyCode('E'))
            return trigger("Shift+E - Next Track", [this] { switchTrack(1); });
        if (key.isKeyCode('C')) // drop the loop START marker at the cursor -- Cmd+C does the end marker
            return trigger("Shift+C - Set Loop Start", [this] { setLoopStartHere(); });
        return false;
    }

    if (key == juce::KeyPress::spaceKey)
        return trigger("Space - Advance", [this] { advanceByDuration(); });
    if (key == juce::KeyPress::tabKey)
        return trigger("Tab - Play/Stop", [this] { togglePlayback(); });

    auto c = juce::CharacterFunctions::toLowerCase((juce::juce_wchar) key.getTextCharacter());

    // 3/e/d/f/g/t/z/x are the only keys that mean something different in
    // Session View -- everything else in this switch (a,v,c,s) plus every
    // Space/Tab/Cmd/Shift/Option shortcut above stays identical in both
    // views. Session View's own scheme: 3/e move the track (row) cursor,
    // d/f move the slot (column) cursor -- mirroring how 3/e and d/f
    // already move things in the piano roll, just repurposed onto the
    // grid's two axes -- while z/x (stop/launch) and g/t (capture/load)
    // carry the actual clip actions, kept off the navigation keys so
    // moving the cursor around never accidentally launches or stops
    // anything.
    auto inSessionView = currentViewMode == ViewMode::Session;

    switch (c)
    {
        case 'd': return inSessionView
            ? trigger("d - Prev Slot", [this] { moveSessionCursor(-1); })
            : trigger("d - Delete Note/Prev", [this] { handleBackwardKey(); });
        case 'f': return inSessionView
            ? trigger("f - Next Slot", [this] { moveSessionCursor(1); })
            : trigger("f - Place Hum/Next", [this] { handleForwardKey(); });
        case 'a': return inSessionView
            ? trigger("a - Delete Clip", [this] { deleteClipAtCursor(); })
            : trigger("a - Clear Step", [this] { clearCurrentStep(); });
        case 'g': return inSessionView
            ? trigger("g - Capture to Slot", [this] { captureClipToSlotAtCursor(); })
            : trigger("g - Delete+Retreat", [this] { deleteAndRetreat(); });
        case 't': return inSessionView
            ? trigger("t - Load Slot to Editor", [this] { loadSlotAtCursorToEditor(); })
            : trigger("t - Tie", [this] { tieCurrentStep(); });
        case 'z': return inSessionView
            ? trigger("z - Stop Track", [this] { stopCurrentTrackSlot(); })
            : trigger("z - Octave Down", [this] { shiftOctave(-1); });
        case 'x': return inSessionView
            ? trigger("x - Launch Slot", [this] { launchSlotAtCursor(); })
            : trigger("x - Octave Up", [this] { shiftOctave(1); });
        case 'v': return trigger("v - Toggle Hum", [this] { toggleHumInput(); }); // toggle hum-listening mode on/off
        case 'c': return trigger("c - Toggle Loop", [this] { toggleLoopEnabled(); }); // 'C' for "Cycle" -- also left-hand, unlike the rejected 'l'
        case 's': return trigger("s - Toggle Session View", [this] { toggleViewMode(); });
        // PianoRoll 'B' for "Bound" -- marks the clip's end at the cursor so
        // a trailing rest can be kept intentionally instead of always being
        // trimmed away (see MidiClip::explicitLengthInSteps). Session View
        // repurposes it for "duplicate the clip at the cursor" (no step
        // cursor exists there for a clip-end marker to mean anything).
        case 'b': return inSessionView
            ? trigger("b - Duplicate Clip", [this] { duplicateClipAtCursor(); })
            : trigger("b - Set Clip End", [this] { setClipEndHere(); });
        case 'w': return trigger("w - Toggle Metronome", [this] { toggleMetronome(); });
        // Piano Roll, HUM OFF: 3/e narrow/navigate the individual-note
        // selection within the chord at the cursor instead of nudging hum
        // pitch (see navigateNoteSelection()) -- hum pitch correction only
        // means anything while hum is actually being listened to.
        case '3': return inSessionView
            ? trigger("3 - Prev Track", [this] { switchTrack(-1); })
            : humInputListener.isActive()
                // Nudges the HUM-detected pitch by a semitone, up -- a quick
                // correction for the YIN detector occasionally landing a
                // semitone off. Doesn't affect MIDI input (see shiftedPendingPitch()).
                // View-scroll (this key's old job) moved to Cmd+Option+3/E.
                ? trigger("3 - Hum Pitch Up", [this] { nudgeHumPitch(1); })
                : trigger("3 - Select Note Up", [this] { navigateNoteSelection(-1, false); });
        case 'e': return inSessionView
            ? trigger("e - Next Track", [this] { switchTrack(1); })
            : humInputListener.isActive()
                ? trigger("e - Hum Pitch Down", [this] { nudgeHumPitch(-1); })
                : trigger("e - Select Note Down", [this] { navigateNoteSelection(1, false); });
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
                    sessionCursorSlotIndex = 0;
                    // Old undo entries reference step data by track index
                    // into the project that just got replaced -- meaningless
                    // (and, if track counts differ, unsafe) to keep around.
                    undoManager.clearUndoHistory();
                    // Every existing plugin editor window points at an
                    // instrument that's about to be replaced/destroyed.
                    pluginEditorWindowsByTrack.clear();
                    pluginEditorDesiredVisible.clear();
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
    playbackEngine.setTrackInstrument(0, nullptr); // clear any leftover instrument from the previous project
    pluginEditorWindowsByTrack.clear(); // pointed at that now-destroyed instrument
    pluginEditorDesiredVisible.clear();

    project = Project{};
    project.tracks.push_back(Track{});
    currentProjectFile = juce::File();
    cursorTrackIndex = 0;
    cursorStepIndex = 0;
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
    micLevelMeter.setBounds(topRow.removeFromLeft(100).reduced(4));

    shortcutHelpBar.setBounds(bounds.removeFromBottom(48));

    transportBar.setBounds(bounds.removeFromTop(28));
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
