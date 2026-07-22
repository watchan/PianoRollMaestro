#include "PlaybackEngine.h"
#include <algorithm>
#include <cmath>

// MidiMessageCollector timestamps its incoming queue relative to
// Time::getMillisecondCounterHiRes(), same clock real MIDI input messages
// arrive stamped with -- freshly-constructed messages default to 0.0 and
// get scheduled wrong (or dropped) without this.
static juce::MidiMessage withNowTimestamp(juce::MidiMessage message)
{
    message.setTimeStamp(juce::Time::getMillisecondCounterHiRes() * 0.001);
    return message;
}

void PlaybackEngine::initialiseFallbackSynth(juce::Synthesiser& s)
{
    s.clearVoices();
    for (int i = 0; i < 8; ++i)
        s.addVoice(new SimpleSineVoice());

    s.clearSounds();
    s.addSound(new SimpleSineSound());

    s.setCurrentPlaybackSampleRate(sampleRate);
}

void PlaybackEngine::ensureTrackAudioStates()
{
    if (project == nullptr)
        return;

    while (trackAudioStates.size() < project->tracks.size())
    {
        trackAudioStates.push_back(std::make_unique<TrackAudioState>());
        auto& state = *trackAudioStates.back();
        initialiseFallbackSynth(state.fallbackSynth);
        state.liveMidiCollector.reset(sampleRate);
        updateScratchBufferSize(state);

        perTrackMidiBuffers.emplace_back();
    }

    // A track added WHILE already playing (MainEditorComponent::addTrack())
    // needs a TrackCursor too -- scheduleUpTo()'s per-track loop walks
    // project->tracks.size(), not trackCursors.size(), so without this,
    // indexing trackCursors[t] for the newly-added track runs straight past
    // the end of this vector. That's a real out-of-bounds crash (confirmed
    // via a crash log: std::vector<TrackCursor>::operator[] inside
    // scheduleUpTo()), not just a stutter -- it only started actually
    // surfacing once addTrack() stopped calling stop() unconditionally,
    // since stop()-then-Space-to-resume used to always resize this vector
    // via start()'s own trackCursors.assign(), masking the gap. A new
    // cursor starts at step 0, synced to
    // the transport's current sample position, same as any freshly-started
    // track.
    while (trackCursors.size() < project->tracks.size())
        trackCursors.push_back(TrackCursor{ 0, blockStartSample });
}

void PlaybackEngine::updateScratchBufferSize(TrackAudioState& state)
{
    auto channels = state.plugin != nullptr ? juce::jmax(2, state.plugin->getTotalNumOutputChannels()) : 2;
    // avoidReallocating=true: renderNextBlock() calls this same setSize()
    // every block to trim down to that block's actual numSamples (see its
    // own comment) -- as long as it never asks for more than blockSize
    // (established here), that call stays allocation-free.
    state.scratch.setSize(channels, blockSize, false, false, true);
}

void PlaybackEngine::prepare(double sampleRateIn, int blockSizeIn)
{
    sampleRate = sampleRateIn;
    blockSize = blockSizeIn;

    trackAudioStates.reserve(reservedTrackAudioStateCapacity); // see its declaration
    perTrackMidiBuffers.reserve(reservedTrackAudioStateCapacity);
    trackCursors.reserve(reservedTrackAudioStateCapacity); // see ensureTrackAudioStates()'s trackCursors growth

    for (auto& statePtr : trackAudioStates)
    {
        auto& state = *statePtr;
        initialiseFallbackSynth(state.fallbackSynth);
        state.liveMidiCollector.reset(sampleRate);

        if (state.plugin != nullptr)
            state.plugin->prepareToPlay(sampleRate, blockSize);

        updateScratchBufferSize(state); // blockSize may just have changed
    }

    ensureTrackAudioStates();
}

void PlaybackEngine::reset()
{
    blockStartSample = 0;
    nextClickSample = 0;
    clicksSinceStart = 0;
    countingIn = false;
    pendingEvents.clear();
    pendingParameterEvents.clear();
    trackCursors.clear();

    for (auto& statePtr : trackAudioStates)
        statePtr->fallbackSynth.allNotesOff(1, false);
}

void PlaybackEngine::setProject(const Project* projectToPlay)
{
    project = projectToPlay;
    ensureTrackAudioStates();
}

void PlaybackEngine::start(int startStep)
{
    if (project == nullptr)
        return;

    reset();

    if (startStep > 0 && !project->tracks.empty())
    {
        auto& clip = project->tracks[0].clip;
        auto stepSamples = (int64_t) std::round(clip.stepDurationSeconds(project->tempoBpm) * sampleRate);
        blockStartSample = stepSamples * (int64_t) startStep;

        // Snap the click grid to the NEXT quarter-note boundary on-or-after
        // blockStartSample, on the same fixed, absolute (sample-0-anchored)
        // grid every start() call uses -- not just blockStartSample itself,
        // which is usually mid-beat (e.g. resuming from an arbitrary step
        // after a real-time REC commit -- see MainEditorComponent::
        // applyStepEdit()). Without this, restarting playback mid-beat
        // permanently shifted the click grid's phase away from the true
        // beat/bar positions, and clicksSinceStart's accent-every-4th-click
        // pattern reset to the wrong phase too.
        auto quarterNoteSamples = stepSamples * (int64_t) clip.stepsPerQuarterNote;
        if (quarterNoteSamples > 0)
        {
            auto quarterIndex = (blockStartSample + quarterNoteSamples - 1) / quarterNoteSamples; // ceiling division
            nextClickSample = quarterIndex * quarterNoteSamples;
            clicksSinceStart = quarterIndex;
        }
    }

    trackCursors.assign(project->tracks.size(), TrackCursor{ startStep, blockStartSample });
    playing = true;
}

