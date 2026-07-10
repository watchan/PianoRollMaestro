#pragma once
#include <JuceHeader.h>
#include <vector>
#include "ProjectModel.h"
#include "SimpleSineVoice.h"

// Sample-accurate scheduler: walks each track's MidiClip, emits note on/off
// MIDI events at exact sample positions, and renders them through a built-in
// placeholder synth so the clip is audible without hosting a real instrument.
class PlaybackEngine
{
public:
    void prepare(double sampleRateIn);
    void reset();

    void setProject(const Project* projectToPlay);

    void start();
    void stop();
    bool isPlaying() const { return playing; }

    // Direct-to-synth preview path, bypassing the scheduled clip (PlayMonitor mode).
    void liveNoteOn(int noteNumber, float velocity);
    void liveNoteOff(int noteNumber);

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

    void scheduleUpTo(int64_t blockEndSample);

    double sampleRate = 44100.0;
    int64_t blockStartSample = 0;
    bool playing = false;

    const Project* project = nullptr;
    std::vector<ScheduledEvent> pendingEvents;
    std::vector<TrackCursor> trackCursors;

    juce::Synthesiser synth;
};
