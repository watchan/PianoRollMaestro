#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>
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

    // startStep: which step every track's cursor (and the transport's
    // sample clock) begins at -- 0 (default) is the clip's very start,
    // matching this method's original behavior exactly. Session View slots
    // still loop from their own start regardless of startStep (this only
    // affects where the transport itself begins counting from).
    void start(int startStep = 0);
    // Real-time REC pre-roll: clicks countInBeats quarter-note beats
    // (always audible, regardless of project->metronomeEnabled -- otherwise
    // there'd be no way to hear when recording is actually about to start),
    // then calls start(startStep) automatically once the count-in finishes.
    // isPlaying() stays false throughout the count-in (nothing is scheduled
    // yet) -- isCountingIn() reports it instead.
    void startWithCountIn(int startStep, int countInBeats = 4);
    void stop();
    bool isPlaying() const { return playing; }
    bool isCountingIn() const { return countingIn; }
    // The step start()/scheduling will actually begin at once the count-in
    // finishes -- lets a caller treat a note played DURING the count-in
    // (anticipating beat 1, before isPlaying() ever goes true) as if it
    // landed exactly there, instead of not being real-time-capturable at
    // all. Only meaningful while
    // isCountingIn() is true.
    int getCountInTargetStep() const { return countInPendingStartStep; }

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

    // Call from the message thread right after growing project->tracks
    // (see MainEditorComponent::addTrack()) -- pre-builds this new track's
    // TrackAudioState (a juce::Synthesiser + 8 voices + a
    // MidiMessageCollector, see the struct below) here instead of leaving
    // it for renderNextBlock()'s own ensureTrackAudioStates() call to build
    // on the audio thread the next time it runs. That allocation happening
    // ON the audio thread was a real, separate stutter source that
    // persisted even after project.tracks itself stopped being a hazard
    // (see reservedTrackCapacity's declaration).
    void prepareTrackAudioStates() { ensureTrackAudioStates(); }

    // Call once per audio block.
    void renderNextBlock(juce::AudioBuffer<float>& audioOut, juce::MidiBuffer& midiOut, int numSamples);

    // renderNextBlock() reads project->tracks (a raw pointer set once via
    // setProject(), see its declaration) from the audio thread with no
    // synchronization of its own. Growing that vector (push_back(), e.g.
    // MainEditorComponent::addTrack()) can reallocate its storage -- if that
    // happens while renderNextBlock() is mid-iteration over the old memory,
    // it's a use-after-free/data race (this used to crash when Add Track
    // was pressed during playback). A juce::CriticalSection shared with the
    // message thread was tried first to make that safe, held for this
    // function's entire body -- but even briefly locking OUT the audio
    // thread on every single block turned out to itself cause audible
    // stutter the moment a track was actually added during playback.
    // Replaced with a much cheaper approach instead: MainEditorComponent keeps project.tracks
    // reserve()'d well beyond any realistic track count (see its
    // reservedTrackCapacity), so addTrack()'s push_back() essentially never
    // reallocates in the first place -- no lock needed here at all.
    //
    // That still wasn't the whole story -- the stutter persisted even after
    // this fix. The remaining cause: this function used to unconditionally heap-allocate
    // every single block regardless of whether a track had just been added
    // (a fresh std::vector<juce::MidiBuffer>, plus a fresh scratch
    // juce::AudioBuffer per track) -- real-time audio code is never
    // supposed to call malloc/free in its own hot path at all, because the
    // OS heap allocator's internal lock can be held by some OTHER,
    // non-real-time thread (e.g. the message thread doing its own,
    // unrelated allocation right as a track gets added) that then gets
    // preempted before releasing it, stalling this audio thread for
    // unbounded time waiting on the same lock (priority inversion) --
    // ordinarily rare enough to go unnoticed, but reliably audible right
    // when addTrack() itself does a burst of allocation (building the new
    // track's Synthesiser + 8 voices) at the exact same moment. Every
    // buffer this function touches is now a reused member
    // (perTrackMidiBuffers, TrackAudioState::scratch) sized ahead of time
    // from the message thread only (ensureTrackAudioStates()/prepare()/
    // setTrackInstrument()) -- this function itself never allocates,
    // regardless of what any other thread happens to be doing at the same
    // moment.

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

        // A recorded sustain-pedal (CC64) automation event -- see
        // MidiClip::SustainPedalEvent's declaration -- instead of a
        // note-on/off. noteNumber/isNoteOn/velocity above are unused when
        // this is true; controllerNumber/controllerValue are unused
        // otherwise.
        bool isController = false;
        int controllerNumber = 0;
        int controllerValue = 0;

        // An interpolated sample from a pitch-bend automation lane -- see
        // MidiClip::pitchBendPoints/AutomationPoint's declaration. Pitch
        // bend is its own MIDI message type (14-bit), not a controller
        // event, so it needs its own flag/value pair rather than reusing
        // isController above.
        bool isPitchWheel = false;
        int pitchWheelValue = 8192;
    };

    struct TrackCursor
    {
        int nextStepIndex = 0;
        int64_t nextStepSample = 0;
    };

    // An interpolated sample from a MidiClip::parameterLanes lane -- see
    // its declaration. Deliberately NOT folded into ScheduledEvent above
    // (which is MIDI-message-shaped): this applies directly to the
    // plugin's own AudioProcessorParameter via setValueNotifyingHost(),
    // bypassing MIDI entirely, the same mechanism real DAW host
    // automation uses.
    struct ScheduledParameterEvent
    {
        int64_t samplePosition;
        int trackIndex;
        juce::String parameterID;
        float value;
    };

    struct TrackAudioState
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin; // nullptr = use fallbackSynth
        juce::Synthesiser fallbackSynth;
        juce::MidiMessageCollector liveMidiCollector; // only used when plugin != nullptr

        // Render scratch space for this track's own block -- reused every
        // call instead of being freshly constructed each time (see
        // updateScratchBufferSize()'s comment for why that mattered).
        juce::AudioBuffer<float> scratch;

        // Pitches currently sounding on THIS track's plugin/synth, kept up
        // to date from BOTH renderNextBlock() dispatch loops below (the
        // live-monitoring drain and the scheduled/recorded event loop) --
        // the only two ways a note actually starts or stops sounding here,
        // regardless of which one committed it.
        //
        // Exists to work around a confirmed-buggy loaded instrument
        // (Kontakt via Komplete Kontrol) that kills ANY currently-sounding
        // note the instant it receives a real CC64=0, including notes that
        // never got an explicit note-off at all -- e.g. a note still being
        // physically/genuinely held mid-attack right when the pedal
        // happens to lift.
        //
        // Several increasingly complicated attempts to DEFER the CC64=0
        // itself until nothing was "at risk" all failed for the same
        // underlying reason: in continuous/overlapping playing, SOMETHING
        // is active at almost every instant, so deferral could starve the
        // real transition from ever reaching the plugin at all -- and once
        // it also had to hold back a LATER on-transition behind it (so the
        // plugin still saw both, in order), an unrelated still-ringing
        // note could end up blocking an EARLIER, already-released note's
        // legitimate cutoff indefinitely, well past any new pedal press.
        //
        // Fixed instead by NOT deferring anything: every CC64 transition
        // (both directions) is now forwarded immediately, exactly as
        // recorded/played, so the plugin's own internal sustain state
        // always stays correctly in sync and every note's release timing
        // matches what was actually played. The one remaining risk this
        // creates -- a note genuinely still sounding right at the instant
        // CC64=0 arrives DOES get killed by the buggy plugin -- is
        // countered by immediately re-triggering (a fresh, same-instant
        // note-on) every pitch still marked active here right after
        // sending that CC64=0, using activeNoteVelocities below. Bounded
        // to, at worst, a same-sample-position retrigger click on whatever
        // few notes are genuinely mid-attack at that exact moment, instead
        // of unboundedly delaying or losing an entire transition.
        std::array<bool, 128> activeNotePitches {};
        std::array<float, 128> activeNoteVelocities {};
        int activeNoteCount = 0;
    };

    void scheduleUpTo(int64_t blockEndSample);
    // Linearly interpolates points (an already stepIndex-sorted automation
    // lane -- see MidiClip::AutomationPoint's declaration) at stepIndex, or
    // returns -1 if stepIndex falls before the first recorded point
    // (meaning this lane isn't automated yet at this position, so nothing
    // should be sent at all). Holds the last point's value for anything at
    // or past it, matching how a real pedal/wheel just stays wherever it
    // was last left.
    static int interpolateAutomationValue(const std::vector<AutomationPoint>& points, int stepIndex);
    // Same as interpolateAutomationValue() above, just for a
    // ParameterAutomationLane's float (0.0-1.0) points instead -- returns
    // -1.0f (out of the valid 0.0-1.0 range, so unambiguous) as the "not
    // automated yet at this position" sentinel.
    static float interpolateParameterAutomationValue(const std::vector<ParameterAutomationPoint>& points, int stepIndex);
    // Re-emitted every this-many steps (not just once per recorded
    // breakpoint) so a continuous automation lane reconstructs as a smooth
    // ramp during playback instead of jumping discretely -- 24 steps is
    // roughly 10-20ms at typical tempos, given stepsPerQuarterNote's 960
    // resolution, dense enough to sound continuous without flooding the
    // MIDI stream. Approximate (not sample-accurate to each breakpoint's
    // own step), same trade-off as everywhere else timing is quantized to
    // the audio block/step grid in this engine.
    static constexpr int automationInterpolationStepInterval = 24;
    // Set by stop() (message thread) -- actually applied from inside
    // renderNextBlock() (audio thread) at the top of its very next call,
    // instead of being carried out directly by stop() itself.
    // AudioPluginInstance::processBlock()/reset() follow the same one-
    // thread-at-a-time contract as the VST3/AU host spec (see
    // renderNextBlock()'s own extensive comments on audio-thread rules) --
    // calling them straight from stop() would race whatever in-flight
    // renderNextBlock() call is concurrently touching that very same
    // plugin instance, and that race is exactly the kind of thing that
    // can make a "reliable" All-Notes-Off/All-Sound-Off silently fail to
    // land, leaving a note stuck sounding forever on a plugin that
    // doesn't otherwise handle Note Off cleanly.
    std::atomic<bool> forceStopRequested { false };
    // Only ever called from renderNextBlock() -- see forceStopRequested's
    // declaration for why this can't just run inline in stop().
    void performForceStop();
    void ensureTrackAudioStates();
    void initialiseFallbackSynth(juce::Synthesiser& s);
    // Sizes state.scratch to whatever renderNextBlock() will need to hand
    // this track's plugin/synth this block -- called only from the message
    // thread (ensureTrackAudioStates() when a state is first built,
    // prepare() when blockSize changes, setTrackInstrument() when a
    // plugin's channel count changes), NEVER from renderNextBlock() itself.
    // renderNextBlock() only ever calls scratch.setSize(..., true) with a
    // size already within what was reserved here, which JUCE guarantees is
    // allocation-free -- see renderNextBlock()'s own comment on why the
    // audio thread must never actually allocate.
    void updateScratchBufferSize(TrackAudioState& state);

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
    // Shared by both loop-wrap paths (Loop Start/End markers, and the
    // clip-end fallback when no markers are set -- see renderNextBlock()):
    // silences every track, drops stale pendingEvents, jumps blockStartSample
    // and every track's TrackCursor to startStep, and resyncs the click grid
    // to the same absolute quarter-note phase startStep implies (startStep
    // isn't necessarily a quarter-note multiple, so naively jumping the
    // click grid straight to the new blockStartSample would shift its phase
    // away from the true beat/bar positions on every wrap).
    void wrapPlaybackToStep(int startStep);

    // Synthesizes metronome clicks (project->metronomeEnabled) directly into
    // audioOut for [blockStartSample, blockEndSample) -- accented (higher
    // pitch) on the downbeat of each 4/4 bar. A simple decaying sine blip,
    // fully rendered within whatever block it starts in (clicks are short
    // enough, ~15ms, that block-boundary truncation is inaudible in
    // practice) rather than tracked as cross-block state like note
    // scheduling is. nextClickSample/clicksSinceStart reset alongside
    // blockStartSample/trackCursors (reset(), and the loop-wrap block in
    // renderNextBlock()) so the downbeat accent stays aligned with wherever
    // the transport actually starts/loops from.
    void renderMetronomeClicks(juce::AudioBuffer<float>& audioOut, int64_t blockEndSample);
    // Shared by renderMetronomeClicks() and the count-in click sequence
    // (startWithCountIn()) -- a short decaying-sine blip written into
    // audioOut starting at sample startOffset, truncated to whatever fits
    // in numSamplesAvailable (a click running past the end of its block is
    // simply cut short, same trade-off renderMetronomeClicks() already made
    // -- clicks are short enough, ~15ms, that this is inaudible in practice).
    void renderClickBlip(juce::AudioBuffer<float>& audioOut, int startOffset, int numSamplesAvailable, bool isAccent);

    double sampleRate = 44100.0;
    int blockSize = 512;
    int64_t blockStartSample = 0;
    bool playing = false;

    int64_t nextClickSample = 0;
    int64_t clicksSinceStart = 0;

    // Real-time REC count-in state -- see startWithCountIn(). Independent
    // of blockStartSample/trackCursors (real playback hasn't started yet),
    // so it doesn't interact with note scheduling or the main click grid
    // at all until it hands off to start() once finished.
    bool countingIn = false;
    int64_t countInSamplePosition = 0;
    int64_t countInNextClickSample = 0;
    int64_t countInBeatSamples = 0;
    int countInBeatsTotal = 0;
    int countInBeatsElapsed = 0;
    int countInPendingStartStep = 0;

    const Project* project = nullptr;
    std::vector<ScheduledEvent> pendingEvents;
    std::vector<ScheduledParameterEvent> pendingParameterEvents;
    std::vector<TrackCursor> trackCursors;

    // unique_ptr per element: Synthesiser/MidiMessageCollector aren't move-
    // constructible (they hold a CriticalSection), so the vector can't
    // reallocate TrackAudioState by value -- only move the pointers. Each
    // element's pointee (the actual TrackAudioState) never moves once
    // allocated regardless of vector growth, but the vector's OWN internal
    // array-of-pointers can still be reallocated/freed by a push_back() --
    // reserve()'d in prepare() to the same headroom as MainEditorComponent
    // ::reservedTrackCapacity for the same reason project.tracks is, so
    // ensureTrackAudioStates() growing this from prepareTrackAudioStates()
    // (message thread) can never race a renderNextBlock() (audio thread)
    // that's mid-iteration over it.
    static constexpr size_t reservedTrackAudioStateCapacity = 64;
    std::vector<std::unique_ptr<TrackAudioState>> trackAudioStates;

    // One reused MidiBuffer per track, parallel to trackAudioStates --
    // renderNextBlock() used to construct a fresh
    // std::vector<juce::MidiBuffer> from scratch every single block
    // (allocating) regardless of whether any track had just been added;
    // grown/reserved alongside trackAudioStates instead (same capacity, same
    // message-thread-only growth points) and just .clear()'d (allocation-
    // free) each block now. See renderNextBlock()'s comment.
    std::vector<juce::MidiBuffer> perTrackMidiBuffers;
};
