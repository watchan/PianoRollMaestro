#include "MainEditorComponent.h"

MainEditorComponent::MainEditorComponent()
{
    project.tracks.push_back(Track{});
    playbackEngine.setProject(&project);

    addAndMakeVisible(transportBar);
    addAndMakeVisible(trackList);
    addAndMakeVisible(stepGrid);

    addAndMakeVisible(midiDeviceBox);
    midiDeviceBox.onChange = [this] { midiDeviceSelected(); };
    refreshMidiDeviceList();

    addAndMakeVisible(playButton);
    playButton.onClick = [this] { togglePlayback(); grabKeyboardFocus(); };

    addAndMakeVisible(instrumentButton);
    instrumentButton.onClick = [this] { openInstrumentPanel(); };

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

    setWantsKeyboardFocus(true);

    setSize(900, 600);
    setAudioChannels(0, 2);

    refreshChildViews();
}

MainEditorComponent::~MainEditorComponent()
{
    shutdownAudio();
}

void MainEditorComponent::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    playbackEngine.prepare(sampleRate, samplesPerBlockExpected);
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
}

void MainEditorComponent::releaseResources()
{
}

void MainEditorComponent::togglePlayback()
{
    if (playbackEngine.isPlaying())
        playbackEngine.stop();
    else
        playbackEngine.start();

    transportBar.setPlaying(playbackEngine.isPlaying());
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
    auto shiftedNote = juce::jlimit(0, 127, noteNumber + octaveShiftOctaves * 12);

    if (isOn)
        playbackEngine.liveNoteOn(cursorTrackIndex, shiftedNote, velocity);
    else
        playbackEngine.liveNoteOff(cursorTrackIndex, shiftedNote);
}

void MainEditorComponent::moveCursor(int deltaSteps)
{
    cursorStepIndex = juce::jmax(0, cursorStepIndex + deltaSteps);
    refreshChildViews();
}

void MainEditorComponent::switchTrack(int deltaTracks)
{
    auto numTracks = (int) project.tracks.size();
    cursorTrackIndex = juce::jlimit(0, numTracks - 1, cursorTrackIndex + deltaTracks);
    refreshChildViews();
}

void MainEditorComponent::addTrack()
{
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
    moveCursor(-1);
    ensureStepExists(cursorTrackIndex, cursorStepIndex);
    project.tracks[(size_t) cursorTrackIndex].clip.steps[(size_t) cursorStepIndex] = Step{};
    refreshChildViews();
}

void MainEditorComponent::clearCurrentStep()
{
    ensureStepExists(cursorTrackIndex, cursorStepIndex);
    project.tracks[(size_t) cursorTrackIndex].clip.steps[(size_t) cursorStepIndex] = Step{};
    refreshChildViews();
}

void MainEditorComponent::tieCurrentStep()
{
    if (cursorStepIndex <= 0)
        return;

    auto& steps = project.tracks[(size_t) cursorTrackIndex].clip.steps;
    if ((int) steps.size() <= cursorStepIndex - 1)
        return;

    auto& prev = steps[(size_t) (cursorStepIndex - 1)];
    if (prev.notes.empty() && !prev.tiedFromPrevious)
        return; // nothing sounding to extend

    ensureStepExists(cursorTrackIndex, cursorStepIndex);

    Step tieStep;
    tieStep.tiedFromPrevious = true;
    project.tracks[(size_t) cursorTrackIndex].clip.steps[(size_t) cursorStepIndex] = tieStep;

    moveCursor(1);
}

void MainEditorComponent::shiftOctave(int deltaOctaves)
{
    octaveShiftOctaves = juce::jlimit(-3, 3, octaveShiftOctaves + deltaOctaves);
    refreshChildViews();
}

void MainEditorComponent::toggleInputMode()
{
    auto newMode = midiInputRouter.getMode() == MidiInputMode::StepRecord
                       ? MidiInputMode::PlayMonitor
                       : MidiInputMode::StepRecord;
    midiInputRouter.setMode(newMode);
    refreshChildViews();
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
}

bool MainEditorComponent::keyPressed(const juce::KeyPress& key)
{
    // Left-hand-only layout: the right hand stays on the MIDI keyboard the
    // whole time, so every command here lives on the QWERTY left side.
    // d/f (step left/right) let the hand stay put and just rock two fingers
    // sideways. Track prev/next is Cmd+3/Cmd+E (directly above d), Cmd-
    // modified to stay consistent with the same shortcut in the Instrument
    // panel's search box (where plain 3/e can't be used -- they're needed
    // for typing search text there).
    if (key.getModifiers().isCommandDown())
    {
        if (key.isKeyCode('S') && key.getModifiers().isShiftDown()) { saveProjectAs(); return true; }
        if (key.isKeyCode('S')) { saveProject(); return true; }
        if (key.isKeyCode('O') || key.isKeyCode('0')) { openProject(); return true; } // '0' aliased: easy to mis-press next to 'O'
        if (key.isKeyCode('N')) { newProject(); return true; }
        if (key.isKeyCode('T')) { addTrack(); return true; }
        if (key.isKeyCode('I')) { openInstrumentPanel(); return true; }
        if (key.isKeyCode('3')) { switchTrack(-1); return true; }  // previous track (up)
        if (key.isKeyCode('E')) { switchTrack(1); return true; }   // next track (down)
        return false;
    }

    if (key == juce::KeyPress::spaceKey) { insertRestAndAdvance(); return true; }
    if (key == juce::KeyPress::tabKey) { togglePlayback(); return true; }

    auto c = juce::CharacterFunctions::toLowerCase((juce::juce_wchar) key.getTextCharacter());

    switch (c)
    {
        case 'd': moveCursor(-1); return true;      // step left
        case 'f': moveCursor(1); return true;       // step right
        case 'a': clearCurrentStep(); return true;  // clear, cursor stays
        case 'g': deleteAndRetreat(); return true;  // delete + retreat
        case 't': tieCurrentStep(); return true;
        case 'z': shiftOctave(-1); return true;
        case 'x': shiftOctave(1); return true;
        case 'c': toggleInputMode(); return true;
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

    transportBar.setBounds(bounds.removeFromTop(28));
    trackList.setBounds(bounds.removeFromLeft(200));
    stepGrid.setBounds(bounds);
}
