#pragma once
#include <JuceHeader.h>
#include "MidiInputRouter.h"
#include "PlaybackEngine.h"
#include "ProjectModel.h"
#include "StepGridComponent.h"
#include "TrackListComponent.h"
#include "TransportBarComponent.h"

class MainEditorComponent : public juce::AudioAppComponent
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
    void refreshMidiDeviceList();
    void midiDeviceSelected();
    void stepChordCaptured(const std::vector<StepNote>& notes);
    void liveNote(int noteNumber, float velocity, bool isOn);
    void togglePlayback();

    // Editing commands, all reachable with hands on the keyboard home row.
    void ensureStepExists(int trackIndex, int stepIndex);
    void moveCursor(int deltaSteps);
    void switchTrack(int deltaTracks);
    void insertRestAndAdvance();
    void deleteAndRetreat();
    void clearCurrentStep();
    void tieCurrentStep();
    void shiftOctave(int deltaOctaves);
    void toggleInputMode();

    // Persistence -- the only commands allowed to touch a mouse dialog.
    void saveProject();
    void saveProjectAs();
    void openProject();
    void newProject();
    void writeProjectToFile(const juce::File& file);

    void refreshChildViews();

    MidiInputRouter midiInputRouter;
    juce::ComboBox midiDeviceBox;
    juce::Array<juce::MidiDeviceInfo> availableMidiDevices;

    juce::TextButton playButton{ "Play" };
    PlaybackEngine playbackEngine;

    TransportBarComponent transportBar;
    TrackListComponent trackList;
    StepGridComponent stepGrid;

    Project project;
    juce::File currentProjectFile;
    std::unique_ptr<juce::FileChooser> fileChooser;

    int cursorTrackIndex = 0;
    int cursorStepIndex = 0;
    int octaveShiftOctaves = 0;
};