void PlaybackEngine::startWithCountIn(int startStep, int countInBeats)
{
    if (project == nullptr || project->tracks.empty() || countInBeats <= 0)
    {
        start(startStep); // nothing to derive tempo/resolution from, or a trivial 0-beat count-in -- just start normally
        return;
    }

    reset();

    auto& clip = project->tracks[0].clip;
    auto quarterNoteSamples = (int64_t) std::round(clip.stepDurationSeconds(project->tempoBpm) * sampleRate * clip.stepsPerQuarterNote);
    if (quarterNoteSamples <= 0)
    {
        start(startStep);
        return;
    }

    countingIn = true;
    countInSamplePosition = 0;
    countInNextClickSample = 0;
    countInBeatSamples = quarterNoteSamples;
    countInBeatsTotal = countInBeats;
    countInBeatsElapsed = 0;
    countInPendingStartStep = startStep;
}

void PlaybackEngine::stop()
{
    playing = false;
    countingIn = false;

    // Discarding pendingEvents here can drop a note-off that was scheduled
    // for later but whose matching note-on already fired -- that note is
    // still actually sounding. Actually silencing everything is handled by
    // performForceStop(), deferred to the audio thread -- see
    // forceStopRequested's declaration for why this can't happen directly
    // here.
    pendingEvents.clear();
    pendingParameterEvents.clear();

    forceStopRequested = true;
}

void PlaybackEngine::performForceStop()
{
    for (auto& statePtr : trackAudioStates)
    {
        auto& state = *statePtr;
        state.fallbackSynth.allNotesOff(1, true);

        if (state.plugin != nullptr)
        {
            juce::MidiBuffer allNotesOffMidi;

            // An individually-addressed Note Off for every pitch this
            // engine itself still believes is sounding (see
            // TrackAudioState::activeNotePitches' declaration), IN
            // ADDITION to the standards-compliant blanket CC123/CC120
            // below -- some plugins simply don't act on the blanket All-
            // Notes-Off/All-Sound-Off controller messages at all (their
            // own internal voice allocator only ever listens for a
            // genuine per-note Note Off), so the blanket messages alone
            // can silently do nothing against one of those -- a plugin
            // that doesn't honor a bare Note Off can get stuck sounding
            // a note after playback stops.
            for (int p = 0; p < 128; ++p)
                if (state.activeNotePitches[(size_t) p])
                    allNotesOffMidi.addEvent(juce::MidiMessage::noteOff(1, p), 0);

            allNotesOffMidi.addEvent(juce::MidiMessage::allNotesOff(1), 0);
            allNotesOffMidi.addEvent(juce::MidiMessage::allSoundOff(1), 0);

            // Reuse this track's own scratch buffer (already reserved to
            // fit its plugin's channel count/blockSize by
            // updateScratchBufferSize(), message-thread-only) rather than
            // constructing a fresh one -- this now runs on the audio
            // thread (see forceStopRequested's declaration), which must
            // never allocate.
            auto pluginChannels = juce::jmax(2, state.plugin->getTotalNumOutputChannels());
            state.scratch.setSize(pluginChannels, juce::jmax(1, blockSize), false, false, true);
            state.scratch.clear();
            state.plugin->processBlock(state.scratch, allNotesOffMidi);

            state.plugin->reset();
        }

        // See TrackAudioState::activeNotePitches' declaration -- this just
        // silenced every voice directly, bypassing the two dispatch loops
        // in renderNextBlock() that are the only places that tracking is
        // normally kept in sync, so it must be reset here too or it would
        // keep claiming notes are still sounding (and retriggering them on
        // the next CC64=0) long after they were actually silenced.
        state.activeNotePitches.fill(false);
        state.activeNoteCount = 0;
    }
}

void PlaybackEngine::sendAllNotesOffForTrack(int trackIndex)
{
    if (trackIndex < 0 || trackIndex >= (int) trackAudioStates.size())
        return;

    auto& state = *trackAudioStates[(size_t) trackIndex];
    state.fallbackSynth.allNotesOff(1, true);

    if (state.plugin != nullptr)
    {
        juce::MidiBuffer allNotesOffMidi;
        allNotesOffMidi.addEvent(juce::MidiMessage::allNotesOff(1), 0);

        auto pluginChannels = juce::jmax(2, state.plugin->getTotalNumOutputChannels());
        juce::AudioBuffer<float> scratch(pluginChannels, juce::jmax(1, blockSize));
        scratch.clear();
        state.plugin->processBlock(scratch, allNotesOffMidi);
    }

    // See the matching reset in stop() above -- sendAllNotesOffForLoop()
    // (every loop wrap) and retriggerTrack() both funnel through here, so
    // fixing it in this one shared place covers both.
    state.activeNotePitches.fill(false);
    state.activeNoteCount = 0;
}

void PlaybackEngine::sendAllNotesOffForLoop()
{
    for (int t = 0; t < (int) trackAudioStates.size(); ++t)
        sendAllNotesOffForTrack(t);
}

void PlaybackEngine::wrapPlaybackToStep(int startStep)
{
    if (project == nullptr || project->tracks.empty())
        return;

    sendAllNotesOffForLoop();
    pendingEvents.clear();
    pendingParameterEvents.clear();

    auto stepSamples = (int64_t) std::round(project->tracks[0].clip.stepDurationSeconds(project->tempoBpm) * sampleRate);
    blockStartSample = stepSamples * (int64_t) startStep;

    // Same absolute-grid snap as start() -- startStep isn't necessarily a
    // quarter-note multiple, so jumping the click grid straight to
    // blockStartSample would shift its phase away from the true beat/bar
    // positions on every wrap.
    auto quarterNoteSamples = stepSamples * (int64_t) project->tracks[0].clip.stepsPerQuarterNote;
    if (quarterNoteSamples > 0)
    {
        auto quarterIndex = (blockStartSample + quarterNoteSamples - 1) / quarterNoteSamples;
        nextClickSample = quarterIndex * quarterNoteSamples;
        clicksSinceStart = quarterIndex;
    }

    for (auto& cursor : trackCursors)
        cursor = TrackCursor{ startStep, blockStartSample };
}

