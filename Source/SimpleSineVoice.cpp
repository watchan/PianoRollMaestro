#include "SimpleSineVoice.h"

SimpleSineVoice::SimpleSineVoice()
{
    juce::ADSR::Parameters params;
    params.attack = 0.005f;
    params.decay = 0.05f;
    params.sustain = 0.8f;
    params.release = 0.08f;
    adsr.setParameters(params);
}

bool SimpleSineVoice::canPlaySound(juce::SynthesiserSound* sound)
{
    return dynamic_cast<SimpleSineSound*>(sound) != nullptr;
}

void SimpleSineVoice::startNote(int midiNoteNumber, float velocity, juce::SynthesiserSound*, int currentPitchWheelPosition)
{
    level = velocity;
    currentAngle = 0.0;
    currentNoteNumber = midiNoteNumber;
    updateAngleDelta(currentPitchWheelPosition);

    adsr.setSampleRate(getSampleRate());
    adsr.noteOn();
}

void SimpleSineVoice::stopNote(float, bool allowTailOff)
{
    adsr.noteOff();

    if (!allowTailOff)
    {
        clearCurrentNote();
        currentNoteNumber = -1;
    }
}

void SimpleSineVoice::pitchWheelMoved(int newPitchWheelValue)
{
    if (currentNoteNumber >= 0)
        updateAngleDelta(newPitchWheelValue);
}

void SimpleSineVoice::updateAngleDelta(int pitchWheelValue)
{
    // Standard General MIDI default bend range: +/-2 semitones at the
    // 14-bit extremes (0/16383), 8192 = center/no bend.
    constexpr double bendRangeSemitones = 2.0;
    auto bendSemitones = ((double) pitchWheelValue - 8192.0) / 8192.0 * bendRangeSemitones;
    auto frequency = juce::MidiMessage::getMidiNoteInHertz(currentNoteNumber) * std::pow(2.0, bendSemitones / 12.0);
    angleDelta = juce::MathConstants<double>::twoPi * frequency / getSampleRate();
}

void SimpleSineVoice::renderNextBlock(juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples)
{
    if (angleDelta == 0.0)
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        auto envelope = adsr.getNextSample();
        auto sample = (float) std::sin(currentAngle) * level * envelope;

        for (int ch = 0; ch < outputBuffer.getNumChannels(); ++ch)
            outputBuffer.addSample(ch, startSample + i, sample);

        currentAngle += angleDelta;

        if (!adsr.isActive())
        {
            clearCurrentNote();
            angleDelta = 0.0;
            break;
        }
    }
}
