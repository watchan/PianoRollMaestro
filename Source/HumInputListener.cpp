#include "HumInputListener.h"

void HumInputListener::setActive(bool shouldBeActive)
{
    bool wasActive = active.exchange(shouldBeActive);

    if (wasActive && !shouldBeActive)
    {
        bufferFill = 0;
        forceNoteOff();
    }
}

void HumInputListener::forceNoteOff()
{
    candidateNote = -1;
    candidateStreak = 0;

    if (currentNote < 0)
        return;

    auto oldNote = currentNote;
    currentNote = -1;
    sustainedSampleCount = 0;

    juce::MessageManager::callAsync([this, oldNote]
    {
        if (onNoteChange)
            onNoteChange(oldNote, 0.0f, false);
    });
}

void HumInputListener::setVelocitySensingEnabled(bool shouldBeEnabled)
{
    velocitySensingEnabled.store(shouldBeEnabled);
}

void HumInputListener::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    sampleRate = device->getCurrentSampleRate();
    bufferFill = 0;
    currentNote = -1;
    candidateNote = -1;
    candidateStreak = 0;

    auto activeChannels = device->getActiveInputChannels();
    auto channelNames = device->getInputChannelNames();
    juce::StringArray activeNames;
    for (int i = 0; i < channelNames.size(); ++i)
        if (activeChannels[i])
            activeNames.add(channelNames[i] + " (index " + juce::String(i) + ")");

    DBG("HumInputListener: audio device \"" << device->getName() << "\", active input channel(s): "
        << (activeNames.isEmpty() ? "NONE" : activeNames.joinIntoString(", ")));
}

void HumInputListener::audioDeviceStopped()
{
    bufferFill = 0;
    currentNote = -1;
}

void HumInputListener::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                          int numInputChannels,
                                                          float* const* /*outputChannelData*/,
                                                          int /*numOutputChannels*/,
                                                          int numSamples,
                                                          const juce::AudioIODeviceCallbackContext&)
{
    // Deliberately never touches outputChannelData -- this callback is a
    // pure input listener registered alongside AudioAppComponent's own
    // output-writing callback on the same AudioDeviceManager.
    if (numInputChannels == 0 || inputChannelData[0] == nullptr)
    {
        currentLevel.store(0.0f);
        return;
    }

    auto* in = inputChannelData[0];

    // Peak level for the UI meter -- computed unconditionally (regardless of
    // active()) so the user can confirm mic/channel selection is receiving
    // signal even before toggling hum listening on.
    float peak = 0.0f;
    for (int i = 0; i < numSamples; ++i)
        peak = juce::jmax(peak, std::abs(in[i]));
    currentLevel.store(peak);

    if (!active.load())
        return;

    int sourceOffset = 0;
    int samplesRemaining = numSamples;

    while (samplesRemaining > 0)
    {
        int spaceLeft = analysisWindowSize - bufferFill;
        int samplesToCopy = juce::jmin(spaceLeft, samplesRemaining);

        std::copy(in + sourceOffset, in + sourceOffset + samplesToCopy, analysisBuffer.begin() + bufferFill);

        bufferFill += samplesToCopy;
        sourceOffset += samplesToCopy;
        samplesRemaining -= samplesToCopy;

        if (bufferFill >= analysisWindowSize)
        {
            processAnalysisWindow();
            bufferFill = 0;
        }
    }
}

void HumInputListener::processAnalysisWindow()
{
    if (currentNote >= 0)
    {
        sustainedSampleCount += analysisWindowSize;

        if ((double) sustainedSampleCount / sampleRate >= maxSustainedNoteSeconds)
        {
            // A note held perfectly steady this long is almost certainly a
            // false-positive pitch lock (fan/electrical hum), not a person
            // humming -- cut it and wait for a fresh attack.
            forceNoteOff();
            return;
        }
    }

    auto result = pitchDetector.detectPitch(analysisBuffer.data(), analysisWindowSize, sampleRate);

    if (!result.voiced)
    {
        int detectedNote = -1;

        if (detectedNote == candidateNote)
            ++candidateStreak;
        else
        {
            candidateNote = detectedNote;
            candidateStreak = 1;
        }

        if (candidateStreak >= stableWindowsRequired && candidateNote != currentNote)
            setCurrentNote(candidateNote);
        return;
    }

    // Vibrato guard: while a note is already held, absorb small wobble
    // around ITS center frequency instead of treating every semitone
    // crossing as a new note. Only a bigger jump starts a real candidate.
    if (currentNote >= 0)
    {
        auto currentNoteFreq = 440.0 * std::pow(2.0, (currentNote - 69) / 12.0);
        auto semitoneDistance = 12.0 * std::log2(result.frequencyHz / currentNoteFreq);

        if (std::abs(semitoneDistance) < noteChangeThresholdSemitones)
        {
            candidateNote = currentNote;
            candidateStreak = stableWindowsRequired;
            return;
        }
    }

    auto noteNumber = (int) std::round(69.0 + 12.0 * std::log2(result.frequencyHz / 440.0));
    auto detectedNote = juce::jlimit(0, 127, noteNumber);

    // Hysteresis: a single noisy window can report a wrong/spurious note;
    // only commit once the same candidate has repeated for
    // stableWindowsRequired consecutive windows.
    if (detectedNote == candidateNote)
        ++candidateStreak;
    else
    {
        candidateNote = detectedNote;
        candidateStreak = 1;
    }

    if (candidateStreak >= stableWindowsRequired && candidateNote != currentNote)
        setCurrentNote(candidateNote);
}

void HumInputListener::setCurrentNote(int newNote)
{
    if (newNote == currentNote)
        return;

    auto oldNote = currentNote;
    currentNote = newNote;
    sustainedSampleCount = 0;

    // Velocity sensing (RMS-based) is not implemented in v1 -- the user
    // explicitly asked for a fixed default velocity instead, with this flag
    // reserved as a future opt-in. See setVelocitySensingEnabled().
    auto velocity = defaultVelocity;

    juce::MessageManager::callAsync([this, oldNote, newNote, velocity]
    {
        if (oldNote >= 0 && onNoteChange)
            onNoteChange(oldNote, 0.0f, false);

        if (newNote >= 0 && onNoteChange)
            onNoteChange(newNote, velocity, true);
    });
}