void PlaybackEngine::renderClickBlip(juce::AudioBuffer<float>& audioOut, int startOffset, int numSamplesAvailable, bool isAccent)
{
    auto clickLengthSamples = juce::jmin(numSamplesAvailable, (int) (0.015 * sampleRate));
    auto freq = isAccent ? 1500.0 : 1000.0;

    for (int i = 0; i < clickLengthSamples; ++i)
    {
        auto t = (double) i / sampleRate;
        auto envelope = std::exp(t * -80.0); // fast decay, short percussive blip
        auto sample = (float) (std::sin(2.0 * juce::MathConstants<double>::pi * freq * t) * envelope * 0.5);

        for (int ch = 0; ch < audioOut.getNumChannels(); ++ch)
            audioOut.addSample(ch, startOffset + i, sample);
    }
}

void PlaybackEngine::renderMetronomeClicks(juce::AudioBuffer<float>& audioOut, int64_t blockEndSample)
{
    if (project == nullptr || !project->metronomeEnabled || project->tracks.empty())
        return;

    // Tempo/resolution is project-wide -- any track's clip works, same
    // convention the loop-wrap block above already uses.
    auto& clip = project->tracks[0].clip;
    auto quarterNoteSamples = (int64_t) std::round(clip.stepDurationSeconds(project->tempoBpm) * sampleRate * clip.stepsPerQuarterNote);
    if (quarterNoteSamples <= 0)
        return;

    while (nextClickSample < blockEndSample)
    {
        if (nextClickSample >= blockStartSample)
        {
            auto isAccent = (clicksSinceStart % 4) == 0; // 4/4 assumed, same as everywhere else in this app
            auto startOffset = (int) (nextClickSample - blockStartSample);
            renderClickBlip(audioOut, startOffset, (int) (blockEndSample - nextClickSample), isAccent);
        }

        nextClickSample += quarterNoteSamples;
        ++clicksSinceStart;
    }
}

void PlaybackEngine::retriggerTrack(int trackIndex)
{
    if (trackIndex < 0 || trackIndex >= (int) trackAudioStates.size())
        return;

    // Same reasoning as sendAllNotesOffForLoop(): about to drop this
    // track's pendingEvents/cursor, so silence it first rather than risk a
    // stuck note whose matching note-off never fires.
    sendAllNotesOffForTrack(trackIndex);

    pendingEvents.erase(
        std::remove_if(pendingEvents.begin(), pendingEvents.end(),
            [trackIndex](const ScheduledEvent& ev) { return ev.trackIndex == trackIndex; }),
        pendingEvents.end());
    pendingParameterEvents.erase(
        std::remove_if(pendingParameterEvents.begin(), pendingParameterEvents.end(),
            [trackIndex](const ScheduledParameterEvent& ev) { return ev.trackIndex == trackIndex; }),
        pendingParameterEvents.end());

    if (trackIndex < (int) trackCursors.size())
        trackCursors[(size_t) trackIndex] = TrackCursor{ 0, blockStartSample };
}

void PlaybackEngine::liveNoteOn(int trackIndex, int noteNumber, float velocity)
{
    ensureTrackAudioStates();
    if (trackIndex < 0 || trackIndex >= (int) trackAudioStates.size())
        return;

    auto& state = *trackAudioStates[(size_t) trackIndex];
    if (state.plugin != nullptr)
        state.liveMidiCollector.addMessageToQueue(withNowTimestamp(juce::MidiMessage::noteOn(1, noteNumber, velocity)));
    else
        state.fallbackSynth.noteOn(1, noteNumber, velocity);
}

void PlaybackEngine::liveNoteOff(int trackIndex, int noteNumber)
{
    ensureTrackAudioStates();
    if (trackIndex < 0 || trackIndex >= (int) trackAudioStates.size())
        return;

    auto& state = *trackAudioStates[(size_t) trackIndex];
    if (state.plugin != nullptr)
        state.liveMidiCollector.addMessageToQueue(withNowTimestamp(juce::MidiMessage::noteOff(1, noteNumber)));
    else
        state.fallbackSynth.noteOff(1, noteNumber, 1.0f, true);
}

void PlaybackEngine::liveMidiMessage(int trackIndex, const juce::MidiMessage& message)
{
    ensureTrackAudioStates();
    if (trackIndex < 0 || trackIndex >= (int) trackAudioStates.size())
        return;

    auto& state = *trackAudioStates[(size_t) trackIndex];

    if (state.plugin != nullptr)
    {
        state.liveMidiCollector.addMessageToQueue(message);
        return;
    }

    auto& synth = state.fallbackSynth;

    if (message.isSustainPedalOn())
        synth.handleSustainPedal(message.getChannel(), true);
    else if (message.isSustainPedalOff())
        synth.handleSustainPedal(message.getChannel(), false);
    else if (message.isSostenutoPedalOn())
        synth.handleSostenutoPedal(message.getChannel(), true);
    else if (message.isSostenutoPedalOff())
        synth.handleSostenutoPedal(message.getChannel(), false);
    else if (message.isController())
        synth.handleController(message.getChannel(), message.getControllerNumber(), message.getControllerValue());
    else if (message.isPitchWheel())
        synth.handlePitchWheel(message.getChannel(), message.getPitchWheelValue());
    else if (message.isAftertouch())
        synth.handleAftertouch(message.getChannel(), message.getNoteNumber(), message.getAfterTouchValue());
    else if (message.isChannelPressure())
        synth.handleChannelPressure(message.getChannel(), message.getChannelPressureValue());
}

void PlaybackEngine::setTrackInstrument(int trackIndex, std::unique_ptr<juce::AudioPluginInstance> instrument)
{
    ensureTrackAudioStates();
    if (trackIndex < 0 || trackIndex >= (int) trackAudioStates.size())
        return;

    auto& state = *trackAudioStates[(size_t) trackIndex];

    if (state.plugin != nullptr)
        state.plugin->releaseResources();

    state.plugin = std::move(instrument);

    if (state.plugin != nullptr)
    {
        // Deliberately leave the plugin's default bus layout alone -- some
        // multi-output instruments (e.g. Komplete Kontrol, 16 output buses
        // for multi-timbral routing) don't support being forced down to a
        // 2-channel bus via setNumberOfChannels(). Instead we render into a
        // scratch buffer sized to whatever the plugin actually wants and
        // mix down to stereo ourselves (see renderNextBlock).
        state.plugin->prepareToPlay(sampleRate, blockSize);
        state.liveMidiCollector.reset(sampleRate);
    }

    updateScratchBufferSize(state); // the new plugin's channel count may differ from the old one's (or from the fallback synth's)
}

