#include "PlaybackEngine.h"

void PlaybackEngine::prepare(double sampleRateIn)
{
    sampleRate = sampleRateIn;

    synth.clearVoices();
    for (int i = 0; i < 8; ++i)
        synth.addVoice(new SimpleSineVoice());

    synth.clearSounds();
    synth.addSound(new SimpleSineSound());

    synth.setCurrentPlaybackSampleRate(sampleRate);
}

void PlaybackEngine::reset()
{
    blockStartSample = 0;
    pendingEvents.clear();
    trackCursors.clear();
    synth.allNotesOff(1, false);
}

void PlaybackEngine::setProject(const Project* projectToPlay)
{
    project = projectToPlay;
}

void PlaybackEngine::start()
{
    if (project == nullptr)
        return;

    reset();
    trackCursors.assign(project->tracks.size(), TrackCursor{});
    playing = true;
}

void PlaybackEngine::liveNoteOn(int noteNumber, float velocity)
{
    synth.noteOn(1, noteNumber, velocity);
}

void PlaybackEngine::liveNoteOff(int noteNumber)
{
    synth.noteOff(1, noteNumber, 1.0f, true);
}

void PlaybackEngine::stop()
{
    playing = false;
    pendingEvents.clear();
    synth.allNotesOff(1, true);
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

    if (!playing || project == nullptr)
        return;

    auto blockEndSample = blockStartSample + numSamples;
    scheduleUpTo(blockEndSample);

    std::stable_sort(pendingEvents.begin(), pendingEvents.end(),
                      [](const ScheduledEvent& a, const ScheduledEvent& b) { return a.samplePosition < b.samplePosition; });

    juce::MidiBuffer synthMidi;
    size_t eventsConsumed = 0;

    for (auto& ev : pendingEvents)
    {
        if (ev.samplePosition >= blockEndSample)
            break;

        auto localOffset = juce::jlimit(0, numSamples - 1, (int) (ev.samplePosition - blockStartSample));
        auto msg = ev.isNoteOn ? juce::MidiMessage::noteOn(1, ev.noteNumber, ev.velocity)
                                : juce::MidiMessage::noteOff(1, ev.noteNumber);

        midiOut.addEvent(msg, localOffset);
        synthMidi.addEvent(msg, localOffset);
        ++eventsConsumed;
    }

    pendingEvents.erase(pendingEvents.begin(), pendingEvents.begin() + (long) eventsConsumed);

    synth.renderNextBlock(audioOut, synthMidi, 0, numSamples);

    blockStartSample = blockEndSample;

    bool allTracksDone = true;
    for (size_t t = 0; t < trackCursors.size(); ++t)
        if (trackCursors[t].nextStepIndex < (int) project->tracks[t].clip.steps.size())
            allTracksDone = false;

    if (allTracksDone && pendingEvents.empty())
        playing = false;
}
