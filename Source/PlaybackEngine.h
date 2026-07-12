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

    // Current playback position, in samples from the start of the clip --
    // read from the message thread by a UI Timer to draw a playhead locator.
    // Not sample-accurate by the time it's read (blockStartSample updates on
    // the audio thread), but plenty precise for a ~30Hz visual indicator.
    // This is a single GLOBAL clock shared by every track, so it only wraps
    // correctly for the global loop region (renderNextBlock() resets
    // blockStartSample itself when that wraps) -- it does NOT reflect a
    // Session View clip looping on its own, since each track's scene slot
    // can be a different length and loop independently of the others. Use
    // getTrackPlaybackStep() instead for a step position that's correct
    // for whatever one specific track is actually playing.
    int64_t getPlaybackPositionSamples() const { return blockStartSample; }

    // The step index trackIndex's own TrackCursor last scheduled from --
    // correct even while that track is looping a launched Session View
    // clip independently of the transport's global sample clock (see
    // getPlaybackPositionSamples()'s comment). -1 while stopped or for an
    // out-of-range track.
    int getTrackPlaybackStep(int trackIndex) const;

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

    // Session View clip launching: call AFTER mutating
    // project->tracks[trackIndex].playingSlotIndex (PlaybackEngine only
    // holds a const Project*, so it can't make that change itself) to pick
    // up the new source immediately. Silences whatever was sounding on
    // just this track, drops its stale pendingEvents, and resets its
    // scheduling cursor to the very start of the new source, synced to the
    // current transport position -- every other track's playback is
    // completely unaffected.
    void retriggerTrack(int trackIndex);

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

    // Sends MIDI note-off for every currently-sounding note on one track,
    // without touching plugin effect tails or resetting plugin state
    // (unlike stop(), which additionally sends All-Sound-Off and calls
    // plugin->reset()) -- a reverb/delay tail bleeding across a loop/clip-
    // launch seam is often musically desirable, but a note that was still
    // held when pendingEvents got cleared should not get stuck on.
    void sendAllNotesOffForTrack(int trackIndex);
    // sendAllNotesOffForTrack() across every track -- used when wrapping
    // playback back to the loop start.
    void sendAllNotesOffForLoop();

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
