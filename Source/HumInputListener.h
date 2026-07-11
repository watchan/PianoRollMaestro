#pragma once
#include <JuceHeader.h>
#include <vector>
#include "PitchDetector.h"

// Captures microphone input on a raw AudioIODeviceCallback (registered via
// AudioDeviceManager::addAudioCallback, NOT AudioAppComponent's
// getNextAudioBlock -- that path aliases input samples into the output
// buffer memory via AudioSourcePlayer, which is confusing/fragile for a
// synth-style app that also needs to write real output). Runs YIN pitch
// detection on the input while active() and reports MIDI note changes.
class HumInputListener : public juce::AudioIODeviceCallback
{
public:
    void setActive(bool shouldBeActive);
    void setVelocitySensingEnabled(bool shouldBeEnabled);
    bool isActive() const { return active.load(); }

    // Peak input level (0..1) of the most recent audio block, updated
    // regardless of active() -- lets the UI show a level meter to confirm
    // the mic/channel selection is receiving signal even before hum
    // listening is toggled on.
    float getCurrentLevel() const { return currentLevel.load(); }

    // Fired on the message thread.
    std::function<void(int noteNumber, float velocity, bool isOn)> onNoteChange;

    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    void processAnalysisWindow();
    void setCurrentNote(int newNote); // audio-thread; hops note-change to message thread
    void forceNoteOff();              // audio-thread; used by setActive(false) and the sustain-limit safety cutoff below

    static constexpr float defaultVelocity = 0.8f;
    static constexpr int analysisWindowSize = 2048;

    // Safety cutoff: a false-positive pitch lock (e.g. YIN mistaking a fan
    // or electrical hum for a sustained voiced pitch) would otherwise ring
    // as an uninterrupted tone through headphones indefinitely. Real humming
    // naturally breaks for breath well before this; anything held perfectly
    // steady this long is almost certainly not a person humming.
    static constexpr double maxSustainedNoteSeconds = 5.0;
    int64_t sustainedSampleCount = 0; // audio-thread-owned; reset whenever currentNote changes

    // A single noisy analysis window can report a spurious/wrong note; only
    // commit to a note change once the SAME candidate has repeated this many
    // consecutive windows in a row (~2 windows = ~92ms at 2048/44.1kHz).
    static constexpr int stableWindowsRequired = 2;

    // Natural vocal vibrato/wobble around the "true" pitch is normal and
    // shouldn't retrigger a new note every time it crosses a semitone
    // boundary. While a note is already held, a newly detected pitch within
    // this many semitones of the CURRENT note's center frequency is treated
    // as still-the-same-note; only a bigger jump starts a real candidate.
    static constexpr double noteChangeThresholdSemitones = 0.6;

    PitchDetector pitchDetector;
    double sampleRate = 44100.0;

    std::vector<float> analysisBuffer { std::vector<float>((size_t) analysisWindowSize, 0.0f) };
    int bufferFill = 0;

    std::atomic<bool> active { false };
    std::atomic<bool> velocitySensingEnabled { false };
    std::atomic<float> currentLevel { 0.0f };

    int currentNote = -1;    // audio-thread-owned only; the committed, reported note
    int candidateNote = -1;  // audio-thread-owned only; not yet confirmed stable
    int candidateStreak = 0; // consecutive windows candidateNote has repeated
};