juce::AudioPluginInstance* PlaybackEngine::getTrackInstrument(int trackIndex)
{
    if (trackIndex < 0 || trackIndex >= (int) trackAudioStates.size())
        return nullptr;
    return trackAudioStates[(size_t) trackIndex]->plugin.get();
}

int PlaybackEngine::getTrackPlaybackStep(int trackIndex) const
{
    if (!playing || trackIndex < 0 || trackIndex >= (int) trackCursors.size())
        return -1;
    return trackCursors[(size_t) trackIndex].nextStepIndex;
}

int PlaybackEngine::interpolateAutomationValue(const std::vector<AutomationPoint>& points, int stepIndex)
{
    if (points.empty() || stepIndex < points.front().stepIndex)
        return -1;
    if (stepIndex >= points.back().stepIndex)
        return points.back().value;

    auto afterIt = std::upper_bound(points.begin(), points.end(), stepIndex,
        [](int step, const AutomationPoint& p) { return step < p.stepIndex; });
    auto beforeIt = std::prev(afterIt);

    auto span = afterIt->stepIndex - beforeIt->stepIndex;
    if (span <= 0)
        return beforeIt->value;
    auto t = (double) (stepIndex - beforeIt->stepIndex) / (double) span;

    // See AutomationCurveType's declaration -- afterIt's curveType/
    // curveAmount shape the segment ARRIVING at it (the one being
    // interpolated here), not beforeIt's.
    if (afterIt->curveType == AutomationCurveType::Step)
    {
        t = 0.0; // holds beforeIt->value right up until afterIt, then jumps (handled by the >= check above)
    }
    else if (afterIt->curveAmount != 0.0f)
    {
        // curveAmount > 0 = Ease In (slow start, accelerating), < 0 =
        // Ease Out (fast start, decelerating) -- one continuous signed
        // "tension" parameter instead of separate discrete curve types,
        // same convention DAW automation editors (Ableton Live, FL
        // Studio, Cubase, Logic) use for a segment's bend.
        auto exponent = 1.0 + std::abs((double) afterIt->curveAmount) * automationCurveAmountExponentScale;
        t = afterIt->curveAmount > 0.0f ? std::pow(t, exponent) : 1.0 - std::pow(1.0 - t, exponent);
    }

    return beforeIt->value + (int) std::round(t * (double) (afterIt->value - beforeIt->value));
}

float PlaybackEngine::interpolateParameterAutomationValue(const std::vector<ParameterAutomationPoint>& points, int stepIndex)
{
    if (points.empty() || stepIndex < points.front().stepIndex)
        return -1.0f;
    if (stepIndex >= points.back().stepIndex)
        return points.back().value;

    auto afterIt = std::upper_bound(points.begin(), points.end(), stepIndex,
        [](int step, const ParameterAutomationPoint& p) { return step < p.stepIndex; });
    auto beforeIt = std::prev(afterIt);

    auto span = afterIt->stepIndex - beforeIt->stepIndex;
    if (span <= 0)
        return beforeIt->value;
    auto t = (double) (stepIndex - beforeIt->stepIndex) / (double) span;

    // Same curve-on-arrival shaping as interpolateAutomationValue() above.
    if (afterIt->curveType == AutomationCurveType::Step)
    {
        t = 0.0;
    }
    else if (afterIt->curveAmount != 0.0f)
    {
        auto exponent = 1.0 + std::abs((double) afterIt->curveAmount) * automationCurveAmountExponentScale;
        t = afterIt->curveAmount > 0.0f ? std::pow(t, exponent) : 1.0 - std::pow(1.0 - t, exponent);
    }

    return beforeIt->value + (float) (t * (double) (afterIt->value - beforeIt->value));
}

