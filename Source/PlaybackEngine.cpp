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
    pendingEvents.clear();

    for (auto& statePtr : trackAudioStates)
    {
        auto& state = *statePtr;
        state.fallbackSynth.allNotesOff(1, true);
        if (state.plugin != nullptr)
            state.plugin->reset();
    }
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

void PlaybackEngine::scheduleUpTo(int64_t blockEndSample)
{
    if (project == nullptr)
        return;

    for (size_t t = 0; t < project->tracks.size(); ++t)
    {
        auto& clip = project->tracks[t].clip;
        auto& cursor = trackCursors[t];

        auto stepSamples = (int64_t) std::round(clip.stepDurationSeconds(project->tempoBpm) * sampleRate);
        if (stepSamples <= 0)
            continue;

        while (cursor.nextStepIndex < (int) clip.steps.size() && cursor.nextStepSample < blockEndSample)
        {
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
                audioOut.addFrom(ch, 0, pluginScratch, juce::jmin(ch, pluginScratch.getNumChannels() - 1), 0, numSamples);
        }
        else
        {
            auto synthChannels = juce::jmax(2, audioOut.getNumChannels());
            juce::AudioBuffer<float> synthScratch(synthChannels, numSamples);
            synthScratch.clear();
            state.fallbackSynth.renderNextBlock(synthScratch, perTrackMidi[t], 0, numSamples);

            for (int ch = 0; ch < audioOut.getNumChannels(); ++ch)
                audioOut.addFrom(ch, 0, synthScratch, juce::jmin(ch, synthScratch.getNumChannels() - 1), 0, numSamples);
        }
    }

    if (playing)
    {
        blockStartSample = blockEndSample;

        bool allTracksDone = true;
        for (size_t t = 0; t < trackCursors.size(); ++t)
            if (trackCursors[t].nextStepIndex < (int) project->tracks[t].clip.steps.size())
                allTracksDone = false;

        if (allTracksDone && pendingEvents.empty())
            playing = false;
    }
}
