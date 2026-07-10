#pragma once
#include <JuceHeader.h>
#include <vector>
#include "ProjectModel.h"
#include "SimpleSineVoice.h"

// Sample-accurate scheduler: walks each track's MidiClip, emits note on/off
// MIDI events at exact sample positions, and renders them through that
// track's instrument -- a hosted VST3/AU plugin if one is loaded, otherwise
// a built-in placeholder synth so every track is audible out of the box.
class PlaybackEngine
{
public:
    void prepare(double sampleRateIn, int blockSizeIn);
    void reset();

    void setProject(const Project* projectToPlay);

    void start();
    void stop();
    bool isPlaying() const { return playing; }

    // Direct preview path, bypassing the scheduled clip (PlayMonitor mode).
    // Active regardless of transport play/stop state. Targets one track's
    // instrument (normally whichever track the cursor is on).
    void liveNoteOn(int trackIndex, int noteNumber, float velocity);
    void liveNoteOff(int trackIndex, int noteNumber);
    void liveMidiMessage(int trackIndex, const juce::MidiMessage& message);

    // Loads (or clears, with nullptr) the instrument plugin for one track.
    // A track with no plugin loaded falls back to the built-in synth.
    void setTrackInstrument(int trackIndex, std::unique_ptr<juce::AudioPluginInstance> instrument);
    juce::AudioPluginInstance* getTrackInstrument(int trackIndex);

    // Call once per audio block.
    void renderNextBlock(juce::AudioBuffer<float>& audioOut, juce::MidiBuffer& midiOut, int numSamples);

private:
    struct ScheduledEvent
    {
        int64_t samplePosition;
        int trackIndex;
        int noteNumber;
        bool isNoteOn;
        float velocity;
    };

    struct TrackCursor
    {
        int nextStepIndex = 0;
        int64_t nextStepSample = 0;
    };

    struct TrackAudioState
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin; // nullptr = use fallbackSynth
        juce::Synthesiser fallbackSynth;
        juce::MidiMessageCollector liveMidiCollector; // only used when plugin != nullptr
    };

    void scheduleUpTo(int64_t blockEndSample);
    void ensureTrackAudioStates();
    void initialiseFallbackSynth(juce::Synthesiser& s);

    double sampleRate = 44100.0;
    int blockSize = 512;
    int64_t blockStartSample = 0;
    bool playing = false;

    const Project* project = nullptr;
    std::vector<ScheduledEvent> pendingEvents;
    std::vector<TrackCursor> trackCursors;

    // unique_ptr per element: Synthesiser/MidiMessageCollector aren't move-
    // constructible (they hold a CriticalSection), so the vector can't
    // reallocate TrackAudioState by value -- only move the pointers.
    std::vector<std::unique_ptr<TrackAudioState>> trackAudioStates;
};