void PlaybackEngine::scheduleUpTo(int64_t blockEndSample)
{
    if (project == nullptr)
        return;

    for (size_t t = 0; t < project->tracks.size(); ++t)
    {
        auto& track = project->tracks[t];
        if (track.playingSlotIndex == -2)
            continue; // explicitly stopped (Session View) -- schedules nothing

        // -1 (default) plays the piano-roll editing buffer, same as before
        // Session View existed; >=0 plays that launched scene slot instead.
        auto& clip = (track.playingSlotIndex >= 0 && track.playingSlotIndex < (int) track.sceneClips.size())
            ? track.sceneClips[(size_t) track.playingSlotIndex]
            : track.clip;
        auto& cursor = trackCursors[t];

        auto stepSamples = (int64_t) std::round(clip.stepDurationSeconds(project->tempoBpm) * sampleRate);
        if (stepSamples <= 0)
            continue;

        // A launched Session View slot (playingSlotIndex >= 0) loops
        // indefinitely, the way every clip launcher's clips do -- that's
        // the whole point of "launching" one. The piano-roll editing
        // buffer (-1) does NOT auto-loop here; its repeat behavior is the
        // separate, explicit global loop region (see the loop-wrap block
        // in renderNextBlock() below), matching pre-Session-View behavior.
        auto loopsForever = track.playingSlotIndex >= 0;

        // The clip's real length -- explicitLengthInSteps if the user set
        // one (see MidiClip's declaration), otherwise clip.steps.size(),
        // exactly matching the old unconditional steps.size() check. When
        // an explicit length runs past the stored steps array, indices in
        // [steps.size(), effectiveLength) are implicit trailing rests (the
        // guard below skips scheduling anything for them).
        auto effectiveLength = clip.effectiveLengthInSteps();

        while (cursor.nextStepSample < blockEndSample)
        {
            if (cursor.nextStepIndex >= effectiveLength)
            {
                if (!loopsForever || effectiveLength <= 0)
                    break; // reached the end -- stop scheduling this track (or nothing to loop at all)
                cursor.nextStepIndex = 0; // wrap back to the start of this clip and keep going
            }

            if (cursor.nextStepIndex < (int) clip.steps.size())
            {
                auto& step = clip.steps[(size_t) cursor.nextStepIndex];

                if (!step.tiedFromPrevious)
                {
                    // The tie-chain envelope -- how far this Step's
                    // continuation steps run. Still the fallback duration
                    // for any note that doesn't carry its own
                    // durationSteps (see below), and the only duration
                    // that exists for hand-entered/old-file notes.
                    int64_t totalSamples = stepSamples * step.lengthInSteps;

                    auto lookahead = cursor.nextStepIndex + 1;
                    while (lookahead < (int) clip.steps.size() && clip.steps[(size_t) lookahead].tiedFromPrevious)
                    {
                        totalSamples += stepSamples * clip.steps[(size_t) lookahead].lengthInSteps;
                        ++lookahead;
                    }

                    // Release slightly early instead of riding all the way to
                    // the container's last sample: a note ending EXACTLY
                    // where the next one starts (e.g. tied to fill a full
                    // bar) would otherwise schedule its note-off at the
                    // identical sample as the following note's note-on, and
                    // some plugins (a strumming guitar instrument was the
                    // one that surfaced this) audibly glitch on a same-
                    // sample note-off/note-on collision instead of cutting
                    // cleanly. A pure fraction of a step turned out too small
                    // to actually be heard/felt at normal tempos -- floored
                    // at ~30ms of wall-clock time
                    // instead, capped to at most half the container's own
                    // length so a very short note can't be trimmed to
                    // nothing.
                    auto minGapSamples = (int64_t) std::round(0.03 * sampleRate);
                    auto releaseGapSamples = juce::jmax(minGapSamples, stepSamples / 4);
                    releaseGapSamples = juce::jmin(releaseGapSamples, totalSamples / 2);

                    for (auto& note : step.notes)
                    {
                        // Each note's own actual sounding length (real-time
                        // recording captures this per note, see
                        // MainEditorComponent::handleMidiNoteChange()) takes
                        // priority over the shared tie-chain envelope, so a
                        // chord's individually-released notes each play for
                        // exactly as long as they were actually held.
                        //
                        // A positive durationSteps is trusted here EXACTLY
                        // as stored, with NO further clamp against
                        // totalSamples (this Step's own tie-chain envelope)
                        // -- that used to also apply here, on the theory
                        // that totalSamples already accounts for a later
                        // note having truncated the envelope, so an
                        // unclamped note could "run past where a completely
                        // different, later-written note already claimed the
                        // same territory". In practice that blanket clamp
                        // fired for ANY later note at all, including a
                        // totally unrelated DIFFERENT pitch played moments
                        // later in an ordinary overlapping/legato passage --
                        // an everyday occurrence, not a conflict -- silently
                        // chopping a properly-recorded ~quarter-note-long
                        // note down to just a few milliseconds (confirmed
                        // via diagnostic log: durationSteps=855
                        // recorded correctly, but the envelope had been
                        // truncated to 21 steps by an unrelated note, and
                        // playback was clamping to that 21-step envelope).
                        // The one real risk this clamp was ever protecting
                        // against -- a genuine SAME-pitch retrigger while
                        // still ringing -- is now instead prevented once, at
                        // commit time on the message thread (see
                        // MainEditorComponent::clampDurationForPitchConflict()),
                        // rather than by re-deriving it here every playback
                        // pass from a container length that was never
                        // designed to double as a scheduling limit.
                        auto rawDurationSamples = note.durationSteps > 0
                            ? stepSamples * (int64_t) note.durationSteps
                            : totalSamples;
                        auto clampedToContainer = note.durationSteps > 0
                            ? rawDurationSamples
                            : juce::jmin(rawDurationSamples, totalSamples);

                        // Only apply the release gap when the note's length
                        // actually reaches the container's edge (whether
                        // because it's a fallback/tied note that always
                        // fills the whole thing, or because a held note got
                        // clamped down to fit) -- a note that legitimately
                        // ends earlier than that shouldn't be trimmed
                        // further, it already isn't touching anything.
                        auto noteDurationSamples = clampedToContainer >= totalSamples
                            ? juce::jmax((int64_t) 1, clampedToContainer - releaseGapSamples)
                            : clampedToContainer;
                        pendingEvents.push_back({ cursor.nextStepSample, (int) t, note.pitch, true, note.velocity });
                        pendingEvents.push_back({ cursor.nextStepSample + noteDurationSamples, (int) t, note.pitch, false, 0.0f });
                    }
                }
            }
            // else: within effectiveLength but past the stored steps array
            // -- an implicit trailing rest, nothing to schedule this step.

            // Sustain pedal (CC64) automation -- see MidiClip::
            // sustainPedalEvents' declaration. Sparse relative to the step
            // count, so a binary search per step (rather than a persistent
            // per-track cursor) is cheap and, unlike a cursor, needs no
            // extra reset bookkeeping on loop-wrap/retrigger -- the same
            // events are simply found again on every pass, exactly like
            // clip.steps[cursor.nextStepIndex]'s notes already are.
            if (!clip.sustainPedalEvents.empty())
            {
                auto eventIt = std::lower_bound(clip.sustainPedalEvents.begin(), clip.sustainPedalEvents.end(),
                    cursor.nextStepIndex,
                    [](const SustainPedalEvent& ev, int step) { return ev.stepIndex < step; });
                if (eventIt != clip.sustainPedalEvents.end() && eventIt->stepIndex == cursor.nextStepIndex)
                {
                    ScheduledEvent ccEvent;
                    ccEvent.samplePosition = cursor.nextStepSample;
                    ccEvent.trackIndex = (int) t;
                    ccEvent.isController = true;
                    ccEvent.controllerNumber = 64;
                    ccEvent.controllerValue = eventIt->pedalDown ? 127 : 0;
                    pendingEvents.push_back(ccEvent);
                }
            }

            // Continuous automation (pitch bend / filter cutoff CC74) --
            // see MidiClip::pitchBendPoints/AutomationPoint's declaration.
            // Unlike CC64's discrete on/off events, these need periodic
            // RE-EMISSION to reconstruct a smooth ramp during playback,
            // not just one message at each recorded breakpoint -- see
            // automationInterpolationStepInterval's declaration.
            if ((!clip.pitchBendPoints.empty() || !clip.filterCutoffPoints.empty())
                && cursor.nextStepIndex % automationInterpolationStepInterval == 0)
            {
                auto pitchBendValue = interpolateAutomationValue(clip.pitchBendPoints, cursor.nextStepIndex);
                if (pitchBendValue >= 0)
                {
                    ScheduledEvent pbEvent;
                    pbEvent.samplePosition = cursor.nextStepSample;
                    pbEvent.trackIndex = (int) t;
                    pbEvent.isPitchWheel = true;
                    pbEvent.pitchWheelValue = pitchBendValue;
                    pendingEvents.push_back(pbEvent);
                }

                auto filterValue = interpolateAutomationValue(clip.filterCutoffPoints, cursor.nextStepIndex);
                if (filterValue >= 0)
                {
                    ScheduledEvent cutoffEvent;
                    cutoffEvent.samplePosition = cursor.nextStepSample;
                    cutoffEvent.trackIndex = (int) t;
                    cutoffEvent.isController = true;
                    cutoffEvent.controllerNumber = 74;
                    cutoffEvent.controllerValue = filterValue;
                    pendingEvents.push_back(cutoffEvent);
                }
            }

            // Host-style plugin-parameter automation -- see
            // MidiClip::parameterLanes' declaration. Same periodic-
            // re-emission reasoning as pitch bend/filter cutoff above,
            // just dispatched through pendingParameterEvents (applied
            // directly to the plugin's own parameter, not as MIDI) instead
            // of pendingEvents.
            if (!clip.parameterLanes.empty() && cursor.nextStepIndex % automationInterpolationStepInterval == 0)
            {
                for (auto& lane : clip.parameterLanes)
                {
                    auto value = interpolateParameterAutomationValue(lane.points, cursor.nextStepIndex);
                    if (value >= 0.0f)
                    {
                        ScheduledParameterEvent paramEvent;
                        paramEvent.samplePosition = cursor.nextStepSample;
                        paramEvent.trackIndex = (int) t;
                        paramEvent.parameterID = lane.parameterID;
                        paramEvent.value = value;
                        pendingParameterEvents.push_back(paramEvent);
                    }
                }
            }

            cursor.nextStepSample += stepSamples;
            ++cursor.nextStepIndex;
        }
    }
}

