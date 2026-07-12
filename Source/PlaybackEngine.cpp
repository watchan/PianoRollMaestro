#include "PlaybackEngine.h"

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
    }
}

void PlaybackEngine::prepare(double sampleRateIn, int blockSizeIn)
{
    sampleRate = sampleRateIn;
    blockSize = blockSizeIn;

    for (auto& statePtr : trackAudioStates)
    {
        auto& state = *statePtr;
        initialiseFallbackSynth(state.fallbackSynth);
        state.liveMidiCollector.reset(sampleRate);

        if (state.plugin != nullptr)
            state.plugin->prepareToPlay(sampleRate, blockSize);
    }

    ensureTrackAudioStates();
}

void PlaybackEngine::reset()
{
    blockStartSample = 0;
    pendingEvents.clear();
    trackCursors.clear();

    for (auto& statePtr : trackAudioStates)
        statePtr->fallbackSynth.allNotesOff(1, false);
}

void PlaybackEngine::setProject(const Project* projectToPlay)
{
    project = projectToPlay;
    ensureTrackAudioStates();
}

void PlaybackEngine::start()
{
    if (project == nullptr)
        return;

    reset();
    trackCursors.assign(project->tracks.size(), TrackCursor{});
    playing = true;
}

void PlaybackEngine::stop()
{
    playing = false;

    // Discarding pendingEvents here can drop a note-off that was scheduled
    // for later but whose matching note-on already fired -- that note is
    // still actually sounding. juce::Synthesiser::allNotesOff() below
    // reliably handles that for the fallback synth, but a hosted plugin's
    // reset() is not guaranteed to silence currently-held voices (many
    // plugins treat it as clearing internal buffers/tails, not "kill all
    // notes"). Explicitly feeding an MIDI All-Notes-Off/All-Sound-Off
    // through the plugin's normal processBlock() path is the standards-
    // compliant way to make sure it actually stops, before reset() clears
    // any remaining internal state.
    pendingEvents.clear();

    for (auto& statePtr : trackAudioStates)
    {
        auto& state = *statePtr;
        state.fallbackSynth.allNotesOff(1, true);

        if (state.plugin != nullptr)
        {
            juce::MidiBuffer allNotesOffMidi;
            allNotesOffMidi.addEvent(juce::MidiMessage::allNotesOff(1), 0);
            allNotesOffMidi.addEvent(juce::MidiMessage::allSoundOff(1), 1);

            auto pluginChannels = juce::jmax(2, state.plugin->getTotalNumOutputChannels());
            juce::AudioBuffer<float> scratch(pluginChannels, juce::jmax(1, blockSize));
            scratch.clear();
            state.plugin->processBlock(scratch, allNotesOffMidi);

            state.plugin->reset();
        }
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
}

void PlaybackEngine::sendAllNotesOffForLoop()
{
    for (int t = 0; t < (int) trackAudioStates.size(); ++t)
        sendAllNotesOffForTrack(t);
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

        while (cursor.nextStepSample < blockEndSample)
        {
            if (cursor.nextStepIndex >= (int) clip.steps.size())
            {
                if (!loopsForever || clip.steps.empty())
                    break; // reached the end -- stop scheduling this track (or nothing to loop at all)
                cursor.nextStepIndex = 0; // wrap back to the start of this clip and keep going
            }

            auto& step = clip.steps[(size_t) cursor.nextStepIndex];

            if (!step.tiedFromPrevious)
            {
                int64_t totalSamples = stepSamples * step.lengthInSteps;

                auto lookahead = cursor.nextStepIndex + 1;
                while (lookahead < (int) clip.steps.size() && clip.steps[(size_t) lookahead].tiedFromPrevious)
                {
                    totalSamples += stepSamples * clip.steps[(size_t) lookahead].lengthInSteps;
                    ++lookahead;
                }

                for (auto& note : step.notes)
                {
                    pendingEvents.push_back({ cursor.nextStepSample, (int) t, note.pitch, true, note.velocity });
                    pendingEvents.push_back({ cursor.nextStepSample + totalSamples, (int) t, note.pitch, false, 0.0f });
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

    std::vector<juce::MidiBuffer> perTrackMidi(trackAudioStates.size());

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
            perTrackMidi[t].addEvent(meta.getMessage(), meta.samplePosition);
    }

    auto blockEndSample = blockStartSample;

    if (playing)
    {
        // Loop wrap: checked once per block against where THIS block is
        // about to start (not mid-block), so the wrap point is quantized to
        // the audio block size (~11ms at 512 samples/44.1kHz) rather than
        // sample-accurate -- an acceptable trade-off given grid steps
        // themselves are already much coarser than that. If the previous
        // block's end already reached or passed the loop end, jump the
        // transport back to the loop start and reset every track's
        // scheduling cursor to match before scheduling this block.
        if (project->loopEnabled && project->loopEndStep > project->loopStartStep && !project->tracks.empty())
        {
            auto stepSamples = (int64_t) std::round(project->tracks[0].clip.stepDurationSeconds(project->tempoBpm) * sampleRate);
            auto loopEndSample = stepSamples * (int64_t) project->loopEndStep;

            if (stepSamples > 0 && blockStartSample >= loopEndSample)
            {
                sendAllNotesOffForLoop();
                pendingEvents.clear();
                blockStartSample = stepSamples * (int64_t) project->loopStartStep;

                for (auto& cursor : trackCursors)
                    cursor = TrackCursor{ project->loopStartStep, blockStartSample };
            }
        }

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
            auto msg = ev.isNoteOn ? juce::MidiMessage::noteOn(1, ev.noteNumber, ev.velocity)
                                    : juce::MidiMessage::noteOff(1, ev.noteNumber);

            midiOut.addEvent(msg, localOffset);
            perTrackMidi[(size_t) ev.trackIndex].addEvent(msg, localOffset);
            ++eventsConsumed;
        }

        pendingEvents.erase(pendingEvents.begin(), pendingEvents.begin() + (long) eventsConsumed);
    }

    // Flat per-track attenuation before summing -- multiple tracks (or even
    // one loud plugin) adding up unattenuated clips easily. Not a real
    // per-track mixer (no user control yet, see backlog), just headroom.
    constexpr float perTrackGain = 0.6f;

    for (size_t t = 0; t < trackAudioStates.size(); ++t)
    {
        auto& state = *trackAudioStates[t];

        if (state.plugin != nullptr)
        {
            auto pluginChannels = juce::jmax(2, state.plugin->getTotalNumOutputChannels());
            juce::AudioBuffer<float> pluginScratch(pluginChannels, numSamples);
            pluginScratch.clear();
            state.plugin->processBlock(pluginScratch, perTrackMidi[t]);

            for (int ch = 0; ch < audioOut.getNumChannels(); ++ch)
                audioOut.addFrom(ch, 0, pluginScratch, juce::jmin(ch, pluginScratch.getNumChannels() - 1), 0, numSamples, perTrackGain);
        }
        else
        {
            auto synthChannels = juce::jmax(2, audioOut.getNumChannels());
            juce::AudioBuffer<float> synthScratch(synthChannels, numSamples);
            synthScratch.clear();
            state.fallbackSynth.renderNextBlock(synthScratch, perTrackMidi[t], 0, numSamples);

            for (int ch = 0; ch < audioOut.getNumChannels(); ++ch)
                audioOut.addFrom(ch, 0, synthScratch, juce::jmin(ch, synthScratch.getNumChannels() - 1), 0, numSamples, perTrackGain);
        }
    }

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
        // call resets it back to the loop start.
        bool loopingActive = project->loopEnabled && project->loopEndStep > project->loopStartStep;

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
                    if (!track.sceneClips[(size_t) track.playingSlotIndex].steps.empty())
                        allTracksDone = false;
                    continue;
                }

                if (track.playingSlotIndex == -2)
                    continue; // explicitly stopped -- contributes nothing either way

                if (trackCursors[t].nextStepIndex < (int) track.clip.steps.size())
                    allTracksDone = false;
            }

            if (allTracksDone && pendingEvents.empty())
                playing = false;
        }
    }
}