void PlaybackEngine::renderNextBlock(juce::AudioBuffer<float>& audioOut, juce::MidiBuffer& midiOut, int numSamples)
{
    audioOut.clear();

    if (project == nullptr)
        return;

    ensureTrackAudioStates();

    // See forceStopRequested's declaration -- stop() only raises this flag;
    // the actual plugin-touching work happens here, exclusively on the
    // audio thread, exactly like the plugin's normal processBlock() call
    // later in this same function. exchange(false) both reads and clears
    // it atomically, so a stop() that lands mid-block is picked up on
    // whichever renderNextBlock() call comes next, exactly once.
    if (forceStopRequested.exchange(false))
        performForceStop();

    // Reused every block instead of freshly constructed (see
    // perTrackMidiBuffers' declaration) -- grown/reserved alongside
    // trackAudioStates, so this is always at least trackAudioStates.size()
    // long already; just clear each one this block will actually use.
    for (size_t t = 0; t < trackAudioStates.size(); ++t)
        perTrackMidiBuffers[t].clear();

    // Live preview: always active regardless of play/stop state. Only
    // plugin-backed tracks need buffering here -- fallback-synth live notes
    // were already applied via direct calls in liveNoteOn/Off/liveMidiMessage.
    for (size_t t = 0; t < trackAudioStates.size(); ++t)
    {
        auto& state = *trackAudioStates[t];
        if (state.plugin == nullptr)
            continue;

        juce::MidiBuffer liveMidi;
        state.liveMidiCollector.removeNextBlockOfMessages(liveMidi, numSamples);

        for (const auto meta : liveMidi)
        {
            auto msg = meta.getMessage();
            auto pitch = msg.getNoteNumber();

            // See TrackAudioState::activeNotePitches' declaration.
            if (msg.isNoteOn() && pitch >= 0 && pitch < 128)
            {
                if (!state.activeNotePitches[(size_t) pitch]) { state.activeNotePitches[(size_t) pitch] = true; ++state.activeNoteCount; }
                state.activeNoteVelocities[(size_t) pitch] = msg.getFloatVelocity();
                perTrackMidiBuffers[t].addEvent(msg, meta.samplePosition);
            }
            else if (msg.isNoteOff() && pitch >= 0 && pitch < 128)
            {
                if (state.activeNotePitches[(size_t) pitch]) { state.activeNotePitches[(size_t) pitch] = false; --state.activeNoteCount; }
                perTrackMidiBuffers[t].addEvent(msg, meta.samplePosition);
            }
            else if (msg.isController() && msg.getControllerNumber() == 64 && msg.getControllerValue() == 0)
            {
                perTrackMidiBuffers[t].addEvent(msg, meta.samplePosition);
                // See TrackAudioState::activeNotePitches' declaration --
                // immediately re-trigger anything the buggy plugin just
                // killed that's still genuinely supposed to be sounding.
                for (int p = 0; p < 128; ++p)
                    if (state.activeNotePitches[(size_t) p])
                        perTrackMidiBuffers[t].addEvent(juce::MidiMessage::noteOn(1, p, state.activeNoteVelocities[(size_t) p]), meta.samplePosition);
            }
            else
            {
                perTrackMidiBuffers[t].addEvent(msg, meta.samplePosition);
            }
        }
    }

    if (countingIn)
    {
        // Independent of blockStartSample/the main click grid -- real
        // playback hasn't started yet, so this just counts click-by-click
        // through countInBeatsTotal beats, then hands off to start(), which
        // sets playing=true -- falling straight through into the (now
        // true) `if (playing)` block below in this same call gives a
        // seamless, gap-free transition into real playback the instant the
        // count-in finishes, rather than wasting a block on silence.
        // Always audible (unlike renderMetronomeClicks(), not gated on
        // project->metronomeEnabled) -- otherwise there'd be no way to
        // hear when Real-time REC is actually about to begin.
        while (countInNextClickSample < countInSamplePosition + numSamples && countInBeatsElapsed < countInBeatsTotal)
        {
            auto startOffset = (int) (countInNextClickSample - countInSamplePosition);
            renderClickBlip(audioOut, startOffset, numSamples - startOffset, true);
            countInNextClickSample += countInBeatSamples;
            ++countInBeatsElapsed;
        }
        countInSamplePosition += numSamples;

        // Wait for a full countInBeatsTotal beats' worth of TIME to elapse,
        // not just for the last click to have been emitted -- the previous
        // version started playback the instant the 4th click fired (i.e. at
        // the start of beat 4), so the song's own beat 1 landed right on top
        // of the count-in's 4th click instead of one full beat later, where
        // beat 5 would have been.
        if (countInSamplePosition >= countInBeatsTotal * countInBeatSamples)
            start(countInPendingStartStep);
    }

    auto blockEndSample = blockStartSample;

    if (playing)
    {
        // Loop wrap: checked once per block against where THIS block is
        // about to start (not mid-block), so the wrap point is quantized to
        // the audio block size (~11ms at 512 samples/44.1kHz) rather than
        // sample-accurate -- an acceptable trade-off given grid steps
        // themselves are already much coarser than that, AS LONG AS the
        // wrap target itself is a FIXED sample position computed the same
        // way on every single pass. It used to not be, for the no-marker
        // loop kind: that one waited for allTracksDone && pendingEvents.
        // empty() at the very bottom of this function instead, a condition
        // whose exact timing depends on note content (how long the last
        // notes near the clip's end happen to still be ringing) rather than
        // a fixed point in time -- so the amount of overrun before it
        // actually wrapped varied pass to pass, which read as the beat
        // itself drifting/wobbling rather than a small constant offset.
        // Both loop kinds now compute a fixed wrapSample up front here instead.
        int64_t wrapSample = -1;
        int wrapTargetStep = 0;

        if (!project->tracks.empty())
        {
            auto stepSamples = (int64_t) std::round(project->tracks[0].clip.stepDurationSeconds(project->tempoBpm) * sampleRate);

            if (stepSamples > 0 && project->loopEnabled && project->loopEndStep > project->loopStartStep)
            {
                // Loop Start/End markers are set -- that fixed region takes
                // priority.
                wrapSample = stepSamples * (int64_t) project->loopEndStep;
                wrapTargetStep = project->loopStartStep;
            }
            else if (stepSamples > 0 && project->loopEnabled)
            {
                // No usable markers -- fall back to looping every currently-
                // playing main-clip track together once the LONGEST of them
                // reaches its own end (matches the old allTracksDone
                // semantics, just computed as a fixed sample position
                // instead of discovered after the fact). Session View slots
                // (playingSlotIndex >= 0) already loop forever on their own
                // in scheduleUpTo() and don't gate this; explicitly-stopped
                // tracks (-2) don't either.
                int maxEffectiveLength = 0;
                for (auto& track : project->tracks)
                {
                    if (track.playingSlotIndex == -1)
                        maxEffectiveLength = juce::jmax(maxEffectiveLength, track.clip.effectiveLengthInSteps());
                }

                if (maxEffectiveLength > 0)
                    wrapSample = stepSamples * (int64_t) maxEffectiveLength;
            }
        }

        if (wrapSample > 0 && blockStartSample >= wrapSample)
            wrapPlaybackToStep(wrapTargetStep);

        blockEndSample = blockStartSample + numSamples;
        scheduleUpTo(blockEndSample);

        std::stable_sort(pendingEvents.begin(), pendingEvents.end(),
                          [](const ScheduledEvent& a, const ScheduledEvent& b) { return a.samplePosition < b.samplePosition; });

        size_t eventsConsumed = 0;

        for (auto& ev : pendingEvents)
        {
            if (ev.samplePosition >= blockEndSample)
                break;

            auto localOffset = juce::jlimit(0, numSamples - 1, (int) (ev.samplePosition - blockStartSample));
            auto& state = *trackAudioStates[(size_t) ev.trackIndex];

            auto msg = ev.isPitchWheel ? juce::MidiMessage::pitchWheel(1, ev.pitchWheelValue)
                     : ev.isController ? juce::MidiMessage::controllerEvent(1, ev.controllerNumber, ev.controllerValue)
                     : ev.isNoteOn      ? juce::MidiMessage::noteOn(1, ev.noteNumber, ev.velocity)
                                        : juce::MidiMessage::noteOff(1, ev.noteNumber);

            if (!ev.isController && !ev.isPitchWheel && ev.noteNumber >= 0 && ev.noteNumber < 128)
            {
                if (ev.isNoteOn)
                {
                    if (!state.activeNotePitches[(size_t) ev.noteNumber]) { state.activeNotePitches[(size_t) ev.noteNumber] = true; ++state.activeNoteCount; }
                    state.activeNoteVelocities[(size_t) ev.noteNumber] = ev.velocity;
                }
                else if (state.activeNotePitches[(size_t) ev.noteNumber])
                {
                    state.activeNotePitches[(size_t) ev.noteNumber] = false;
                    --state.activeNoteCount;
                }
            }

            midiOut.addEvent(msg, localOffset);
            perTrackMidiBuffers[(size_t) ev.trackIndex].addEvent(msg, localOffset);
            ++eventsConsumed;

            // See TrackAudioState::activeNotePitches' declaration --
            // immediately re-trigger anything the buggy plugin just killed
            // that's still genuinely supposed to be sounding.
            if (ev.isController && ev.controllerNumber == 64 && ev.controllerValue == 0)
            {
                for (int p = 0; p < 128; ++p)
                {
                    if (state.activeNotePitches[(size_t) p])
                    {
                        auto retrigger = juce::MidiMessage::noteOn(1, p, state.activeNoteVelocities[(size_t) p]);
                        midiOut.addEvent(retrigger, localOffset);
                        perTrackMidiBuffers[(size_t) ev.trackIndex].addEvent(retrigger, localOffset);
                    }
                }
            }
        }

        pendingEvents.erase(pendingEvents.begin(), pendingEvents.begin() + (long) eventsConsumed);

        // Host-style plugin-parameter automation -- see
        // ScheduledParameterEvent's declaration. Applied directly via
        // AudioProcessorParameter::setValueNotifyingHost() rather than
        // through midiOut/perTrackMidiBuffers above, so this deliberately
        // doesn't touch either of those. This is the exact callback path
        // MainEditorComponent's own AudioProcessorListener watches (see
        // its audioProcessorParameterChanged() override) -- it ignores
        // changes that arrive off the message thread specifically so this
        // playback-driven call (audio thread) is never mistaken for a
        // user physically touching the control and re-recorded.
        std::stable_sort(pendingParameterEvents.begin(), pendingParameterEvents.end(),
                          [](const ScheduledParameterEvent& a, const ScheduledParameterEvent& b) { return a.samplePosition < b.samplePosition; });

        size_t parameterEventsConsumed = 0;
        for (auto& ev : pendingParameterEvents)
        {
            if (ev.samplePosition >= blockEndSample)
                break;
            ++parameterEventsConsumed;

            if (ev.trackIndex < 0 || ev.trackIndex >= (int) trackAudioStates.size())
                continue;
            auto& state = *trackAudioStates[(size_t) ev.trackIndex];
            if (state.plugin == nullptr)
                continue;

            for (auto* param : state.plugin->getParameters())
            {
                auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*>(param);
                if (hosted != nullptr && hosted->getParameterID() == ev.parameterID)
                {
                    param->setValueNotifyingHost(ev.value);
                    break;
                }
            }
        }
        pendingParameterEvents.erase(pendingParameterEvents.begin(), pendingParameterEvents.begin() + (long) parameterEventsConsumed);
    }

    // Flat per-track attenuation before summing -- multiple tracks (or even
    // one loud plugin) adding up unattenuated clips easily. Not a real
    // per-track mixer (no user control yet, see backlog), just headroom.
    constexpr float perTrackGain = 0.6f;

    for (size_t t = 0; t < trackAudioStates.size(); ++t)
    {
        auto& state = *trackAudioStates[t];

        // state.scratch is reused every block (see its declaration) --
        // already sized to (channels, blockSize) by whichever message-
        // thread call last touched this track's plugin/blockSize
        // (updateScratchBufferSize()), so trimming down to this block's
        // actual numSamples here is a fast, allocation-free adjustment
        // (avoidReallocating=true, and numSamples never exceeds blockSize)
        // -- NOT a real reallocation on the audio thread.
        auto channels = state.plugin != nullptr ? juce::jmax(2, state.plugin->getTotalNumOutputChannels()) : 2;
        state.scratch.setSize(channels, numSamples, false, false, true);
        state.scratch.clear();

        if (state.plugin != nullptr)
        {
            state.plugin->processBlock(state.scratch, perTrackMidiBuffers[t]);

            for (int ch = 0; ch < audioOut.getNumChannels(); ++ch)
                audioOut.addFrom(ch, 0, state.scratch, juce::jmin(ch, state.scratch.getNumChannels() - 1), 0, numSamples, perTrackGain);
        }
        else
        {
            state.fallbackSynth.renderNextBlock(state.scratch, perTrackMidiBuffers[t], 0, numSamples);

            for (int ch = 0; ch < audioOut.getNumChannels(); ++ch)
                audioOut.addFrom(ch, 0, state.scratch, juce::jmin(ch, state.scratch.getNumChannels() - 1), 0, numSamples, perTrackGain);
        }
    }

    if (playing)
        renderMetronomeClicks(audioOut, blockEndSample);

    // Master safety net: soft-clip (tanh) rather than hard digital clipping,
    // in case several tracks still sum above 0dBFS despite the attenuation
    // above -- smooths the overload instead of harsh crackling distortion.
    for (int ch = 0; ch < audioOut.getNumChannels(); ++ch)
    {
        auto* data = audioOut.getWritePointer(ch);
        for (int i = 0; i < numSamples; ++i)
            data[i] = std::tanh(data[i]);
    }

    if (playing)
    {
        blockStartSample = blockEndSample;

        // A track running out of steps shouldn't stop playback while
        // looping -- its cursor just sits at clip.steps.size() (scheduleUpTo
        // stops advancing it) until the wrap check at the top of the next
        // call resets it back to the loop start. Both loop kinds (marker
        // region or the no-marker per-clip fallback, see project->
        // loopEnabled's declaration) are now wrapped by that same top-of-
        // function check, so anything with the loop switch on at all skips
        // the auto-stop below.
        bool loopingActive = project->loopEnabled;

        if (!loopingActive)
        {
            bool allTracksDone = true;
            for (size_t t = 0; t < trackCursors.size(); ++t)
            {
                auto& track = project->tracks[t];

                if (track.playingSlotIndex >= 0 && track.playingSlotIndex < (int) track.sceneClips.size())
                {
                    // A launched Session View slot loops forever (see
                    // scheduleUpTo()) -- as long as it actually has content,
                    // it never counts as "done" the way a linear main-clip
                    // playthrough does, or the transport would auto-stop
                    // out from under an actively-looping clip.
                    if (track.sceneClips[(size_t) track.playingSlotIndex].effectiveLengthInSteps() > 0)
                        allTracksDone = false;
                    continue;
                }

                if (track.playingSlotIndex == -2)
                    continue; // explicitly stopped -- contributes nothing either way

                if (trackCursors[t].nextStepIndex < track.clip.effectiveLengthInSteps())
                    allTracksDone = false;
            }

            // loopingActive is false here, so the loop switch is fully off
            // -- nothing left to do but stop once everything's finished.
            if (allTracksDone && pendingEvents.empty())
                playing = false;
        }
    }
}
